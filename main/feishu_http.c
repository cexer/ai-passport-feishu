#include "feishu_http.h"
#include "feishu_http_poll.h"
#include "feishu_api_error.h"

#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "feishu_store.h"
#include "feishu_oauth_stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "feishu_http";
static esp_http_client_handle_t s_stream_client;
static feishu_http_progress_t s_progress;
static void *s_progress_context;
static TaskHandle_t s_progress_owner;
static bool s_read_preemptible;
void feishu_http_set_progress(feishu_http_progress_t progress,void *context)
{s_progress_owner=xTaskGetCurrentTaskHandle();s_progress_context=context;s_progress=progress;s_read_preemptible=false;}
void feishu_http_set_read_preemptible(bool enabled)
{if(s_progress_owner==xTaskGetCurrentTaskHandle())s_read_preemptible=enabled;}
bool feishu_http_progress(void)
{return !s_progress || s_progress_owner!=xTaskGetCurrentTaskHandle() || s_progress(s_progress_context);}


/* Read-only requests under the assistant owner must be cancellable during
 * TLS establishment as well as response parsing. The SDK preserves handshake
 * and header-write state across ESP_ERR_HTTP_EAGAIN. No request is replayed. */
static bool polled_read_owner(void)
{return s_read_preemptible && s_progress && s_progress_owner==xTaskGetCurrentTaskHandle();}
static esp_err_t open_read_poll(esp_http_client_handle_t client,feishu_http_cancel_t cancel,void *context)
{
    int64_t deadline=esp_timer_get_time()+30000000;
    for(;;){
        if(!feishu_http_progress() || (cancel && cancel(context)))return FEISHU_HTTP_ERR_CANCELLED;
        if(esp_timer_get_time()>=deadline){
            int fd=esp_http_client_get_socket(client),tls_error=0,tls_flags=0;
            struct sockaddr_storage peer;socklen_t size=sizeof(peer);
            bool tcp_connected=fd>=0 && getpeername(fd,(struct sockaddr *)&peer,&size)==0;
            esp_err_t last=esp_http_client_get_and_clear_last_tls_error(client,&tls_error,&tls_flags);
            ESP_LOGW(TAG,"HTTP_OPEN_TIMEOUT socket=%d tcp_connected=%u tls_result=%d tls_error=%d flags=%d",fd,(unsigned)tcp_connected,last,tls_error,tls_flags);
            return ESP_ERR_TIMEOUT;
        }
        esp_err_t err=esp_http_client_open(client,0);
        if(err!=ESP_ERR_HTTP_EAGAIN)return err;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static esp_err_t fetch_headers_poll(esp_http_client_handle_t client,int64_t deadline,
                                     feishu_http_cancel_t cancel,void *context,int *status)
{
    int seen=0;bool errno_wait_logged=false;
    for(;;) {
        if(!feishu_http_progress() || (cancel && cancel(context)))return FEISHU_HTTP_ERR_CANCELLED;
        if(esp_timer_get_time()>deadline)return ESP_ERR_TIMEOUT;
        int64_t fetched=esp_http_client_fetch_headers(client);
        int socket_errno=errno;
        int current=esp_http_client_get_status_code(client);if(current>0)seen=current;
        if(feishu_http_headers_pending(fetched,socket_errno,ESP_ERR_HTTP_EAGAIN)){
            if(fetched!=-ESP_ERR_HTTP_EAGAIN && !errno_wait_logged){
                errno_wait_logged=true;ESP_LOGI(TAG,"HTTP_HEADER_WAIT errno_retry=1 error=%d status=%d",socket_errno,seen);
            }
            vTaskDelay(pdMS_TO_TICKS(10));continue;
        }
        if(fetched<0){ESP_LOGW(TAG,"HTTP_HEADER_FAILED result=%lld error=%d status=%d",(long long)fetched,socket_errno,seen);return ESP_ERR_HTTP_FETCH_HEADER;}
        *status=seen;return seen>0 ? ESP_OK:ESP_ERR_INVALID_RESPONSE;
    }
}
typedef struct {
    esp_http_client_handle_t client;
    size_t written,limit;
    esp_err_t error;
    int64_t deadline;
    feishu_http_cancel_t cancel;
    void *cancel_context;
} upload_t;
static bool upload_bytes(void *context,const void *data,size_t size)
{
    upload_t *p=context;
    if(size>p->limit-p->written) {p->error=ESP_ERR_INVALID_SIZE;return false;}
    const uint8_t *bytes=data;
    while(size) {
        if(p->cancel && p->cancel(p->cancel_context)) {p->error=FEISHU_HTTP_ERR_CANCELLED;return false;}
        if(esp_timer_get_time()>p->deadline) {p->error=ESP_ERR_TIMEOUT;return false;}
        int n=(int)(size>1024 ? 1024:size);
        int written=esp_http_client_write(p->client,(const char *)bytes,n);
        if(written<=0 || written>n) {
            ESP_LOGW(TAG,"upload write failed result=%d errno=%d written=%u total=%u",
                written,errno,(unsigned)p->written,(unsigned)p->limit);
            p->error=ESP_ERR_HTTP_WRITE_DATA;return false;
        }
        p->written+=(size_t)written;bytes+=written;size-=(size_t)written;
    }
    return true;
}
static esp_err_t post_stream(const char *url,const char *token,const feishu_http_header_t *headers,size_t header_count,size_t body_size,
                                   feishu_http_upload_t upload,void *upload_context,bool keepalive,
                                   size_t response_limit,feishu_http_sink_t sink,void *sink_context,
                                   feishu_http_cancel_t cancel,void *cancel_context,
                                   int *status_code,size_t *response_size)
{
    if(!url || (!token && (!headers || !header_count)) || !upload || !sink || !status_code || !response_size ||
       body_size>INT_MAX || !response_limit || (token && strlen(token)>INT_MAX-512))return ESP_ERR_INVALID_ARG;
    size_t tx=token ? strlen(token)+512:1024;
    for(size_t i=0;i<header_count;++i) {
        if(!headers[i].name || !headers[i].value || strchr(headers[i].value,'\r') || strchr(headers[i].value,'\n'))return ESP_ERR_INVALID_ARG;
        size_t n=strlen(headers[i].name)+strlen(headers[i].value)+4;
        if(n>8192 || tx>8192-n)return ESP_ERR_INVALID_SIZE;
        tx+=n;
    }
    *status_code=0;*response_size=0;
    esp_http_client_handle_t client=keepalive ? s_stream_client:NULL;
    if(!client) {
        esp_http_client_config_t config={.url=url,.method=HTTP_METHOD_POST,
            .crt_bundle_attach=esp_crt_bundle_attach,.timeout_ms=20000,
            .buffer_size=2048,.buffer_size_tx=(int)tx,
            .keep_alive_enable=keepalive,.disable_auto_redirect=true};
        client=esp_http_client_init(&config);
        if(!client)return ESP_ERR_NO_MEM;
        if(keepalive)s_stream_client=client;
    }
    esp_err_t err=ESP_OK;
    if(token) {
        size_t auth_size=strlen(token)+8;
        char *authorization=malloc(auth_size);
        if(!authorization){err=ESP_ERR_NO_MEM;goto done;}
        snprintf(authorization,auth_size,"Bearer %s",token);
        err=esp_http_client_set_header(client,"Authorization",authorization);
        memset(authorization,0,auth_size);free(authorization);
        if(err==ESP_OK)err=esp_http_client_set_header(client,"Accept","application/json");
        if(err==ESP_OK)err=esp_http_client_set_header(client,"Content-Type","application/json; charset=utf-8");
    }
    for(size_t i=0;err==ESP_OK && i<header_count;++i)
        err=esp_http_client_set_header(client,headers[i].name,headers[i].value);
    if(err==ESP_OK) {
        esp_http_client_set_timeout_ms(client,20000);
        if(cancel && cancel(cancel_context))err=FEISHU_HTTP_ERR_CANCELLED;
        else err=esp_http_client_open(client,(int)body_size);
    }
    int64_t deadline=esp_timer_get_time()+120000000;
    upload_t writer={.client=client,.limit=body_size,.error=ESP_OK,.deadline=deadline,
        .cancel=cancel,.cancel_context=cancel_context};
    if(err==ESP_OK && !upload(upload_context,upload_bytes,&writer))err=writer.error==ESP_OK ? ESP_FAIL:writer.error;
    if(err==ESP_OK && writer.written!=body_size)err=ESP_ERR_INVALID_SIZE;
    if(err==ESP_OK) {
        esp_http_client_set_timeout_ms(client,1000);
        err=fetch_headers_poll(client,deadline,cancel,cancel_context,status_code);
    }
    uint8_t block[1024];unsigned chunks=0;
    while(err==ESP_OK) {
        if(!feishu_http_progress()){err=FEISHU_HTTP_ERR_CANCELLED;break;}
        if(cancel && cancel(cancel_context)) {err=FEISHU_HTTP_ERR_CANCELLED;break;}
        if(esp_timer_get_time()>deadline) {err=ESP_ERR_TIMEOUT;break;}
        int got=esp_http_client_read(client,(char *)block,sizeof(block));
        if(got==-ESP_ERR_HTTP_EAGAIN)continue;
        if(got<0) {err=ESP_FAIL;break;}
        if(!got) {
            if(!esp_http_client_is_complete_data_received(client))err=ESP_ERR_INVALID_RESPONSE;
            break;
        }
        if((size_t)got>response_limit-*response_size) {err=ESP_ERR_INVALID_SIZE;break;}
        *response_size+=(size_t)got;
        err=sink(sink_context,block,(size_t)got);
        if(++chunks%8==0)vTaskDelay(1);
    }
    if(esp_http_client_get_status_code(client)>0)*status_code=esp_http_client_get_status_code(client);
done:
    if(!keepalive)esp_http_client_cleanup(client);
    else if(err!=ESP_OK)feishu_http_stream_close();
    return err;
}
esp_err_t feishu_http_post_stream(const char *url,const char *token,size_t body_size,
    feishu_http_upload_t upload,void *upload_context,bool keepalive,size_t response_limit,
    feishu_http_sink_t sink,void *sink_context,feishu_http_cancel_t cancel,void *cancel_context,
    int *status_code,size_t *response_size)
{ return post_stream(url,token,NULL,0,body_size,upload,upload_context,keepalive,response_limit,sink,sink_context,cancel,cancel_context,status_code,response_size); }
esp_err_t feishu_http_post_headers_stream(const char *url,const feishu_http_header_t *headers,size_t count,
    size_t body_size,feishu_http_upload_t upload,void *upload_context,bool keepalive,size_t response_limit,
    feishu_http_sink_t sink,void *sink_context,feishu_http_cancel_t cancel,void *cancel_context,
    int *status_code,size_t *response_size)
{ return post_stream(url,NULL,headers,count,body_size,upload,upload_context,keepalive,response_limit,sink,sink_context,cancel,cancel_context,status_code,response_size); }

typedef struct response_chunk {
    struct response_chunk *next;
    size_t length;
    char data[];
} response_chunk_t;

typedef struct {
    esp_http_client_handle_t client;
    size_t limit,*length;
    int64_t deadline;
    esp_err_t error;
    feishu_http_cancel_t cancel;
    void *context;
    unsigned chunks;
} pull_t;
static int pull_read(void *context,void *data,size_t capacity)
{
    pull_t *p=context;
    if(!data || !capacity)return -1;
    if(capacity>1024)capacity=1024;
    for(;;) {
        if(p->cancel && p->cancel(p->context)) {p->error=FEISHU_HTTP_ERR_CANCELLED;return -1;}
        if(esp_timer_get_time()>p->deadline) {p->error=ESP_ERR_TIMEOUT;return -1;}
        int got=esp_http_client_read(p->client,data,(int)capacity);
        if(got==-ESP_ERR_HTTP_EAGAIN)continue;
        if(got<0) {p->error=ESP_FAIL;return -1;}
        if(!got && !esp_http_client_is_complete_data_received(p->client)) {p->error=ESP_ERR_INVALID_RESPONSE;return -1;}
        if((size_t)got>p->limit-*p->length) {p->error=ESP_ERR_INVALID_SIZE;return -1;}
        *p->length+=(size_t)got;
        if(++p->chunks%8==0)vTaskDelay(1);
        return got;
    }
}
esp_err_t feishu_http_get_consume(const char *url,const char *token,size_t limit,
                                   feishu_http_consumer_t consume,void *context,
                                   feishu_http_cancel_t cancel,void *cancel_context,
                                   int *status,size_t *length)
{
    if(!url || !token || !*token || !consume || !status || !length || !limit || strlen(token)>=FEISHU_TOKEN_MAX)
        return ESP_ERR_INVALID_ARG;
    *status=0;*length=0;
    if(cancel && cancel(cancel_context))return FEISHU_HTTP_ERR_CANCELLED;
    esp_http_client_config_t cfg={.url=url,.method=HTTP_METHOD_GET,.crt_bundle_attach=esp_crt_bundle_attach,
        .timeout_ms=polled_read_owner() ? 1000:20000,.is_async=polled_read_owner(),.buffer_size=2048,.buffer_size_tx=(int)strlen(token)+512,.disable_auto_redirect=true};
#if CONFIG_MBEDTLS_DYNAMIC_BUFFER
    /* Reserve RX once after handshake, before response parsing can fragment
     * its hole. The short-lived GET client releases it at cleanup. WSS and
     * POST retain their existing policies. This is a public IDF strategy. */
    cfg.tls_dyn_buf_strategy=HTTP_TLS_DYN_BUF_RX_STATIC;
    ESP_LOGI(TAG,"HTTP_RX_STATIC max_content=%u",(unsigned)CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN);
#endif
    esp_http_client_handle_t client=esp_http_client_init(&cfg);if(!client)return ESP_ERR_NO_MEM;
    if(esp_http_client_defer_get_tx_buffer(client)!=ESP_OK){esp_http_client_cleanup(client);return ESP_ERR_INVALID_STATE;}
    size_t auth_size=strlen(token)+8;char *auth=malloc(auth_size);esp_err_t err=ESP_ERR_NO_MEM;
    if(!auth)goto done;
    snprintf(auth,auth_size,"Bearer %s",token);err=esp_http_client_set_header(client,"Authorization",auth);
    memset(auth,0,auth_size);free(auth);
    if(err==ESP_OK)err=esp_http_client_set_header(client,"Accept-Encoding","identity");
    if(err==ESP_OK)err=open_read_poll(client,cancel,cancel_context);
    if(err==ESP_OK)esp_http_client_delete_header(client,"Authorization");
    int64_t deadline=esp_timer_get_time()+120000000;
    if(err==ESP_OK) {
        esp_http_client_set_timeout_ms(client,1000);
        err=fetch_headers_poll(client,deadline,cancel,cancel_context,status);
    }
    if(err==ESP_OK && (*status<200 || *status>=300))err=ESP_FAIL;
    int64_t expected=esp_http_client_get_content_length(client);
    if(err==ESP_OK && expected>0 && (uint64_t)expected>limit)err=ESP_ERR_INVALID_SIZE;
    if(err==ESP_OK) {
        pull_t reader={.client=client,.limit=limit,.length=length,.deadline=deadline,
            .error=ESP_OK,.cancel=cancel,.context=cancel_context};
        err=consume(context,pull_read,&reader,expected);
        if(reader.error!=ESP_OK)err=reader.error;
        if(err==ESP_OK && !esp_http_client_is_complete_data_received(client))err=ESP_ERR_INVALID_RESPONSE;
    }
done:
    if(esp_http_client_get_status_code(client)>0)*status=esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);return err;
}

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
    size_t limit;
    esp_err_t error;
    bool segmented;
    response_chunk_t *head;
    response_chunk_t *tail;
} response_buffer_t;

typedef struct {
    FILE *file;
    size_t length;
    size_t limit;
    esp_err_t error;
} download_file_t;

static esp_err_t download_event(esp_http_client_event_t *event)
{
    download_file_t *download = event->user_data;
    if (event->event_id != HTTP_EVENT_ON_DATA || download->error != ESP_OK) {
        return download->error;
    }
    size_t length = (size_t)event->data_len;
    if (download->length > download->limit ||
        length > download->limit - download->length) {
        download->error = ESP_ERR_INVALID_SIZE;
    } else if (length > 0 &&
               fwrite(event->data, 1, length, download->file) != length) {
        download->error = ESP_FAIL;
    } else {
        download->length += length;
    }
    return download->error;
}

static esp_err_t append_response(response_buffer_t *buffer, const char *data,
                                 size_t length)
{
    size_t required;
    size_t capacity;
    char *next;

    if (length == 0) return ESP_OK;
    if (buffer->length > buffer->limit || length > buffer->limit - buffer->length) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (buffer->segmented) {
        response_chunk_t *chunk = malloc(sizeof(*chunk) + length);
        if (chunk == NULL) return ESP_ERR_NO_MEM;
        chunk->next = NULL;
        chunk->length = length;
        memcpy(chunk->data, data, length);
        if (buffer->tail == NULL) buffer->head = chunk;
        else buffer->tail->next = chunk;
        buffer->tail = chunk;
        buffer->length += length;
        return ESP_OK;
    }
    required = buffer->length + length + 1;
    if (required > buffer->capacity) {
        capacity = buffer->capacity == 0 ? 1024 : buffer->capacity;
        while (capacity < required && capacity < buffer->limit + 1) {
            capacity *= 2;
        }
        if (capacity > buffer->limit + 1) capacity = buffer->limit + 1;
        if (capacity < required) return ESP_ERR_INVALID_SIZE;
        next = realloc(buffer->data, capacity);
        if (next == NULL) return ESP_ERR_NO_MEM;
        buffer->data = next;
        buffer->capacity = capacity;
    }
    memcpy(buffer->data + buffer->length, data, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
    return ESP_OK;
}

static void free_response_chunks(response_buffer_t *buffer)
{
    response_chunk_t *chunk = buffer->head;
    while (chunk != NULL) {
        response_chunk_t *next = chunk->next;
        free(chunk);
        chunk = next;
    }
    buffer->head = NULL;
    buffer->tail = NULL;
}

static esp_err_t join_response_chunks(response_buffer_t *buffer)
{
    char *joined = malloc(buffer->length + 1);
    size_t offset = 0;
    if (joined == NULL) return ESP_ERR_NO_MEM;
    for (response_chunk_t *chunk = buffer->head; chunk != NULL;
         chunk = chunk->next) {
        memcpy(joined + offset, chunk->data, chunk->length);
        offset += chunk->length;
    }
    joined[offset] = '\0';
    buffer->data = joined;
    buffer->capacity = buffer->length + 1;
    return ESP_OK;
}

static esp_err_t response_sink(void *context,const uint8_t *data,size_t size)
{ return append_response(context,(const char *)data,size); }

esp_err_t feishu_http_get_response(const char *url,const char *token,size_t limit,
    feishu_http_cancel_t cancel,void *context,feishu_http_response_t *response)
{
    if(!response || !limit || limit>SIZE_MAX-1)return ESP_ERR_INVALID_ARG;
    memset(response,0,sizeof(*response));
    response_buffer_t buffer={.limit=limit,.segmented=true};
    size_t received=0;
    esp_err_t err=feishu_http_get_stream(url,token,limit,response_sink,&buffer,
        cancel,context,&response->status_code,&received);
    if(err==ESP_OK && cancel && cancel(context))err=FEISHU_HTTP_ERR_CANCELLED;
    // get_stream has already released its HTTP/TLS client here.
    if(err==ESP_OK)err=join_response_chunks(&buffer);
    if(err==ESP_OK) {
        response->body=buffer.data;response->length=buffer.length;buffer.data=NULL;
    }
    free(buffer.data);free_response_chunks(&buffer);return err;
}

static esp_err_t http_event(esp_http_client_event_t *event)
{
    response_buffer_t *buffer = event->user_data;

    if (event->event_id == HTTP_EVENT_ON_DATA && buffer->error == ESP_OK) {
        buffer->error = append_response(buffer, event->data,
                                        (size_t)event->data_len);
    }
    return buffer->error;
}


esp_err_t feishu_http_oauth_request(const char *url,const char *json_body,
                                    char *access_token,size_t access_capacity,
                                    char *refresh_token,size_t refresh_capacity,
                                    char *oauth_error,size_t error_capacity,
                                    int *status_code,size_t *response_length)
{
    if(!url || !json_body || !status_code || !response_length || !oauth_error || error_capacity<2)
        return ESP_ERR_INVALID_ARG;
    *status_code=0;*response_length=0;
    feishu_oauth_stream_t *parser=calloc(1,sizeof(*parser));
    if(!parser)return ESP_ERR_NO_MEM;
    feishu_oauth_stream_init(parser,access_token,access_capacity,refresh_token,refresh_capacity,
                              oauth_error,error_capacity);
    esp_http_client_config_t config={.url=url,.method=HTTP_METHOD_POST,
        .crt_bundle_attach=esp_crt_bundle_attach,.timeout_ms=30000,
        .buffer_size=2048,.buffer_size_tx=2048,.disable_auto_redirect=false};
    esp_http_client_handle_t client=esp_http_client_init(&config);
    esp_err_t err=ESP_ERR_NO_MEM;
    if(!client)goto done;
    esp_http_client_set_header(client,"Accept","application/json");
    esp_http_client_set_header(client,"Content-Type","application/json; charset=utf-8");
    size_t length=strlen(json_body);
    err=esp_http_client_open(client,(int)length);
    // Keep the tested bounded request upload for multi-kilobyte refresh tokens.
    for(size_t offset=0;err==ESP_OK && offset<length;) {
        size_t remaining=length-offset;
        int size=(int)(remaining>1024 ? 1024 : remaining);
        int wrote=esp_http_client_write(client,json_body+offset,size);
        if(wrote<=0)err=ESP_ERR_HTTP_WRITE_DATA;
        else offset+=(size_t)wrote;
    }
    if(err==ESP_OK && esp_http_client_fetch_headers(client)<0)err=ESP_ERR_HTTP_FETCH_HEADER;
    uint8_t block[512];
    while(err==ESP_OK) {
        if(!feishu_http_progress()){err=FEISHU_HTTP_ERR_CANCELLED;break;}
        int got=esp_http_client_read(client,(char *)block,sizeof(block));
        if(got<0) {err=ESP_FAIL;break;}
        if(!got) {
            if(!esp_http_client_is_complete_data_received(client))err=ESP_ERR_INVALID_RESPONSE;
            break;
        }
        if(*response_length>32768 || (size_t)got>32768-*response_length) {err=ESP_ERR_INVALID_SIZE;break;}
        *response_length+=(size_t)got;
        if(!feishu_oauth_stream_feed(parser,block,(size_t)got))
            err=parser->overflow ? ESP_ERR_INVALID_SIZE : ESP_ERR_INVALID_RESPONSE;
    }
    if(err==ESP_OK && !feishu_oauth_stream_finish(parser))err=ESP_ERR_INVALID_RESPONSE;
    *status_code=esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
done:
    memset(parser,0,sizeof(*parser));free(parser);
    return err;
}

static esp_err_t request_content(esp_http_client_method_t method,
                              const char *url, const char *bearer_token,
                              const char *json_body, size_t response_limit,
                              feishu_http_response_t *response,bool form)
{
    response_buffer_t buffer = {
        .limit = response_limit,
        .error = ESP_OK,
        .segmented = true,
    };
    esp_http_client_config_t config = {
        .url = url,
        .method = method,
        .event_handler = http_event,
        .user_data = &buffer,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 20000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .disable_auto_redirect = form,
    };
    esp_http_client_handle_t client;
    esp_err_t err;
    char *authorization = NULL;

    if (url == NULL || response == NULL || response_limit == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (bearer_token != NULL && bearer_token[0] != '\0') {
        // ESP HTTP serializes the request line and Authorization header into
        // this buffer. Feishu user access tokens can be over 7 KiB.
        config.buffer_size_tx = (int)strlen(bearer_token) + 512;
    }
    memset(response, 0, sizeof(*response));
    client = esp_http_client_init(&config);
    if (client == NULL) return ESP_ERR_NO_MEM;
    esp_http_client_set_header(client, "Accept", "application/json");
    if (bearer_token != NULL && bearer_token[0] != '\0') {
        size_t authorization_size = strlen(bearer_token) + 8;
        authorization = malloc(authorization_size);
        if (authorization == NULL) {
            esp_http_client_cleanup(client);
            return ESP_ERR_NO_MEM;
        }
        int written = snprintf(authorization, authorization_size,
                               "Bearer %s", bearer_token);
        if (written < 0 || (size_t)written >= authorization_size) {
            memset(authorization, 0, authorization_size);
            free(authorization);
            esp_http_client_cleanup(client);
            return ESP_ERR_INVALID_SIZE;
        }
        err = esp_http_client_set_header(client, "Authorization", authorization);
        memset(authorization, 0, authorization_size);
        free(authorization);
        authorization = NULL;
        if (err != ESP_OK) {
            esp_http_client_cleanup(client);
            return err;
        }
    }
    if (json_body != NULL) {
        esp_http_client_set_header(client, "Content-Type",
                                   form ? "application/x-www-form-urlencoded" : "application/json; charset=utf-8");
        esp_http_client_set_post_field(client, json_body, strlen(json_body));
    }

    err = esp_http_client_perform(client);
    response->status_code = esp_http_client_get_status_code(client);
    // Free TLS/client allocations before asking the heap for one contiguous
    // JSON buffer. During the request the body lives in small linked chunks.
    esp_http_client_cleanup(client);
    client = NULL;
    if (err == ESP_OK && buffer.error != ESP_OK) err = buffer.error;
    if (err == ESP_OK) err = join_response_chunks(&buffer);
    if (err == ESP_OK) {
        response->body = buffer.data;
        response->length = buffer.length;
        buffer.data = NULL;
    } else {
        ESP_LOGW(TAG, "request failed: %s, HTTP %d",
                 esp_err_to_name(err), response->status_code);
    }
    if (authorization != NULL) {
        memset(authorization, 0, strlen(authorization));
        free(authorization);
    }
    free(buffer.data);
    free_response_chunks(&buffer);
    if (client != NULL) esp_http_client_cleanup(client);
    return err;
}

esp_err_t feishu_http_request(esp_http_client_method_t method,const char *url,
                              const char *token,const char *body,size_t limit,
                              feishu_http_response_t *response)
{ return request_content(method,url,token,body,limit,response,false); }
esp_err_t feishu_http_form_request(const char *url,const char *body,size_t limit,
                                   feishu_http_response_t *response)
{ return request_content(HTTP_METHOD_POST,url,NULL,body,limit,response,true); }

esp_err_t feishu_http_form_request_cancelable(const char *url,const char *body,size_t limit,
    feishu_http_response_t *response,feishu_http_cancel_t cancel,void *context)
{
    if(!url || !body || !limit || !response)return ESP_ERR_INVALID_ARG;
    memset(response,0,sizeof(*response));
    if(cancel && cancel(context))return FEISHU_HTTP_ERR_CANCELLED;
    response_buffer_t buffer={.limit=limit,.error=ESP_OK,.segmented=true};
    esp_http_client_config_t config={.url=url,.method=HTTP_METHOD_POST,
        .crt_bundle_attach=esp_crt_bundle_attach,.timeout_ms=5000,
        .buffer_size=2048,.buffer_size_tx=2048,.disable_auto_redirect=true};
    esp_http_client_handle_t client=esp_http_client_init(&config);
    if(!client)return ESP_ERR_NO_MEM;
    esp_err_t err=ESP_OK;size_t length=strlen(body),at=0;
    int64_t deadline=esp_timer_get_time()+30000000;
    esp_http_client_set_header(client,"Accept","application/json");
    esp_http_client_set_header(client,"Accept-Encoding","identity");
    esp_http_client_set_header(client,"Content-Type","application/x-www-form-urlencoded");
    if(cancel && cancel(context)){err=FEISHU_HTTP_ERR_CANCELLED;goto done;}
    err=esp_http_client_open(client,(int)length);if(err!=ESP_OK)goto done;
    esp_http_client_set_timeout_ms(client,1000);
    while(at<length) {
        if(cancel && cancel(context)){err=FEISHU_HTTP_ERR_CANCELLED;goto done;}
        if(esp_timer_get_time()>deadline){err=ESP_ERR_TIMEOUT;goto done;}
        size_t n=length-at;if(n>512)n=512;
        int wrote=esp_http_client_write(client,body+at,(int)n);
        if(wrote<=0 || (size_t)wrote>n){err=ESP_ERR_HTTP_WRITE_DATA;goto done;}
        at+=(size_t)wrote;
    }
    err=fetch_headers_poll(client,deadline,cancel,context,&response->status_code);
    while(err==ESP_OK) {
        if(!feishu_http_progress()){err=FEISHU_HTTP_ERR_CANCELLED;break;}
        if(cancel && cancel(context)){err=FEISHU_HTTP_ERR_CANCELLED;break;}
        if(esp_timer_get_time()>deadline){err=ESP_ERR_TIMEOUT;break;}
        char bytes[512];int got=esp_http_client_read(client,bytes,sizeof(bytes));
        if(got==-ESP_ERR_HTTP_EAGAIN)continue;
        if(got<0){err=ESP_FAIL;break;}
        if(!got){if(!esp_http_client_is_complete_data_received(client))err=ESP_ERR_INVALID_RESPONSE;break;}
        err=append_response(&buffer,bytes,(size_t)got);
    }
done:
    if(esp_http_client_get_status_code(client)>0)response->status_code=esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if(err==ESP_OK && cancel && cancel(context))err=FEISHU_HTTP_ERR_CANCELLED;
    if(err==ESP_OK)err=join_response_chunks(&buffer);
    if(err==ESP_OK){response->body=buffer.data;response->length=buffer.length;buffer.data=NULL;}
    free(buffer.data);free_response_chunks(&buffer);
    return err;
}

esp_err_t feishu_http_download_file(const char *url, const char *bearer_token,
                                    const char *path, size_t limit,
                                    int *status_code, size_t *length)
{
    download_file_t download = { .limit = limit, .error = ESP_OK };
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .event_handler = download_event,
        .user_data = &download,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .buffer_size = 2048,
        .disable_auto_redirect = false,
    };
    esp_http_client_handle_t client = NULL;
    char *authorization = NULL;
    esp_err_t err = ESP_FAIL;

    if (url == NULL || bearer_token == NULL || bearer_token[0] == '\0' ||
        path == NULL || limit == 0 || status_code == NULL || length == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *status_code = 0;
    *length = 0;
    download.file = fopen(path, "wb");
    if (download.file == NULL) return ESP_FAIL;
    config.buffer_size_tx = (int)strlen(bearer_token) + 512;
    client = esp_http_client_init(&config);
    if (client == NULL) {
        err = ESP_ERR_NO_MEM;
        goto done;
    }
    size_t authorization_size = strlen(bearer_token) + 8;
    authorization = malloc(authorization_size);
    if (authorization == NULL) {
        err = ESP_ERR_NO_MEM;
        goto done;
    }
    snprintf(authorization, authorization_size, "Bearer %s", bearer_token);
    err = esp_http_client_set_header(client, "Authorization", authorization);
    memset(authorization, 0, authorization_size);
    free(authorization);
    authorization = NULL;
    if (err != ESP_OK) goto done;
    esp_http_client_set_header(client, "Accept", "*/*");
    err = esp_http_client_perform(client);
    *status_code = esp_http_client_get_status_code(client);
    if (err == ESP_OK && download.error != ESP_OK) err = download.error;

done:
    if (authorization != NULL) {
        memset(authorization, 0, strlen(authorization));
        free(authorization);
    }
    if (client != NULL) esp_http_client_cleanup(client);
    fclose(download.file);
    *length = download.length;
    if (err != ESP_OK || *status_code < 200 || *status_code >= 300) {
        remove(path);
    }
    return err;
}

void feishu_http_response_free(feishu_http_response_t *response)
{
    if (response == NULL) return;
    free(response->body);
    memset(response, 0, sizeof(*response));
}

static esp_err_t get_stream_code(const char *url,const char *bearer_token,size_t limit,
                                  feishu_http_sink_t sink,void *context,
                                  feishu_http_cancel_t cancel,void *cancel_context,
                                  int *status_code,size_t *length,int *api_code)
{
    if(!url || !bearer_token || !*bearer_token || !limit || !sink || !status_code || !length)
        return ESP_ERR_INVALID_ARG;
    *status_code=0;*length=0;if(api_code)*api_code=-1;
    size_t token_size=strlen(bearer_token);
    if(token_size>=FEISHU_TOKEN_MAX)return ESP_ERR_INVALID_SIZE;
    esp_http_client_config_t config={.url=url,.method=HTTP_METHOD_GET,
        .crt_bundle_attach=esp_crt_bundle_attach,.timeout_ms=polled_read_owner() ? 1000:30000,.is_async=polled_read_owner(),
        .buffer_size=1024,.buffer_size_tx=(int)token_size+512,
        .disable_auto_redirect=true};
#if CONFIG_MBEDTLS_DYNAMIC_BUFFER
    /* Reserve RX once after handshake, before response parsing can fragment
     * its hole. The short-lived GET client releases it at cleanup. WSS and
     * POST retain their existing policies. This is a public IDF strategy. */
    config.tls_dyn_buf_strategy=HTTP_TLS_DYN_BUF_RX_STATIC;
    ESP_LOGI(TAG,"HTTP_RX_STATIC max_content=%u",(unsigned)CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN);
#endif
    esp_http_client_handle_t client=esp_http_client_init(&config);
    if(!client)return ESP_ERR_NO_MEM;
    if(esp_http_client_defer_get_tx_buffer(client)!=ESP_OK){esp_http_client_cleanup(client);return ESP_ERR_INVALID_STATE;}
    esp_err_t err=ESP_OK;feishu_api_error_t *api_error=NULL;
    char *auth=malloc(token_size+8);
    if(!auth) {err=ESP_ERR_NO_MEM;goto done;}
    snprintf(auth,token_size+8,"Bearer %s",bearer_token);
    err=esp_http_client_set_header(client,"Authorization",auth);
    memset(auth,0,token_size+8);free(auth);auth=NULL;
    if(err!=ESP_OK)goto done;
    esp_http_client_set_header(client,"Accept","application/json");
    esp_http_client_set_header(client,"Accept-Encoding","identity");
    err=open_read_poll(client,cancel,cancel_context);
    if(err!=ESP_OK)goto done;
    // open() has transmitted the request headers. This single-request client
    // has redirects disabled; its multi-kilobyte token copy is no longer needed
    // while receiving/parsing data. The session retains the actual credential.
    esp_http_client_delete_header(client,"Authorization");
    /* Poll between bounded socket waits, including while headers are held by
     * a server. Keep a status already parsed across a partial-header timeout:
     * IDF fetch_headers resets response.status_code on each invocation. */
    esp_http_client_set_timeout_ms(client,1000);
    int64_t deadline=esp_timer_get_time()+120000000;
    err=fetch_headers_poll(client,deadline,cancel,cancel_context,status_code);
    if(err!=ESP_OK)goto done;
    if(*status_code<200 || *status_code>=300) {
        if(!api_code){err=ESP_FAIL;goto done;}
        api_error=calloc(1,sizeof(*api_error));if(!api_error){err=ESP_ERR_NO_MEM;goto done;}
        feishu_api_error_init(api_error);
    }
    /* No consumer is called from inside a TLS/event-handler frame. */
    uint8_t block[1024];unsigned chunks=0;
    while(err==ESP_OK) {
        if(!feishu_http_progress()){err=FEISHU_HTTP_ERR_CANCELLED;break;}
        if(cancel && cancel(cancel_context)) {err=FEISHU_HTTP_ERR_CANCELLED;break;}
        if(esp_timer_get_time()>deadline) {err=ESP_ERR_TIMEOUT;break;}
        int got=esp_http_client_read(client,(char *)block,sizeof(block));
        if(got==-ESP_ERR_HTTP_EAGAIN)continue;
        if(got<0) {err=ESP_FAIL;break;}
        if(got==0) {
            if(!esp_http_client_is_complete_data_received(client))err=ESP_ERR_INVALID_RESPONSE;
            break;
        }
        if(*length>limit || (size_t)got>limit-*length) {err=ESP_ERR_INVALID_SIZE;break;}
        *length+=(size_t)got;
        if(api_error)err=feishu_api_error_feed(api_error,block,(size_t)got) ? ESP_OK:ESP_ERR_INVALID_RESPONSE;
        else err=sink(context,block,(size_t)got);
          if(++chunks%8==0)vTaskDelay(1);
    }
    if(api_error && err==ESP_OK){
        if(feishu_api_error_finish(api_error))*api_code=api_error->code;
        ESP_LOGI(TAG,"HTTP_API_ERROR status=%d code=%d bytes=%u",*status_code,*api_code,(unsigned)*length);
        err=ESP_FAIL;
    }
done:
    free(api_error);
    if(auth) {memset(auth,0,token_size+8);free(auth);}
    if(esp_http_client_get_status_code(client)>0)*status_code=esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    return err;
}

esp_err_t feishu_http_get_stream(const char *url,const char *token,size_t limit,
    feishu_http_sink_t sink,void *context,feishu_http_cancel_t cancel,void *cancel_context,int *status,size_t *length)
{return get_stream_code(url,token,limit,sink,context,cancel,cancel_context,status,length,NULL);}
esp_err_t feishu_http_get_stream_code(const char *url,const char *token,size_t limit,
    feishu_http_sink_t sink,void *context,feishu_http_cancel_t cancel,void *cancel_context,int *status,size_t *length,int *api_code)
{return get_stream_code(url,token,limit,sink,context,cancel,cancel_context,status,length,api_code);}

esp_err_t feishu_http_stream_request(const char *url, const char *bearer_token,
                                     const char *json_body,
                                     size_t response_limit,
                                     feishu_http_response_t *response)
{
    response_buffer_t buffer = {
        .limit = response_limit,
        .error = ESP_OK,
    };
    char *authorization = NULL;
    esp_err_t err;

    if (url == NULL || bearer_token == NULL || json_body == NULL ||
        response == NULL || response_limit == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(response, 0, sizeof(*response));
    if (s_stream_client == NULL) {
        esp_http_client_config_t config = {
            .url = url,
            .method = HTTP_METHOD_POST,
            .event_handler = http_event,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms = 20000,
            .buffer_size = 2048,
            // The Authorization header contains the large user token.
            .buffer_size_tx = (int)strlen(bearer_token) + 512,
            .keep_alive_enable = true,
        };
        s_stream_client = esp_http_client_init(&config);
        if (s_stream_client == NULL) return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_user_data(s_stream_client, &buffer);
    size_t authorization_size = strlen(bearer_token) + 8;
    authorization = malloc(authorization_size);
    if (authorization == NULL) return ESP_ERR_NO_MEM;
    snprintf(authorization, authorization_size, "Bearer %s", bearer_token);
    err = esp_http_client_set_header(s_stream_client, "Authorization", authorization);
    memset(authorization, 0, authorization_size);
    free(authorization);
    authorization = NULL;
    if (err != ESP_OK) {
        feishu_http_stream_close();
        return err;
    }
    esp_http_client_set_header(s_stream_client, "Accept", "application/json");
    esp_http_client_set_header(s_stream_client, "Content-Type",
                               "application/json; charset=utf-8");
    esp_http_client_set_post_field(s_stream_client, json_body, strlen(json_body));
    err = esp_http_client_perform(s_stream_client);
    response->status_code = esp_http_client_get_status_code(s_stream_client);
    if (err == ESP_OK && buffer.error != ESP_OK) err = buffer.error;
    if (err == ESP_OK && buffer.data == NULL) {
        buffer.data = calloc(1, 1);
        if (buffer.data == NULL) err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        response->body = buffer.data;
        response->length = buffer.length;
        buffer.data = NULL;
    } else {
        ESP_LOGW(TAG, "stream request failed: %s, HTTP %d",
                 esp_err_to_name(err), response->status_code);
        feishu_http_stream_close();
    }
    free(buffer.data);
    return err;
}

void feishu_http_stream_close(void)
{
    if (s_stream_client != NULL) {
        esp_http_client_cleanup(s_stream_client);
        s_stream_client = NULL;
    }
}

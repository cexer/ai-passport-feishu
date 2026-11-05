#include "feishu_api.h"

#include "cJSON.h"
#include "esp_log.h"
#include "feishu_http.h"
#include "feishu_binding.h"
#include "feishu_auth_policy.h"
#include "feishu_cache.h"
#include "feishu_messages_stream.h"
#include "feishu_message_store.h"
#include "feishu_result_stream.h"
#include "feishu_send_codec.h"
#include "feishu_internal_asr_protocol.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

static const char *TAG = "feishu_api";
static atomic_uint s_read_epoch;
void feishu_api_cancel_prefetch(void) {(void)atomic_fetch_add(&s_read_epoch,1);}

#define FEISHU_OAUTH_URL "https://accounts.feishu.cn/oauth/v3/token"
#define FEISHU_TENANT_TOKEN_URL \
    "https://open.feishu.cn/open-apis/auth/v3/tenant_access_token/internal"
#define FEISHU_USER_INFO_URL \
    "https://open.feishu.cn/open-apis/authen/v1/user_info"
#define FEISHU_CHATS_URL \
    "https://open.feishu.cn/open-apis/im/v1/chats?types=p2p%%2Cgroup" \
    "&sort_type=ByActiveTimeDesc&page_size=%u&user_id_type=open_id"
#define FEISHU_MESSAGES_URL \
    "https://open.feishu.cn/open-apis/im/v1/messages?container_id_type=chat" \
    "&container_id=%s&sort_type=ByCreateTimeDesc&page_size=%u" \
    "&with_sender_name=true"
#define FEISHU_REPLY_URL \
    "https://open.feishu.cn/open-apis/im/v1/messages/%s/reply"
#define FEISHU_SEND_URL \
    "https://open.feishu.cn/open-apis/im/v1/messages?receive_id_type=chat_id"
#define FEISHU_RESOURCE_URL \
    "https://open.feishu.cn/open-apis/im/v1/messages/%s/resources/%s?type=image"
#define FEISHU_ASR_STREAM_URL \
    "https://open.feishu.cn/open-apis/speech_to_text/v1/speech/stream_recognize"

static cJSON *parse_success(feishu_http_response_t *response)
{
    cJSON *root;
    cJSON *code;

    if (response->status_code < 200 || response->status_code >= 300 ||
        response->body == NULL) {
        ESP_LOGW(TAG, "HTTP status %d", response->status_code);
        return NULL;
    }
    root = cJSON_ParseWithLength(response->body, response->length);
    if (root == NULL) return NULL;
    code = cJSON_GetObjectItemCaseSensitive(root, "code");
    if (cJSON_IsNumber(code) && code->valueint != 0) {
        // Upstream descriptions may echo user data; log numeric codes only.
        ESP_LOGW(TAG, "OpenAPI error %d", code->valueint);
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

static bool copy_json_string(cJSON *object, const char *name, char *output,
                             size_t output_size)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);

    if (!cJSON_IsString(item) || item->valuestring == NULL ||
        strlen(item->valuestring) >= output_size) {
        return false;
    }
    memcpy(output, item->valuestring, strlen(item->valuestring) + 1);
    return true;
}

static char *json_print(cJSON *root)
{
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

static esp_err_t refresh_user_token(feishu_api_session_t *session,
                                    feishu_credentials_t *credentials)
{
    if(!session || !credentials || !credentials->refresh_token[0])return ESP_ERR_INVALID_STATE;
    cJSON *request = cJSON_CreateObject();
    char *body;
    char *next_refresh = NULL;
    char oauth_error[64] = { 0 };
    int status_code = 0;
    size_t response_length = 0;
    esp_err_t err = ESP_FAIL;
    bool response_valid=false;

    if (request == NULL) return ESP_ERR_NO_MEM;
    next_refresh = calloc(1, FEISHU_TOKEN_MAX);
    if (next_refresh == NULL) {
        cJSON_Delete(request);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(request, "grant_type", "refresh_token");
    cJSON_AddStringToObject(request, "client_id", credentials->app_id);
    cJSON_AddStringToObject(request, "client_secret", credentials->app_secret);
    cJSON_AddStringToObject(request, "refresh_token", credentials->refresh_token);
    ESP_LOGI(TAG, "refresh token bytes %u",
             (unsigned)strlen(credentials->refresh_token));
    body = json_print(request);
    if (body == NULL) {
        free(next_refresh);
        return ESP_ERR_NO_MEM;
    }
    err = feishu_http_oauth_request(
        FEISHU_OAUTH_URL, body,
        session->user_access_token, sizeof(session->user_access_token),
        next_refresh, FEISHU_TOKEN_MAX, oauth_error, sizeof(oauth_error),
        &status_code, &response_length);
    memset(body, 0, strlen(body));
    free(body);
    if (err != ESP_OK) goto done;
    ESP_LOGI(TAG, "OAuth refresh HTTP %d, response bytes %u",
             status_code, (unsigned)response_length);
    if (feishu_auth_refresh_revoked(status_code, oauth_error)) {
        err = FEISHU_ERR_AUTH_REVOKED;
        goto done;
    }
    if(feishu_auth_app_rejected(status_code,oauth_error)) {
        err=FEISHU_ERR_APP_REJECTED;goto done;
    }
    if (status_code >= 500) {
        err = ESP_FAIL;
        goto done;
    }
    if (status_code < 200 || status_code >= 300 ||
        session->user_access_token[0] == '\0' || next_refresh[0] == '\0') {
        err = ESP_ERR_INVALID_RESPONSE;
        goto done;
    }
    response_valid=true;
    feishu_utf8_copy(credentials->refresh_token,sizeof(credentials->refresh_token),next_refresh);
    err = feishu_store_save_refresh_token(next_refresh);
    if (err == ESP_OK) {
        esp_err_t cache_err = feishu_store_save_access_token(
            session->user_access_token);
        if (cache_err != ESP_OK) {
            ESP_LOGW(TAG, "access-token cache failed: %s",
                     esp_err_to_name(cache_err));
        }
    } else err=FEISHU_ERR_CREDENTIAL_SAVE;

done:
    if(!response_valid && feishu_store_load_access_token(session->user_access_token,
            sizeof(session->user_access_token))!=ESP_OK)session->user_access_token[0]=0;
    if (next_refresh != NULL) {
        memset(next_refresh, 0, FEISHU_TOKEN_MAX);
        free(next_refresh);
    }
    return err;
}

static esp_err_t fetch_tenant_token(feishu_api_session_t *session)
{
    feishu_http_response_t response = { 0 };
    cJSON *request = cJSON_CreateObject();
    cJSON *root = NULL;
    char *body;
    esp_err_t err;

    if (request == NULL) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(request, "app_id", session->app_id);
    cJSON_AddStringToObject(request, "app_secret", session->app_secret);
    body = json_print(request);
    if (body == NULL) return ESP_ERR_NO_MEM;
    err = feishu_http_request(HTTP_METHOD_POST, FEISHU_TENANT_TOKEN_URL, NULL,
                              body, 4096, &response);
    memset(body, 0, strlen(body));
    free(body);
    if (err != ESP_OK) goto done;
    if (response.status_code == 401 || response.status_code == 403) {
        err = ESP_ERR_INVALID_RESPONSE;
        goto done;
    }
    if (response.status_code < 200 || response.status_code >= 300) {
        err = ESP_FAIL;
        goto done;
    }
    root = parse_success(&response);
    if (root == NULL ||
        !copy_json_string(root, "tenant_access_token",
                          session->tenant_access_token,
                          sizeof(session->tenant_access_token))) {
        err = ESP_ERR_INVALID_RESPONSE;
    }

done:
    cJSON_Delete(root);
    feishu_http_response_free(&response);
    return err;
}

static bool user_token_rejected(const feishu_http_response_t *response)
{
    cJSON *root = response->body == NULL ? NULL :
        cJSON_ParseWithLength(response->body, response->length);
    cJSON *code = root == NULL ? NULL :
        cJSON_GetObjectItemCaseSensitive(root, "code");
    bool rejected = feishu_auth_access_rejected(response->status_code,
        cJSON_IsNumber(code) ? code->valueint : 0);
    cJSON_Delete(root);
    return rejected;
}

static esp_err_t refresh_session_token(feishu_api_session_t *session)
{
    feishu_credentials_t *credentials = calloc(1, sizeof(*credentials));
    if (credentials == NULL) return ESP_ERR_NO_MEM;
    esp_err_t err = feishu_store_load_credentials(credentials);
    if (err == ESP_OK) err = refresh_user_token(session, credentials);
    memset(credentials, 0, sizeof(*credentials));
    free(credentials);
    return err;
}

static esp_err_t fetch_user_info(feishu_api_session_t *session)
{
    feishu_http_response_t response = { 0 };
    cJSON *root = NULL;
    cJSON *data;
    esp_err_t err = feishu_http_request(HTTP_METHOD_GET, FEISHU_USER_INFO_URL,
                                        session->user_access_token, NULL, 4096,
                                        &response);

    if (err != ESP_OK && response.status_code != 401) goto done;
    if (user_token_rejected(&response)) {
        err = FEISHU_ERR_ACCESS_REJECTED;
        goto done;
    }
    if (response.status_code < 200 || response.status_code >= 300) {
        err = ESP_FAIL;
        goto done;
    }
    root = parse_success(&response);
    data = root == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!cJSON_IsObject(data) ||
        !copy_json_string(data, "open_id", session->self_open_id,
                          sizeof(session->self_open_id))) {
        err = ESP_ERR_INVALID_RESPONSE;
    }

done:
    cJSON_Delete(root);
    feishu_http_response_free(&response);
    return err;
}

esp_err_t feishu_api_authenticate(feishu_api_session_t *session,
                                  feishu_credentials_t *credentials)
{
    esp_err_t err;
    bool token_verified = false;

    if (session == NULL || credentials == NULL) return ESP_ERR_INVALID_ARG;
    memset(session, 0, sizeof(*session));
    feishu_utf8_copy(session->app_id, sizeof(session->app_id),
                     credentials->app_id);
    feishu_utf8_copy(session->app_secret, sizeof(session->app_secret),
                     credentials->app_secret);
    bool have_token = feishu_binding_take_access_token(
        session->user_access_token, sizeof(session->user_access_token));
    if (have_token) {
        ESP_LOGI(TAG, "using access token from completed device binding");
    } else if (feishu_store_load_access_token(
                   session->user_access_token,
                   sizeof(session->user_access_token)) == ESP_OK) {
        have_token = true;
        ESP_LOGI(TAG, "using cached access token");
    }
    if (have_token) {
        for (unsigned attempt = 1; attempt <= 3; ++attempt) {
            err = fetch_user_info(session);
            if (err == ESP_OK || err == FEISHU_ERR_ACCESS_REJECTED) break;
            ESP_LOGW(TAG, "access-token verification attempt %u failed: %s",
                     attempt, esp_err_to_name(err));
            if (attempt < 3) vTaskDelay(pdMS_TO_TICKS(1000 * attempt));
        }
        token_verified = err == ESP_OK;
        if (!token_verified && err == FEISHU_ERR_ACCESS_REJECTED) {
            // Only an actual HTTP/auth rejection invalidates the token.
            // DNS, TLS and socket failures must leave the cached token intact.
            ESP_LOGW(TAG, "access token rejected by Feishu; refreshing");
            session->user_access_token[0] = '\0';
        } else if (!token_verified) {
            return err;
        }
    }
    if (!token_verified) {
        err = ESP_FAIL;
        for (unsigned attempt = 1; attempt <= 3; ++attempt) {
            err = refresh_user_token(session, credentials);
            if (err == ESP_OK || err == FEISHU_ERR_AUTH_REVOKED || err==FEISHU_ERR_APP_REJECTED ||
                err == ESP_ERR_INVALID_RESPONSE || err==FEISHU_ERR_CREDENTIAL_SAVE) break;
            ESP_LOGW(TAG, "token refresh transport attempt %u failed: %s",
                     attempt, esp_err_to_name(err));
            if (attempt < 3) vTaskDelay(pdMS_TO_TICKS(1500 * attempt));
        }
    }
    if (err == ESP_OK && !token_verified) err = fetch_user_info(session);
    return err;
}

typedef struct {unsigned epoch;} chat_read_t;
static bool chat_read_cancelled(void *context)
{return ((chat_read_t *)context)->epoch!=atomic_load(&s_read_epoch);}
static esp_err_t chat_list_request(feishu_api_session_t *session,const char *url,feishu_http_response_t *response)
{
    chat_read_t read={atomic_load(&s_read_epoch)};
    for(unsigned attempt=0;attempt<2;++attempt) {
        feishu_http_response_free(response);
        esp_err_t err=feishu_http_get_response(url,session->user_access_token,16384,
            chat_read_cancelled,&read,response);
        ESP_LOGI(TAG,"chat response HTTP=%d bytes=%u result=%s free=%u largest=%u",
            response->status_code,(unsigned)response->length,esp_err_to_name(err),
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
        if(chat_read_cancelled(&read))return FEISHU_HTTP_ERR_CANCELLED;
        if(err!=ESP_OK && response->status_code!=401)return err;
        if(!user_token_rejected(response))return ESP_OK;
        if(attempt)return FEISHU_ERR_ACCESS_REJECTED;
        err=refresh_session_token(session);if(err!=ESP_OK)return err;
    }
    return ESP_ERR_INVALID_RESPONSE;
}
esp_err_t feishu_api_chat_page(feishu_api_session_t *session,const char *token,
                                feishu_chat_t *chats,size_t capacity,feishu_page_cursor_t *page)
{
    char base[384],url[384+FEISHU_PAGE_TOKEN_MAX*3+13];
    feishu_http_response_t response = { 0 };
    cJSON *root = NULL;
    cJSON *data;
    cJSON *items;
    cJSON *item;
    size_t output_count = 0;
    esp_err_t err;

    if (session == NULL || chats == NULL || page == NULL || capacity == 0 || capacity>FEISHU_MAX_CHATS || !feishu_page_token_valid(token)) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(base, sizeof(base), FEISHU_CHATS_URL, (unsigned)capacity);
    if(!feishu_page_url(base,token,url,sizeof(url)))return ESP_ERR_INVALID_ARG;
    err = chat_list_request(session,url,&response);
    if (err != ESP_OK) goto done;
    if(response.status_code<200 || response.status_code>=300) {err=ESP_FAIL;goto done;}
    root=cJSON_ParseWithLengthOpts(response.body,response.length+1,NULL,true);
    cJSON *code=root ? cJSON_GetObjectItemCaseSensitive(root,"code"):NULL;
    if(!cJSON_IsNumber(code) || code->valuedouble!=0) {
        ESP_LOGW(TAG,"chat envelope rejected: numeric_code=%u code=%d",
            (unsigned)cJSON_IsNumber(code),cJSON_IsNumber(code)?code->valueint:-1);
        err=ESP_ERR_INVALID_RESPONSE;goto done;
    }
    data = root == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(root, "data");
    items = data == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(data, "items");
    if (!cJSON_IsArray(items)) {
        err = ESP_ERR_INVALID_RESPONSE;
        goto done;
    }
    cJSON_ArrayForEach(item, items) {
        cJSON *mode;
        if (output_count >= capacity) break;
        if (!copy_json_string(item, "chat_id", chats[output_count].chat_id,
                              sizeof(chats[output_count].chat_id)) ||
            !copy_json_string(item, "name", chats[output_count].name,
                              sizeof(chats[output_count].name))) {
            continue;
        }
        mode = cJSON_GetObjectItemCaseSensitive(item, "chat_mode");
        chats[output_count].p2p = cJSON_IsString(mode) &&
            strcmp(mode->valuestring, "p2p") == 0;
        ++output_count;
    }
    cJSON *more=cJSON_GetObjectItemCaseSensitive(data,"has_more");
    cJSON *next=cJSON_GetObjectItemCaseSensitive(data,"page_token");
    if(!cJSON_IsBool(more) || (cJSON_IsTrue(more) && !cJSON_IsString(next)) ||
       !feishu_page_response(page,token,cJSON_IsString(next) ? next->valuestring:"",cJSON_IsTrue(more),output_count)) {
        err=ESP_ERR_INVALID_RESPONSE;goto done;
    }
    err = ESP_OK;

done:
    ESP_LOGI(TAG,"chat page result=%s rows=%u",esp_err_to_name(err),(unsigned)output_count);
    cJSON_Delete(root);
    feishu_http_response_free(&response);
    return err;
}

esp_err_t feishu_api_list_chats(feishu_api_session_t *session,feishu_chat_t *chats,size_t capacity,size_t *count)
{
    if(!count)return ESP_ERR_INVALID_ARG;
    *count=0;feishu_page_cursor_t page={0};
    esp_err_t err=feishu_api_chat_page(session,"",chats,capacity,&page);
    if(err==ESP_OK)*count=page.count;
    return err;
}


typedef struct {
    feishu_messages_stream_t parser;
    feishu_message_store_t store;
    unsigned read_epoch;
} message_stream_context_t;

static bool message_epoch_cancelled(void *context)
{ return ((message_stream_context_t *)context)->read_epoch!=atomic_load(&s_read_epoch); }
typedef struct {
    message_stream_context_t *active;
    feishu_message_t *messages;
    size_t capacity;
    const char *self;
    unsigned epoch,scope;
    bool pagination;
} message_transfer_t;
static bool message_transfer_cancelled(void *context)
{
    message_transfer_t *t=context;
    return t->epoch!=atomic_load(&s_read_epoch) || t->scope!=feishu_cache_scope_version();
}
static esp_err_t message_bytes(void *context,const uint8_t *data,size_t size)
{
    message_transfer_t *t=context;
    if(message_transfer_cancelled(t))return FEISHU_HTTP_ERR_CANCELLED;
    if(!t->active) {
        // No parser/storage workspace competes with the TLS handshake peak.
        ESP_LOGI(TAG,"message workspace bytes=%u free=%u largest=%u",(unsigned)sizeof(*t->active),(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
        t->active=calloc(1,sizeof(*t->active));
        if(!t->active)return ESP_ERR_NO_MEM;
        t->active->read_epoch=t->epoch;
        feishu_message_store_init(&t->active->store,message_epoch_cancelled,t->active);
        feishu_messages_stream_init(&t->active->parser,t->messages,t->capacity,t->self,
            feishu_message_store_operations(),&t->active->store);
        t->active->parser.pagination_required=t->pagination;
    }
    return feishu_messages_stream_feed(&t->active->parser,data,size) ? ESP_OK:ESP_ERR_INVALID_RESPONSE;
}
static bool message_cancelled(void *context)
{
    message_stream_context_t *c=context;
    return message_epoch_cancelled(c) || c->store.scope!=feishu_cache_scope_version();
}
static esp_err_t read_message_stream(feishu_api_session_t *session,const char *url,
                                      feishu_message_t *messages,size_t capacity,size_t *count,const char *expected,
                                      const char *token,feishu_page_cursor_t *page)
{
    if(!session || !url || !messages || !count || !capacity || capacity>FEISHU_MAX_MESSAGES)
        return ESP_ERR_INVALID_ARG;
    *count=0;
    if(!session->user_access_token[0]) {
        esp_err_t ready=refresh_session_token(session);
        if(ready!=ESP_OK)return ready;
    }
    message_stream_context_t *c=NULL;
    unsigned epoch=atomic_load(&s_read_epoch),scope=feishu_cache_scope_version();
    esp_err_t err=ESP_FAIL;
    for(unsigned attempt=0;attempt<2;++attempt) {
        message_transfer_t transfer={.messages=messages,.capacity=capacity,
            .self=session->self_open_id,.epoch=epoch,.scope=scope,.pagination=page!=NULL};
        int status=0;size_t length=0;
        ESP_LOGI(TAG,"message start token_bytes=%u free=%u largest=%u stack=%u",(unsigned)strlen(session->user_access_token),(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),(unsigned)uxTaskGetStackHighWaterMark(NULL));
        err=feishu_http_get_stream(url,session->user_access_token,64u*1024u*1024u,
            message_bytes,&transfer,message_transfer_cancelled,&transfer,&status,&length);
        c=transfer.active;
        ESP_LOGI(TAG,"message stream HTTP %d, bytes %u result=%s",status,(unsigned)length,esp_err_to_name(err));
        if(err==FEISHU_HTTP_ERR_CANCELLED || message_transfer_cancelled(&transfer)) {err=FEISHU_HTTP_ERR_CANCELLED;break;}
        if(feishu_auth_access_rejected(status,c ? c->parser.api_code:-1)) {
            if(c)feishu_messages_stream_abort(&c->parser);
            feishu_cache_reap_abandoned();
            if(attempt) {err=FEISHU_ERR_ACCESS_REJECTED;break;}
            if(c) {memset(c,0,sizeof(*c));free(c);c=NULL;}
            err=refresh_session_token(session);
            if(err!=ESP_OK)break;
            if(message_transfer_cancelled(&transfer)) {err=FEISHU_HTTP_ERR_CANCELLED;break;}
            continue;
        }
        if(err!=ESP_OK)break;
        if(status<200 || status>=300) {err=ESP_FAIL;break;}
        if(!c) {err=ESP_ERR_INVALID_RESPONSE;break;}
        if(message_cancelled(c)) {err=FEISHU_HTTP_ERR_CANCELLED;break;}
        if(expected && (c->parser.count!=1 || strcmp(messages[0].message_id,expected))) {
            err=ESP_ERR_INVALID_RESPONSE;break;
        }
        if(page && (!c->parser.more_seen ||
           !feishu_page_response(page,token,c->parser.next_page,c->parser.more,c->parser.count))) {
            err=ESP_ERR_INVALID_RESPONSE;break;
        }
        if(!feishu_messages_stream_finish(&c->parser,count))err=ESP_ERR_INVALID_RESPONSE;
        if(message_cancelled(c))err=FEISHU_HTTP_ERR_CANCELLED;
        break;
    }
    if(c)feishu_messages_stream_abort(&c->parser);
    feishu_cache_reap_abandoned();
    if(c) {memset(c,0,sizeof(*c));free(c);}
    if(err==ESP_OK) {
        unsigned texts=0,stored=0,unavailable=0;
        for(size_t i=0;i<*count;++i)if(messages[i].type==FEISHU_MESSAGE_TEXT) {
            ++texts;stored+=messages[i].text_object_id>0;unavailable+=messages[i].text_unavailable;
        }
        ESP_LOGI(TAG,"message page rows=%u texts=%u stored=%u unavailable=%u",
            (unsigned)*count,texts,stored,unavailable);
    }
    return err;
}

esp_err_t feishu_api_list_messages(feishu_api_session_t *session,const char *chat_id,
                                   feishu_message_t *messages,size_t capacity,size_t *count)
{
    if(!chat_id)return ESP_ERR_INVALID_ARG;
    char url[512];
    snprintf(url,sizeof(url),FEISHU_MESSAGES_URL,chat_id,(unsigned)capacity);
    feishu_page_cursor_t page={0};
    return read_message_stream(session,url,messages,capacity,count,NULL,"",&page);
}

esp_err_t feishu_api_message_page(feishu_api_session_t *session,const char *chat_id,const char *token,
                                   feishu_message_t *messages,size_t capacity,feishu_page_cursor_t *page)
{
    if(!chat_id || !page || !feishu_page_token_valid(token))return ESP_ERR_INVALID_ARG;
    char base[512],url[512+FEISHU_PAGE_TOKEN_MAX*3+13];size_t count=0;
    snprintf(base,sizeof(base),FEISHU_MESSAGES_URL,chat_id,(unsigned)capacity);
    if(!feishu_page_url(base,token,url,sizeof(url)))return ESP_ERR_INVALID_ARG;
    return read_message_stream(session,url,messages,capacity,&count,NULL,token,page);
}
esp_err_t feishu_api_get_message(feishu_api_session_t *session,const char *message_id,feishu_message_t *message)
{
    if(!message_id || !*message_id || strlen(message_id)>=FEISHU_MESSAGE_ID_MAX)return ESP_ERR_INVALID_ARG;
    char url[256];size_t count=0;
    snprintf(url,sizeof(url),"https://open.feishu.cn/open-apis/im/v1/messages/%s?user_id_type=open_id",message_id);
    return read_message_stream(session,url,message,1,&count,message_id,NULL,NULL);
}

typedef struct { const feishu_send_source_t *source;const feishu_send_part_t *part; } message_upload_t;
static bool message_upload(void *context,feishu_http_write_t write,void *write_context)
{
    message_upload_t *p=context;return feishu_send_emit(p->source,p->part,write,write_context);
}
static esp_err_t result_bytes(void *context,const uint8_t *data,size_t size)
{
    return feishu_result_stream_feed(context,data,size) ? ESP_OK:ESP_ERR_INVALID_RESPONSE;
}
static esp_err_t send_source(feishu_api_session_t *session,const feishu_send_source_t *source,uint64_t offset,
                               const char *destination,const char *uuid,bool reply,bool require_full,bool trim_cache,
                               uint64_t *sent,bool *rejected)
{
    if(!session || !source || !sent || !rejected)return ESP_ERR_INVALID_ARG;
    *sent=0;*rejected=true;
    feishu_send_part_t part;
    int64_t plan_started=esp_timer_get_time();
    bool planned=feishu_send_plan(source,offset,destination,uuid,reply,&part);
    ESP_LOGI(TAG,"SEND_TIMING stage=plan ms=%u bytes=%u",(unsigned)((esp_timer_get_time()-plan_started)/1000),planned?(unsigned)part.length:0);
    // Planning may scan a disk-backed draft; release those pages before the
    // TLS handshake peak, then reuse the bounded cache during the upload.
    if(trim_cache)feishu_cache_trim();
    if(!planned)return ESP_ERR_INVALID_RESPONSE;
    if(require_full && part.length!=source->length)return ESP_ERR_INVALID_SIZE;
    char url[256];
    if(reply)snprintf(url,sizeof(url),FEISHU_REPLY_URL,destination);
    else snprintf(url,sizeof(url),"%s",FEISHU_SEND_URL);
    if(!session->user_access_token[0]) {
        esp_err_t err=refresh_session_token(session);if(err!=ESP_OK)return err;
    }
    message_upload_t upload={source,&part};
    for(unsigned attempt=0;attempt<2;++attempt) {
        char message_id[FEISHU_MESSAGE_ID_MAX];
        feishu_result_stream_t *parser=calloc(1,sizeof(*parser));if(!parser)return ESP_ERR_NO_MEM;
        feishu_result_stream_init(parser,"message_id",sizeof(message_id)-1,message_id,sizeof(message_id),NULL,NULL);
        int status=0;size_t length=0;
        ESP_LOGI(TAG,"message start token_bytes=%u free=%u largest=%u stack=%u",(unsigned)strlen(session->user_access_token),(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),(unsigned)uxTaskGetStackHighWaterMark(NULL));*rejected=false;
        int64_t post_started=esp_timer_get_time();
        esp_err_t err=feishu_http_post_stream(url,session->user_access_token,part.body_size,
            message_upload,&upload,false,64u*1024u*1024u,result_bytes,parser,NULL,NULL,&status,&length);
        ESP_LOGI(TAG,"SEND_TIMING stage=post ms=%u http=%d result=%d",(unsigned)((esp_timer_get_time()-post_started)/1000),status,err);
        if(trim_cache)feishu_cache_trim();
        bool success=err==ESP_OK && feishu_result_stream_finish(parser) && message_id[0];
        bool valid_error=err==ESP_OK && !parser->failed && feishu_json_finish(&parser->json) &&
            parser->code_seen && parser->code!=0;
        bool access_rejected=feishu_auth_access_rejected(status,parser->code);
        *rejected=valid_error || status==400 || status==401 || status==403 || status==404 ||
            status==405 || status==413 || status==415 || status==422 || status==429;
        memset(parser,0,sizeof(*parser));free(parser);
        if(access_rejected && !attempt) {
            err=refresh_session_token(session);if(err!=ESP_OK)return err;
            continue;
        }
        if(err==ESP_OK && status>=200 && status<300 && success) {*sent=part.length;return ESP_OK;}
        return access_rejected ? FEISHU_ERR_ACCESS_REJECTED:err!=ESP_OK ? err:ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_FAIL;
}
typedef struct {const char *text;size_t length;} memory_text_t;
static bool memory_text_read(void *context,uint64_t offset,void *data,size_t capacity,size_t *size)
{
    const memory_text_t *memory=context;const char *text=memory->text;size_t length=memory->length;
    if(offset>length)return false;
    *size=length-(size_t)offset;if(*size>capacity)*size=capacity;
    memcpy(data,text+(size_t)offset,*size);return true;
}
static esp_err_t send_memory(feishu_api_session_t *session,const char *destination,const char *text,const char *uuid,bool reply)
{
    if(!text || !*text)return ESP_ERR_INVALID_ARG;
    memory_text_t memory={text,strlen(text)};
    feishu_send_source_t source={memory_text_read,&memory,memory.length};uint64_t sent;bool rejected;
    return send_source(session,&source,0,destination,uuid,reply,true,false,&sent,&rejected);
}
esp_err_t feishu_api_reply_text(feishu_api_session_t *session,const char *message_id,const char *text,const char *uuid)
{ return send_memory(session,message_id,text,uuid,true); }
esp_err_t feishu_api_send_text(feishu_api_session_t *session,const char *chat,const char *text,const char *uuid)
{ return send_memory(session,chat,text,uuid,false); }
static bool draft_text_read(void *context,uint64_t offset,void *data,size_t capacity,size_t *size)
{
    const feishu_outbox_t *p=context;
    int64_t started=esp_timer_get_time();
    esp_err_t err=feishu_cache_text_read_stream(p->body_name,p->object_id,false,offset,data,capacity,size);
    int64_t elapsed=esp_timer_get_time()-started;
    if(elapsed>500000 || err!=ESP_OK)
        ESP_LOGW(TAG,"draft source read offset=%u bytes=%u elapsed_ms=%u result=%s",
            (unsigned)offset,(unsigned)*size,(unsigned)(elapsed/1000),esp_err_to_name(err));
    return err==ESP_OK;
}
esp_err_t feishu_api_send_pending_part(feishu_api_session_t *session,const feishu_outbox_t *p,
                                        uint64_t *sent,bool *rejected)
{
    if(!sent || !rejected)return ESP_ERR_INVALID_ARG;
    *sent=0;*rejected=true;
    if(!feishu_outbox_valid(p) || p->state!=FEISHU_OUTBOX_SENDING)return ESP_ERR_INVALID_ARG;
    char uuid[41];if(!feishu_outbox_uuid(p,uuid))return ESP_ERR_INVALID_ARG;
    feishu_send_source_t source={draft_text_read,(void *)p,p->length};
    return send_source(session,&source,p->offset,p->message[0] ? p->message:p->chat,uuid,p->message[0]!=0,false,true,sent,rejected);
}

esp_err_t feishu_api_download_image(feishu_api_session_t *session,
                                    const feishu_message_t *message,
                                    const char *path, size_t limit)
{
    char url[512];
    int status = 0;
    size_t length = 0;

    if (session == NULL || message == NULL || path == NULL || limit == 0 ||
        message->type != FEISHU_MESSAGE_IMAGE ||
        message->message_id[0] == '\0' || message->resource_key[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    int written = snprintf(url, sizeof(url), FEISHU_RESOURCE_URL,
                           message->message_id, message->resource_key);
    if (written < 0 || (size_t)written >= sizeof(url)) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        esp_err_t err = feishu_http_download_file(
            url, session->user_access_token, path, limit, &status, &length);
        if (err != ESP_OK && status != 401) return err;
        if (status >= 200 && status < 300 && length > 3) {
            unsigned char signature[3] = { 0 };
            FILE *image = fopen(path, "rb");
            bool jpeg = image != NULL &&
                fread(signature, 1, sizeof(signature), image) == sizeof(signature) &&
                signature[0] == 0xff && signature[1] == 0xd8 &&
                signature[2] == 0xff;
            if (image != NULL) fclose(image);
            if (jpeg) return ESP_OK;
            remove(path);
            return ESP_ERR_NOT_SUPPORTED;
        }
        if (status != 401 && status != 403) return ESP_FAIL;
        if (attempt > 0) return ESP_ERR_INVALID_RESPONSE;
        err = refresh_session_token(session);
        if (err != ESP_OK) return err;
    }
    return ESP_ERR_INVALID_RESPONSE;
}

static bool media_epoch_cancelled(void *context)
{ return *(const unsigned *)context!=atomic_load(&s_read_epoch); }
esp_err_t feishu_api_prepare_image(feishu_api_session_t *session,const feishu_message_t *message,
                                     const feishu_image_view_t *view,bool allow_network,feishu_prepared_image_t *image)
{
    if(!session || !message || !view || !image || message->type!=FEISHU_MESSAGE_IMAGE || message->deleted ||
       !memchr(message->message_id,0,sizeof(message->message_id)) || !memchr(message->resource_key,0,sizeof(message->resource_key)) ||
       !message->message_id[0] || !message->resource_key[0])return ESP_ERR_INVALID_ARG;
    char url[512];int n=snprintf(url,sizeof(url),FEISHU_RESOURCE_URL,message->message_id,message->resource_key);
    if(n<0 || (size_t)n>=sizeof(url))return ESP_ERR_INVALID_SIZE;
    unsigned epoch=atomic_load(&s_read_epoch);
    for(unsigned attempt=0;attempt<2;++attempt) {
        int status=0;
        esp_err_t err=feishu_image_prepare(url,session->user_access_token,message->message_id,message->resource_key,
            view,allow_network,media_epoch_cancelled,&epoch,image,&status);
        if(err==ESP_OK || err==FEISHU_HTTP_ERR_CANCELLED)return err;
        if(!allow_network || (err!=FEISHU_IMAGE_NEEDS_ACCESS && !feishu_auth_access_rejected(status,0)))return err;
        if(attempt)return FEISHU_ERR_ACCESS_REJECTED;
        err=refresh_session_token(session);if(err!=ESP_OK)return err;
    }
    return ESP_FAIL;
}

typedef struct {feishu_data_writer_t *writer;esp_err_t error;} asr_storage_t;
static bool asr_store(void *context,const void *data,size_t size)
{
    asr_storage_t *p=context;
    if(!p->writer)return true;
    if(!p->writer->id)p->error=feishu_cache_draft_begin(p->writer);
    if(p->error==ESP_OK)p->error=feishu_cache_text_append(p->writer,data,size);
    return p->error==ESP_OK;
}
static bool memory_upload(void *context,feishu_http_write_t write,void *write_context)
{ return write(write_context,context,strlen(context)); }
static bool asr_cancelled(void *context)
{ return context && *(const volatile bool *)context; }

esp_err_t feishu_api_asr_stream_packet(feishu_api_session_t *session,
                                       const char stream_id[17],
                                       uint32_t sequence_id, int action,
                                       const int16_t *pcm, size_t samples,
                                       char *recognition, size_t recognition_size,
                                       feishu_data_writer_t *final_draft,const volatile bool *cancel_requested)
{
    cJSON *request = NULL;
    cJSON *speech;
    cJSON *config;
    feishu_result_stream_t *parser=NULL;
    asr_storage_t storage={.writer=action==2 ? final_draft:NULL,.error=ESP_OK};
    size_t encoded_size = 0;
    size_t encoded_capacity;
    unsigned char *encoded = NULL;
    char *body = NULL;
    esp_err_t err = ESP_FAIL;

    if (session == NULL || stream_id == NULL || strlen(stream_id) != 16 ||
        pcm == NULL || samples == 0 || samples>1600 || recognition == NULL ||
        recognition_size == 0 || action < 0 || action > 3 || (action==2 && !final_draft)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (session->tenant_access_token[0] == '\0') {
        err = fetch_tenant_token(session);
        if (err != ESP_OK) return err;
    }
    encoded_capacity = ((samples * sizeof(*pcm) + 2) / 3) * 4 + 1;
    encoded = malloc(encoded_capacity);
    if (encoded == NULL) return ESP_ERR_NO_MEM;
    if (mbedtls_base64_encode(encoded, encoded_capacity, &encoded_size,
                              (const unsigned char *)pcm,
                              samples * sizeof(*pcm)) != 0) {
        err = ESP_FAIL;
        goto done;
    }
    encoded[encoded_size] = '\0';
    request = cJSON_CreateObject();
    speech = request == NULL ? NULL : cJSON_AddObjectToObject(request, "speech");
    config = request == NULL ? NULL : cJSON_AddObjectToObject(request, "config");
    if (speech == NULL || config == NULL) {
        err = ESP_ERR_NO_MEM;
        goto done;
    }
    // Keep one Base64 buffer. cJSON_CreateStringReference prevents another
    // multi-kilobyte copy before cJSON_PrintUnformatted creates the body.
    cJSON *speech_data = cJSON_CreateStringReference((const char *)encoded);
    if (speech_data == NULL) {
        err = ESP_ERR_NO_MEM;
        goto done;
    }
    cJSON_AddItemToObject(speech, "speech", speech_data);
    cJSON_AddStringToObject(config, "stream_id", stream_id);
    cJSON_AddNumberToObject(config, "sequence_id", sequence_id);
    cJSON_AddNumberToObject(config, "action", action);
    cJSON_AddStringToObject(config, "format", "pcm");
    cJSON_AddStringToObject(config, "engine_type", "16k_auto");
    body = json_print(request);
    request = NULL;
    if (body == NULL) {
        err = ESP_ERR_NO_MEM;
        goto done;
    }
    free(encoded);
    encoded = NULL;
    parser=calloc(1,sizeof(*parser));
    if(!parser) {err=ESP_ERR_NO_MEM;goto done;}
    feishu_result_stream_init(parser,"recognition_text",300u*1024u,recognition,recognition_size,asr_store,&storage);
    int status=0;size_t response_size=0;
    err=feishu_http_post_stream(FEISHU_ASR_STREAM_URL,session->tenant_access_token,strlen(body),
        memory_upload,body,true,64u*1024u*1024u,result_bytes,parser,asr_cancelled,(void *)cancel_requested,&status,&response_size);
    if(err==ESP_OK) {
        bool complete=feishu_result_stream_finish(parser);
        /* Silent interim packets may omit recognition_text; final text must
         * be present and fully validated before its draft can be published. */
        if(!complete && action!=2 && !parser->failed && parser->root && parser->code_seen &&
           parser->code==0 && parser->data_seen && feishu_json_finish(&parser->json))complete=true;
        if(status<200 || status>=300 || !complete)err=ESP_ERR_INVALID_RESPONSE;
    }
    if(err!=ESP_OK || parser->code!=0)ESP_LOGW(TAG,"ASR response HTTP=%d code=%d bytes=%u parsed=%u",status,parser->code,(unsigned)response_size,(unsigned)!parser->failed);
    if(parser->code==99991672 || parser->code==99991679)err=FEISHU_ERR_ACCESS_REJECTED;
    if(status==429 || parser->code==99991400)err=FEISHU_ERR_ASR_LIMITED;
    if(storage.error!=ESP_OK)err=storage.error;

done:
    free(encoded);
    free(body);
    cJSON_Delete(request);
    if(parser) {memset(parser,0,sizeof(*parser));free(parser);}
    if(err!=ESP_OK && final_draft)feishu_cache_text_abort(final_draft);
    return err;
}

void feishu_api_asr_stream_close(void)
{
    feishu_http_stream_close();
}

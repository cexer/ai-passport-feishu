/* Worker-owned XiaoZhi ASR. Only recognition text is consumed: no playback.
 * The live service accepts PCM WebSocket frames. Frame-scaled PCM6 bounds
 * local queue RAM without a heavy DSP encoder or playback task. */
#include "feishu_xiaozhi_protocol.h"
#include "feishu_xiaozhi_audio.h"
#include "cJSON.h"
#include "feishu_cache.h"
#include "nvs.h"
#include "esp_transport_ssl.h"
#include "esp_transport_ws.h"
#include "esp_crt_bundle.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include <lwip/inet.h>
#include <lwip/sockets.h>
#include <errno.h>
#define XIAO_FRAMES FEISHU_XIAO_QUEUE_FRAMES
#define XIAO_FRAME_MS FEISHU_XIAO_FRAME_MS
#define XIAO_SAMPLES FEISHU_XIAO_CAPTURE_SAMPLES
#define XIAO_WIRE_RATE 8000
#define XIAO_PACKET_CAP FEISHU_XIAO_PACKET_CAP
#define XIAO_STACK 4096

typedef struct {char url[512],token[768],uuid[37];int version;} xiao_config_t;
typedef feishu_xiao_packet_t xiao_packet_t;
typedef struct {
    xiao_packet_t packets[XIAO_FRAMES];
    int16_t pcm[XIAO_SAMPLES];
    int16_t upload_pcm[FEISHU_XIAO_QUEUE_SAMPLES];
    uint8_t wire[8+16+FEISHU_XIAO_QUEUE_SAMPLES*sizeof(int16_t)];
    QueueHandle_t free,ready;SemaphoreHandle_t done;TaskHandle_t task;
    volatile bool enabled,shutdown;
    volatile bool *stop,*preparing;const volatile bool *cancel;volatile unsigned *elapsed;
    volatile esp_err_t error;unsigned high,stack_free_min;bool any_sample;
    TickType_t started;
} xiao_capture_t;
typedef struct {char *bytes;size_t size,capacity;} xiao_buffer_t;
static bool xiao_cancel(void *context)
{return context && *(const volatile bool *)context;}
static bool xiao_ascii(const char *s)
{for(;*s;++s)if((unsigned char)*s<32 || (unsigned char)*s>126)return false;return true;}
static bool xiao_upload(void *context,feishu_http_write_t write,void *writer)
{return write(writer,context,strlen(context));}
static esp_err_t xiao_bytes(void *context,const uint8_t *data,size_t size)
{
    xiao_buffer_t *b=context;if(size>=b->capacity-b->size)return ESP_ERR_INVALID_SIZE;
    memcpy(b->bytes+b->size,data,size);b->size+=size;b->bytes[b->size]=0;return ESP_OK;
}
static bool xiao_config_parse(const cJSON *root,xiao_config_t *c)
{
    const cJSON *url=cJSON_GetObjectItemCaseSensitive(root,"url"),*token=cJSON_GetObjectItemCaseSensitive(root,"token"),
        *version=cJSON_GetObjectItemCaseSensitive(root,"version");
    if(!cJSON_IsString(url) || strncmp(url->valuestring,"wss://",6) || strlen(url->valuestring)>=sizeof(c->url) ||
       !xiao_ascii(url->valuestring) || !cJSON_IsString(token) || !*token->valuestring ||
       strlen(token->valuestring)>=sizeof(c->token) || !xiao_ascii(token->valuestring))return false;
    memcpy(c->url,url->valuestring,strlen(url->valuestring)+1);memcpy(c->token,token->valuestring,strlen(token->valuestring)+1);
    c->version=cJSON_IsNumber(version) ? version->valueint:1;
    return c->version>=1 && c->version<=3;
}
static void xiao_uuid(char output[37])
{
    uint8_t b[16];esp_fill_random(b,sizeof(b));b[6]=(b[6]&15)|64;b[8]=(b[8]&63)|128;
    snprintf(output,37,"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],b[8],b[9],b[10],b[11],b[12],b[13],b[14],b[15]);
}
static feishu_asr_pairing_t s_xiao_pairing={.supported=true};
static esp_err_t xiao_config(xiao_config_t *config,const volatile bool *cancel,bool refresh)
{
    char *saved=calloc(1,FEISHU_XIAOZHI_CONFIG_MAX);if(!saved)return ESP_ERR_NO_MEM;
    bool pending=false;
    esp_err_t err=feishu_store_load_xiaozhi(saved,FEISHU_XIAOZHI_CONFIG_MAX);
    if(err==ESP_OK) {
        cJSON *root=cJSON_Parse(saved);const cJSON *id=cJSON_GetObjectItemCaseSensitive(root,"uuid");
        bool identity=cJSON_IsString(id) && strlen(id->valuestring)==36 && xiao_ascii(id->valuestring);
        bool valid=identity && xiao_config_parse(root,config);
        if(identity)memcpy(config->uuid,id->valuestring,37);
        pending=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root,"activation_pending"));
        cJSON_Delete(root);
        if(valid && !pending && !refresh){s_xiao_pairing.paired=true;s_xiao_pairing.code[0]=0;memset(saved,0,FEISHU_XIAOZHI_CONFIG_MAX);free(saved);return ESP_OK;}
    } else if(err!=ESP_ERR_NVS_NOT_FOUND) {free(saved);return err;}
    memset(saved,0,FEISHU_XIAOZHI_CONFIG_MAX);free(saved);
    if(!config->uuid[0])xiao_uuid(config->uuid);
    uint8_t address[6];err=esp_read_mac(address,ESP_MAC_WIFI_STA);if(err!=ESP_OK)return err;
    char mac[18];snprintf(mac,sizeof(mac),"%02x:%02x:%02x:%02x:%02x:%02x",address[0],address[1],address[2],address[3],address[4],address[5]);
    char body[384];snprintf(body,sizeof(body),"{\"version\":2,\"mac_address\":\"%s\",\"uuid\":\"%s\",\"chip_model_name\":\"esp32c3\",\"application\":{\"name\":\"ai-passport-feishu\",\"version\":\"2.2.0\"},\"board\":{\"type\":\"folotoy-ai-passport\",\"name\":\"folotoy-ai-passport\"}}",mac,config->uuid);
    feishu_http_header_t headers[]={{"Activation-Version","1"},{"Device-Id",mac},{"Client-Id",config->uuid},
        {"User-Agent","folotoy-ai-passport/2.2.0"},{"Content-Type","application/json"},{"Accept-Language","zh-CN"}};
    feishu_utf8_copy(s_xiao_pairing.device_id,sizeof(s_xiao_pairing.device_id),mac);
    if(pending && refresh) {
        /* Official activation-v1 uses the same headers and an empty JSON body.
         * 202 is still awaiting user confirmation; never call it paired. */
        char ack[512]={0};xiao_buffer_t reply={.bytes=ack,.capacity=sizeof(ack)};
        int status=0;size_t bytes=0;
        err=feishu_http_post_headers_stream("https://api.tenclass.net/xiaozhi/ota/activate",headers,6,2,xiao_upload,"{}",false,
            sizeof(ack)-1,xiao_bytes,&reply,xiao_cancel,(void *)cancel,&status,&bytes);
        memset(ack,0,sizeof(ack));
        if(err!=ESP_OK)return err;
        /* Expired challenges may return 4xx. The fresh OTA response below,
         * never this acknowledgement alone, determines the pairing state. */
    }
    xiao_buffer_t response={.bytes=calloc(1,4096),.capacity=4096};if(!response.bytes)return ESP_ERR_NO_MEM;
    int status=0;size_t size=0;
    err=feishu_http_post_headers_stream("https://api.tenclass.net/xiaozhi/ota/",headers,6,strlen(body),xiao_upload,body,false,
        response.capacity-1,xiao_bytes,&response,xiao_cancel,(void *)cancel,&status,&size);
    if(err==ESP_OK && status!=200)err=ESP_FAIL;
    if(err==ESP_OK) {
        cJSON *root=cJSON_Parse(response.bytes),*websocket=cJSON_GetObjectItemCaseSensitive(root,"websocket");
        const cJSON *activation=cJSON_GetObjectItemCaseSensitive(root,"activation");
        const cJSON *code=cJSON_GetObjectItemCaseSensitive(activation,"code");
        bool invalid_activation=activation && !cJSON_IsObject(activation);
        bool invalid_code=code && !cJSON_IsString(code);
        int pairing=invalid_activation || invalid_code ? -1:feishu_xiao_pairing_classify(cJSON_IsObject(activation),cJSON_IsString(code) ? code->valuestring:NULL,xiao_config_parse(websocket,config),s_xiao_pairing.code);
        if(pairing<0)err=ESP_ERR_INVALID_RESPONSE;
        else if(pairing>0) {
            s_xiao_pairing.paired=false;
            const cJSON *timeout=cJSON_GetObjectItemCaseSensitive(activation,"timeout_ms");
            uint32_t duration=cJSON_IsNumber(timeout) && timeout->valuedouble>0 && timeout->valuedouble<=3600000 ? (uint32_t)timeout->valuedouble:300000;
            s_xiao_pairing.expires_at_ms=(uint32_t)(esp_timer_get_time()/1000)+duration;
            char identity[128];snprintf(identity,sizeof(identity),"{\"uuid\":\"%s\",\"activation_pending\":true}",config->uuid);
            err=feishu_store_save_xiaozhi(identity);memset(identity,0,sizeof(identity));
            if(err==ESP_OK)err=FEISHU_ERR_XIAO_PAIRING;
        } else {
            cJSON_AddStringToObject(websocket,"uuid",config->uuid);
            char *json=cJSON_PrintUnformatted(websocket);
            err=json ? feishu_store_save_xiaozhi(json):ESP_ERR_NO_MEM;
            if(json){memset(json,0,strlen(json));free(json);}
            if(err==ESP_OK){s_xiao_pairing.paired=true;s_xiao_pairing.expires_at_ms=0;}
        }
        cJSON_Delete(root);
    }
    memset(response.bytes,0,response.capacity);free(response.bytes);
    ESP_LOGI(TAG,"XIAO_CONFIG result=%d http=%d",err,status);return err;
}
static void xiao_capture_task(void *argument)
{
    xiao_capture_t *c=argument;unsigned primed=s_audio_primed;int slot;
    while(primed<ASR_PRIME_MS || !c->enabled) {
        if(c->shutdown || *c->stop || (c->cancel && *c->cancel)){c->error=FEISHU_HTTP_ERR_CANCELLED;break;}
        c->error=bsp_audio_read(c->pcm,sizeof(c->pcm));if(c->error!=ESP_OK)break;
        if(primed<ASR_PRIME_MS)primed+=XIAO_FRAME_MS;
    }
    while(c->error==ESP_OK && !c->shutdown && !(*c->stop && *c->elapsed>=ASR_MIN_MS) && *c->elapsed<ASR_MAX_MS) {
        if(c->cancel && *c->cancel){c->error=FEISHU_HTTP_ERR_CANCELLED;break;}
        if(xQueueReceive(c->free,&slot,pdMS_TO_TICKS(50))!=pdTRUE){c->error=ESP_ERR_TIMEOUT;break;}
        c->error=bsp_audio_read(c->pcm,sizeof(c->pcm));
#ifdef FEISHU_VOICE_AUTOTEST
        if(c->error==ESP_OK)c->error=voice_probe_capture(c->pcm,sizeof(c->pcm),*c->elapsed);
#endif
        for(unsigned i=0;i<XIAO_SAMPLES;++i)if(c->pcm[i]){c->any_sample=true;break;}
        size_t encoded=c->error==ESP_OK ? feishu_xiao_audio_encode(c->pcm,XIAO_SAMPLES,
            &c->packets[slot]):0;
        if(c->error==ESP_OK && encoded!=XIAO_PACKET_CAP)c->error=ESP_ERR_INVALID_SIZE;
        unsigned stack_free=(unsigned)uxTaskGetStackHighWaterMark(NULL);
        if(stack_free<c->stack_free_min)c->stack_free_min=stack_free;
        if(c->error!=ESP_OK){xQueueSend(c->free,&slot,0);break;}
        c->packets[slot].size=(uint16_t)encoded;
        if(!*c->elapsed) {
            if(c->preparing)*c->preparing=false;
            ESP_LOGI(TAG,"ASR_READY provider=xiaozhi startup_ms=%u retained_ms=20",(unsigned)((xTaskGetTickCount()-c->started)*portTICK_PERIOD_MS));
        }
        *c->elapsed+=XIAO_FRAME_MS;xQueueSend(c->ready,&slot,portMAX_DELAY);
        unsigned queued=uxQueueMessagesWaiting(c->ready);if(queued>c->high)c->high=queued;
    }
    slot=c->error==ESP_OK ? -1:-2;xQueueSend(c->ready,&slot,portMAX_DELAY);
    xSemaphoreGive(c->done);vTaskDelete(NULL);
}
static void xiao_capture_destroy(xiao_capture_t **capture,bool started)
{
    xiao_capture_t *c=*capture;if(!c)return;
    if(started) {
        c->shutdown=true;
        /* Drain the queue until its producer has exited; never delete a task
         * blocked in I2S or let it access freed codec/queue/PCM resources. */
        while(xSemaphoreTake(c->done,pdMS_TO_TICKS(50))!=pdTRUE) {
            int slot;if(xQueueReceive(c->ready,&slot,0)==pdTRUE && slot>=0)xQueueSend(c->free,&slot,0);
        }
    }
    if(started)ESP_LOGI(TAG,"XIAO_STACK allocated=%u free_min=%u",XIAO_STACK,c->stack_free_min);
    if(c->done)vSemaphoreDelete(c->done);
    if(c->free)vQueueDelete(c->free);
    if(c->ready)vQueueDelete(c->ready);
    memset(c,0,sizeof(*c));free(c);*capture=NULL;
}
static esp_err_t xiao_control(esp_transport_handle_t ws,const char *session,const char *state)
{
    cJSON *root=cJSON_CreateObject();if(!root)return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(root,"session_id",session);cJSON_AddStringToObject(root,"type",state ? "listen":"abort");
    if(state){cJSON_AddStringToObject(root,"state",state);if(!strcmp(state,"start"))cJSON_AddStringToObject(root,"mode","manual");}
    char *json=cJSON_PrintUnformatted(root);cJSON_Delete(root);if(!json)return ESP_ERR_NO_MEM;
    int n=(int)strlen(json);int sent=esp_transport_ws_send_raw(ws,WS_TRANSPORT_OPCODES_TEXT|WS_TRANSPORT_OPCODES_FIN,json,n,1000);
    free(json);return sent==n ? ESP_OK:ESP_FAIL;
}
/* The SDK sends WS header and payload as separate TLS records and does not
 * finish a positive partial payload write. Send one combined frame instead.
 * A partial failure closes this connection; never resend an ambiguous frame. */
static int xiao_audio_write(esp_transport_handle_t ssl,uint8_t *wire,size_t capacity,size_t size)
{
    uint8_t mask[4];esp_fill_random(mask,sizeof(mask));
    size_t framed=feishu_xiao_binary_frame(wire,capacity,size,mask),offset=0;
    if(!framed)return -1;
    int64_t deadline=esp_timer_get_time()+1000000;
    while(offset<framed) {
        int64_t remaining=deadline-esp_timer_get_time();if(remaining<=0)return 0;
        int n=esp_transport_write(ssl,(char *)wire+offset,(int)(framed-offset),(int)((remaining+999)/1000));
        if(n<=0)return n;
        if((size_t)n>framed-offset)return -1;
        offset+=(size_t)n;
    }
    return (int)size;
}
typedef struct {feishu_xiao_message_t message;int remaining;bool binary,fragment;} xiao_receive_t;
static int xiao_receive(esp_transport_handle_t ws,xiao_receive_t *r,int wait)
{
    if(!r->remaining) {
        int ready=esp_transport_poll_read(ws,wait);if(ready<0)return -1;if(!ready)return XIAO_MESSAGE_OTHER;
    }
    char bytes[256];int n=esp_transport_read(ws,bytes,sizeof(bytes),1000);
    int opcode=esp_transport_ws_get_read_opcode(ws);
    if(opcode==WS_TRANSPORT_OPCODES_CLOSE || n<0)return -1;
    if(!r->remaining) {
        if(opcode==WS_TRANSPORT_OPCODES_TEXT) {feishu_xiao_message_init(&r->message);r->binary=false;}
        else if(opcode==WS_TRANSPORT_OPCODES_BINARY)r->binary=true;
        else if(opcode!=WS_TRANSPORT_OPCODES_CONT)return XIAO_MESSAGE_OTHER;
        r->remaining=esp_transport_ws_get_read_payload_len(ws);
    }
    if(n>r->remaining)return -1;
    if(!r->binary && n>0 && !feishu_xiao_message_feed(&r->message,bytes,(size_t)n))return -1;
    r->remaining-=n;
    if(!r->remaining && esp_transport_ws_get_fin_flag(ws) && !r->binary)return feishu_xiao_message_finish(&r->message);
    return XIAO_MESSAGE_OTHER;
}
static esp_err_t xiao_ws_open(xiao_config_t *cfg,esp_transport_handle_t *ssl,esp_transport_handle_t *ws)
{
    const char *begin=cfg->url+6,*path=strchr(begin,'/');size_t host_size=path ? (size_t)(path-begin):strlen(begin);
    char host[128];if(!host_size || host_size>=sizeof(host))return ESP_ERR_INVALID_ARG;
    memcpy(host,begin,host_size);host[host_size]=0;
    for(size_t i=0;i<host_size;++i)if(!((host[i]>='a'&&host[i]<='z') || (host[i]>='A'&&host[i]<='Z') ||
        (host[i]>='0'&&host[i]<='9') || host[i]=='.' || host[i]=='-'))return ESP_ERR_INVALID_ARG;
    *ssl=esp_transport_ssl_init();if(!*ssl)return ESP_ERR_NO_MEM;
    esp_transport_ssl_crt_bundle_attach(*ssl,esp_crt_bundle_attach);
    *ws=esp_transport_ws_init(*ssl);if(!*ws)return ESP_ERR_NO_MEM;
    esp_transport_ws_set_path(*ws,path ? path:"/");
    uint8_t a[6];esp_read_mac(a,ESP_MAC_WIFI_STA);char mac[18];snprintf(mac,sizeof(mac),"%02x:%02x:%02x:%02x:%02x:%02x",a[0],a[1],a[2],a[3],a[4],a[5]);
    size_t capacity=strlen(cfg->token)+256;char *headers=malloc(capacity);if(!headers)return ESP_ERR_NO_MEM;
    snprintf(headers,capacity,"Authorization: %s%s\r\nProtocol-Version: %d\r\nDevice-Id: %s\r\nClient-Id: %s\r\n",
        strchr(cfg->token,' ') ? "":"Bearer ",cfg->token,cfg->version,mac,cfg->uuid);
    esp_err_t err=esp_transport_ws_set_headers(*ws,headers);memset(headers,0,capacity);free(headers);
    if(err!=ESP_OK)return err;
    if(esp_transport_connect(*ws,host,443,10000)!=0)return ESP_FAIL;
    /* Audio is a stream of small TLS records. Do not defer its WS header
     * behind Nagle while waiting for an ACK to an earlier audio fragment. */
    int fd=esp_transport_get_socket(*ssl),enabled=1;
    int rc=setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&enabled,sizeof(enabled));
    ESP_LOGI(TAG,"XIAO_SOCKET nodelay=%u result=%d errno=%d",(unsigned)(rc==0),rc,rc<0?errno:0);
    return rc==0 ? ESP_OK:ESP_FAIL;
}
static void xiao_ws_close(esp_transport_handle_t *ws,esp_transport_handle_t *ssl)
{
    if(*ws){esp_transport_close(*ws);esp_transport_destroy(*ws);*ws=NULL;}
    if(*ssl){esp_transport_destroy(*ssl);*ssl=NULL;}
}
static void __attribute__((noinline)) xiao_log_network(void)
{
    wifi_ap_record_t ap={0};esp_err_t result=esp_wifi_sta_get_ap_info(&ap);
    ESP_LOGI(TAG,"XIAO_NETWORK result=%d rssi=%d channel=%u wire_rate=%u",result,ap.rssi,(unsigned)ap.primary,XIAO_WIRE_RATE);
}
static esp_err_t feishu_asr_record_xiaozhi(volatile bool *stop,const volatile bool *cancel,volatile unsigned *elapsed,
    char *recognition,size_t capacity,feishu_data_writer_t *draft,volatile bool *preparing)
{
    if(!stop || !elapsed || !recognition || !capacity)return ESP_ERR_INVALID_ARG;
    recognition[0]=0;*elapsed=0;if(preparing)*preparing=true;
    xiao_log_network();
    ESP_LOGI(TAG,"ASR_MEMORY stage=start provider=xiaozhi heap=%u minimum=%u",(unsigned)esp_get_free_heap_size(),(unsigned)esp_get_minimum_free_heap_size());
    xiao_config_t *cfg=calloc(1,sizeof(*cfg));xiao_capture_t *c=calloc(1,sizeof(*c));
    xiao_receive_t *receiver=calloc(1,sizeof(*receiver));
    esp_transport_handle_t ssl=NULL,ws=NULL;bool started=false,audio=false;
    esp_err_t err=ESP_ERR_NO_MEM;char session[96]={0};unsigned frames=0;
    const char *stage="allocate";
    if(!cfg || !c || !receiver)goto done;
    stage="config";err=xiao_config(cfg,cancel,false);if(err!=ESP_OK)goto done;
    c->stack_free_min=XIAO_STACK;
    c->stop=stop;c->cancel=cancel;c->elapsed=elapsed;c->preparing=preparing;c->started=xTaskGetTickCount();
    c->free=xQueueCreate(XIAO_FRAMES,sizeof(int));c->ready=xQueueCreate(XIAO_FRAMES+1,sizeof(int));c->done=xSemaphoreCreateBinary();
    if(!c->free || !c->ready || !c->done)goto done;
    stage="audio_init";err=bsp_audio_init();if(err!=ESP_OK)goto done;audio=true;bsp_audio_set_volume(0);
    err=bsp_audio_set_format(16000,16,1);if(err!=ESP_OK)goto done;
    if(xTaskCreate(xiao_capture_task,"xiaozhi_capture",XIAO_STACK,c,6,&c->task)!=pdPASS){err=ESP_ERR_NO_MEM;goto done;}
    started=true;
    /* A settled microphone can retain speech immediately while TLS connects.
     * The existing 2.4-second queue bounds that backlog; never drop old frames. */
    bool early=s_audio_primed>=ASR_PRIME_MS;
    if(early){for(int i=0;i<XIAO_FRAMES;++i)xQueueSend(c->free,&i,0);c->enabled=true;}
    stage="connect";
    for(unsigned attempt=0;attempt<2;++attempt) {
        err=xiao_ws_open(cfg,&ssl,&ws);
        if(err==ESP_OK)break;
        xiao_ws_close(&ws,&ssl);
        if(attempt || (cancel&&*cancel))goto done;
        err=xiao_config(cfg,cancel,true);if(err!=ESP_OK)goto done;
    }
    const char hello[]="{\"type\":\"hello\",\"version\":1,\"transport\":\"websocket\",\"features\":{\"mcp\":false},\"audio_params\":{\"format\":\"pcm\",\"sample_rate\":8000,\"channels\":1,\"frame_duration\":20}}";
    stage="hello";
    char welcome[sizeof(hello)+8];snprintf(welcome,sizeof(welcome),"%s",hello);
    /* The OTA-provided version controls the envelope and hello consistently. */
    char *version=strstr(welcome,"\"version\":1");if(version)version[10]=(char)('0'+cfg->version);
    if(esp_transport_ws_send_raw(ws,WS_TRANSPORT_OPCODES_TEXT|WS_TRANSPORT_OPCODES_FIN,welcome,(int)strlen(welcome),1000)!=(int)strlen(welcome)){err=ESP_FAIL;goto done;}
    int64_t deadline=esp_timer_get_time()+10000000;
    while(esp_timer_get_time()<deadline) {
        if(cancel && *cancel){err=FEISHU_HTTP_ERR_CANCELLED;goto done;}
        int kind=xiao_receive(ws,receiver,100);if(kind<0){err=ESP_ERR_INVALID_RESPONSE;goto done;}
        if(kind==XIAO_MESSAGE_HELLO){memcpy(session,receiver->message.session,sizeof(session));break;}
    }
    if(!session[0]){err=ESP_ERR_TIMEOUT;goto done;}
    ESP_LOGI(TAG,"ASR_CAPTURE codec=pcm6 wire=pcm8 queue_rate=8000 frames=%u capacity_ms=%u audio_ram=%u heap=%u largest=%u",
        XIAO_FRAMES,XIAO_FRAMES*XIAO_FRAME_MS,(unsigned)sizeof(*c),(unsigned)esp_get_free_heap_size(),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    if(!early)for(int i=0;i<XIAO_FRAMES;++i)xQueueSend(c->free,&i,0);
    stage="listen_start";err=xiao_control(ws,session,"start");if(err!=ESP_OK)goto done;c->enabled=true;
    stage="capture";
    for(;;) {
        if(cancel && *cancel){err=FEISHU_HTTP_ERR_CANCELLED;goto done;}
        if(c->error!=ESP_OK){err=c->error;goto done;}
        int slot;if(xQueueReceive(c->ready,&slot,pdMS_TO_TICKS(30))==pdTRUE) {
            if(slot<0){err=slot==-1?ESP_OK:c->error;break;}
#ifdef FEISHU_VOICE_AUTOTEST
            voice_probe_upload_delay();
#endif
            if(!feishu_xiao_audio_decode(&c->packets[slot],c->upload_pcm,FEISHU_XIAO_QUEUE_SAMPLES)) {
                xQueueSend(c->free,&slot,0);err=ESP_ERR_INVALID_RESPONSE;goto done;
            }
            /* Capture stays 16 kHz. The queue already holds 8 kHz, matching
             * the existing wire; no duration or backlog capacity is reduced. */
            size_t header=cfg->version==2 ? 16:cfg->version==3 ? 4:0,n=sizeof(c->upload_pcm);
            memset(c->wire,0,header);
            if(cfg->version==2) {c->wire[1]=2;uint32_t size=htonl((uint32_t)n);memcpy(c->wire+12,&size,4);}
            if(cfg->version==3) {uint16_t size=htons((uint16_t)n);memcpy(c->wire+2,&size,2);}
            memcpy(c->wire+header,c->upload_pcm,n);
            int64_t send_started=esp_timer_get_time();errno=0;
            int sent=c->error==ESP_OK ? xiao_audio_write(ssl,c->wire,sizeof(c->wire),header+n):-1;
            int send_errno=errno;unsigned send_ms=(unsigned)((esp_timer_get_time()-send_started)/1000);
            if(sent!=(int)(header+n) || send_ms>100)ESP_LOGW(TAG,"XIAO_UPLOAD frame=%u sent=%d expected=%u errno=%d ms=%u capture_err=%d queued=%u",
                frames,sent,(unsigned)(header+n),send_errno,send_ms,c->error,(unsigned)uxQueueMessagesWaiting(c->ready));
            xQueueSend(c->free,&slot,0);if(sent!=(int)(header+n)){stage="upload";err=c->error==ESP_OK?ESP_FAIL:c->error;goto done;}
            ++frames;
        }
        int kind=xiao_receive(ws,receiver,0);if(kind<0 || kind==XIAO_MESSAGE_ALERT || kind==XIAO_MESSAGE_STT){err=ESP_ERR_INVALID_RESPONSE;goto done;}
    }
    if(err!=ESP_OK)goto done;
    stage="listen_stop";err=xiao_control(ws,session,"stop");if(err!=ESP_OK)goto done;
    if(!c->any_sample){err=ESP_ERR_NOT_FOUND;goto done;}
    int64_t stopped=esp_timer_get_time();
    ESP_LOGI(TAG,"ASR_QUEUE provider=xiaozhi high_water=%u ms=%u frames_sent=%u",c->high,c->high*XIAO_FRAME_MS,frames);
    xiao_capture_destroy(&c,started);started=false;bsp_audio_deinit();audio=false;
    stage="recognition";deadline=esp_timer_get_time()+15000000;
    while(esp_timer_get_time()<deadline) {
        if(cancel && *cancel){err=FEISHU_HTTP_ERR_CANCELLED;goto done;}
        int kind=xiao_receive(ws,receiver,100);if(kind<0 || kind==XIAO_MESSAGE_ALERT){err=ESP_ERR_INVALID_RESPONSE;goto done;}
        if(kind==XIAO_MESSAGE_STT) {
            size_t n=strlen(receiver->message.text);
            if(!n){err=ESP_ERR_NOT_FOUND;goto done;}
            if(!draft && n>=capacity){err=ESP_ERR_INVALID_SIZE;goto done;}
            feishu_utf8_copy(recognition,capacity,receiver->message.text);
            (void)xiao_control(ws,session,NULL);xiao_ws_close(&ws,&ssl);
            if(draft) {
                err=feishu_cache_draft_begin(draft);
                if(err==ESP_OK)err=feishu_cache_text_append(draft,receiver->message.text,n);
                if(err!=ESP_OK)recognition[0]=0;
            }
            ESP_LOGI(TAG,"XIAO_STT result=%d text_bytes=%u after_stop_ms=%u",err,(unsigned)n,(unsigned)((esp_timer_get_time()-stopped)/1000));
            goto done;
        }
    }
    err=ESP_ERR_TIMEOUT;
done:
    ESP_LOGI(TAG,"XIAO_RESULT stage=%s result=%d frames=%u elapsed=%u capture_err=%d",stage,err,frames,*elapsed,c?c->error:0);
    xiao_capture_destroy(&c,started);if(audio || s_audio_primed)bsp_audio_deinit();xiao_ws_close(&ws,&ssl);
    if(cfg){memset(cfg,0,sizeof(*cfg));free(cfg);}
    if(receiver){memset(receiver,0,sizeof(*receiver));free(receiver);}
    if(preparing)*preparing=false;
    ESP_LOGI(TAG,"ASR_MEMORY stage=finished provider=xiaozhi result=%d heap=%u minimum=%u",err,
        (unsigned)esp_get_free_heap_size(),(unsigned)esp_get_minimum_free_heap_size());return err;
}

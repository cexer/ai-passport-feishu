/* Worker-owned XiaoZhi ASR, plus an explicitly selected assistant voice turn.
 * Ordinary recognition consumes text only and aborts without playback.
 * The live service accepts PCM WebSocket frames. Frame-scaled PCM6 bounds
 * local queue RAM without a heavy DSP encoder or playback task. */
#include "feishu_xiaozhi_protocol.h"
#include "feishu_xiaozhi_audio.h"
#include "feishu_assistant.h"
#include "feishu_assistant_policy.h"
#include "feishu_xiaozhi_downlink.h"
#include "feishu_xiaozhi_player.h"
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
#define XIAO_WIRE_RATE 16000
#define XIAO_PACKET_CAP FEISHU_XIAO_PACKET_CAP
#define XIAO_STACK 4096

typedef struct {char url[512],token[768],uuid[37];int version;} xiao_config_t;
typedef feishu_xiao_packet_t xiao_packet_t;
typedef struct {
    xiao_packet_t *packets;
    _Atomic(xiao_packet_t *) extra_packets;
    int16_t pcm[XIAO_SAMPLES];
    int16_t upload_pcm[FEISHU_XIAO_QUEUE_SAMPLES];
    uint8_t wire[8+16+FEISHU_XIAO_QUEUE_SAMPLES*4];
    QueueHandle_t free,ready;SemaphoreHandle_t done;TaskHandle_t task;
    volatile bool enabled,shutdown;atomic_bool ready_for_voice,voice_started;
    volatile bool *stop,*preparing;const volatile bool *cancel;volatile unsigned *elapsed;
    volatile esp_err_t error;unsigned high,stack_free_min;bool any_sample;
    TickType_t started;
    bool continuous;
    volatile bool *speech_detected;
    unsigned silence_seconds,command_ms;
    unsigned (*get_silence)(void *);void *silence_context;
    int preroll[6];unsigned preroll_count;
    feishu_assistant_vad_t vad;
} xiao_capture_t;
static xiao_packet_t *xiao_capture_packet(xiao_capture_t *c,unsigned slot)
{
    if(slot<FEISHU_XIAO_IDLE_FRAMES)return c->packets ? &c->packets[slot]:NULL;
    xiao_packet_t *extra=atomic_load_explicit(&c->extra_packets,memory_order_acquire);
    return extra && slot<XIAO_FRAMES ? &extra[slot-FEISHU_XIAO_IDLE_FRAMES]:NULL;
}
static bool xiao_capture_allocate(xiao_capture_t *c,bool full)
{
    c->packets=calloc(FEISHU_XIAO_IDLE_FRAMES,sizeof(*c->packets));
    if(!c->packets)return false;
    if(full){
        xiao_packet_t *extra=calloc(XIAO_FRAMES-FEISHU_XIAO_IDLE_FRAMES,sizeof(*extra));
        if(!extra)return false;
        atomic_store_explicit(&c->extra_packets,extra,memory_order_release);
    }
    return true;
}
static bool xiao_capture_promote(xiao_capture_t *c)
{
    if(atomic_load_explicit(&c->extra_packets,memory_order_acquire))return true;
    xiao_packet_t *extra=calloc(XIAO_FRAMES-FEISHU_XIAO_IDLE_FRAMES,sizeof(*extra));
    if(!extra)return false;
    atomic_store_explicit(&c->extra_packets,extra,memory_order_release);
    for(int i=FEISHU_XIAO_IDLE_FRAMES;i<XIAO_FRAMES;++i)xQueueSend(c->free,&i,0);
    ESP_LOGI(TAG,"XIAO_QUEUE_PROMOTE frames=%u idle=%u extra_bytes=%u",XIAO_FRAMES,FEISHU_XIAO_IDLE_FRAMES,
        (unsigned)((XIAO_FRAMES-FEISHU_XIAO_IDLE_FRAMES)*sizeof(*extra)));
    return true;
}
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
    xiao_capture_t *c=argument;unsigned primed=s_audio_primed;int slot;bool published_ready=false;
    while(primed<ASR_PRIME_MS || !c->enabled) {
        if(c->shutdown || *c->stop || (c->cancel && *c->cancel)){c->error=FEISHU_HTTP_ERR_CANCELLED;break;}
        c->error=bsp_audio_read(c->pcm,sizeof(c->pcm));if(c->error!=ESP_OK)break;
        if(primed<ASR_PRIME_MS)primed+=XIAO_FRAME_MS;
    }
    while(c->error==ESP_OK && !c->shutdown && !(*c->stop && (c->continuous || *c->elapsed>=ASR_MIN_MS)) &&
          (c->continuous || *c->elapsed<ASR_MAX_MS)) {
        if(c->cancel && *c->cancel){c->error=FEISHU_HTTP_ERR_CANCELLED;break;}
        if(xQueueReceive(c->free,&slot,pdMS_TO_TICKS(50))!=pdTRUE){
            ESP_LOGE(TAG,"ASR_BACKPRESSURE heard=%u ready=%u held=%u high=%u",(unsigned)c->vad.heard,
                (unsigned)uxQueueMessagesWaiting(c->ready),c->preroll_count,c->high);
            c->error=FEISHU_ERR_AUDIO_BACKPRESSURE;break;
        }
        c->error=bsp_audio_read(c->pcm,sizeof(c->pcm));
#ifdef FEISHU_VOICE_AUTOTEST
        if(c->error==ESP_OK)c->error=voice_probe_capture(c->pcm,sizeof(c->pcm),*c->elapsed);
#endif
        for(unsigned i=0;i<XIAO_SAMPLES;++i)if(c->pcm[i]){c->any_sample=true;break;}
        if(c->continuous && c->error==ESP_OK){
            /* Keep PCM in the 120ms preroll; suppress only transient VAD
             * activation during a short key chime, not the user's audio. */
            bool quiet=atomic_load(&s_feedback_quiet_active) &&
                (int32_t)(atomic_load(&s_feedback_quiet_until)-(unsigned)(esp_timer_get_time()/1000))>0;
            if(!quiet)atomic_store(&s_feedback_quiet_active,false);
            if(!quiet && feishu_assistant_vad_frame(&c->vad,c->pcm,XIAO_SAMPLES,c->get_silence ? c->get_silence(c->silence_context):c->silence_seconds))*c->stop=true;
            if(c->vad.heard)atomic_store(&c->voice_started,true);
            if(c->speech_detected && c->vad.heard)*c->speech_detected=true;
            if(c->vad.heard && c->command_ms<UINT32_MAX-XIAO_FRAME_MS)c->command_ms+=XIAO_FRAME_MS;
        }
        size_t encoded=c->error==ESP_OK ? feishu_xiao_audio_encode(c->pcm,XIAO_SAMPLES,
            xiao_capture_packet(c,(unsigned)slot)):0;
        if(c->error==ESP_OK && encoded!=XIAO_PACKET_CAP)c->error=ESP_ERR_INVALID_SIZE;
        unsigned stack_free=(unsigned)uxTaskGetStackHighWaterMark(NULL);
        if(stack_free<c->stack_free_min)c->stack_free_min=stack_free;
        if(c->error!=ESP_OK){xQueueSend(c->free,&slot,0);break;}
        if(!published_ready){atomic_store(&c->ready_for_voice,true);published_ready=true;}
        if(!*c->elapsed) {
            if(c->preparing)*c->preparing=false;
            ESP_LOGI(TAG,"ASR_READY provider=xiaozhi startup_ms=%u retained_ms=20",(unsigned)((xTaskGetTickCount()-c->started)*portTICK_PERIOD_MS));
        }
        if(*c->elapsed<UINT32_MAX-XIAO_FRAME_MS)*c->elapsed+=XIAO_FRAME_MS;
        /* While idle, the producer owns only the newest 120ms. Quiet DMA
         * frames never fill the upload queue while the worker polls WSS or
         * paints/handles business work. Once speech starts, publish the held
         * prefix in order, then retain every spoken frame without overwriting. */
        if(c->continuous && !c->vad.heard){
            if(c->preroll_count==6){
                xQueueSend(c->free,&c->preroll[0],0);
                memmove(c->preroll,c->preroll+1,5*sizeof(c->preroll[0]));--c->preroll_count;
            }
            c->preroll[c->preroll_count++]=slot;continue;
        }
        if(c->continuous && c->preroll_count){
            ESP_LOGI(TAG,"ASR_PREROLL_PUBLISH frames=%u",c->preroll_count);
            for(unsigned i=0;i<c->preroll_count;++i)xQueueSend(c->ready,&c->preroll[i],portMAX_DELAY);
            c->preroll_count=0;
        }
        xQueueSend(c->ready,&slot,portMAX_DELAY);
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
    if(c->packets){memset(c->packets,0,FEISHU_XIAO_IDLE_FRAMES*sizeof(*c->packets));free(c->packets);}
    xiao_packet_t *extra=atomic_load_explicit(&c->extra_packets,memory_order_acquire);
    if(extra){memset(extra,0,(XIAO_FRAMES-FEISHU_XIAO_IDLE_FRAMES)*sizeof(*extra));free(extra);}
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
    /* Below the 2.4s capture budget: tolerate a delayed ACK without growing
     * the queue. Never replay any ambiguous frame after this deadline. */
    int64_t deadline=esp_timer_get_time()+2000000;
    while(offset<framed) {
        int64_t remaining=deadline-esp_timer_get_time();if(remaining<=0)return 0;
        int n=esp_transport_write(ssl,(char *)wire+offset,(int)(framed-offset),(int)((remaining+999)/1000));
        if(n<=0){
            ESP_LOGW(TAG,"XIAO_WRITE result=%d offset=%u total=%u errno=%d remaining_ms=%u",n,
                (unsigned)offset,(unsigned)framed,errno,(unsigned)(remaining/1000));return n;
        }
        if((size_t)n>framed-offset)return -1;
        offset+=(size_t)n;
    }
    return (int)size;
}
typedef struct xiao_receive xiao_receive_t;
typedef struct {
    const feishu_assistant_hooks_t *hooks;
    char *request;
    char session[96];
    size_t used,capacity;uint32_t received_bytes;int64_t last_payload_at;
    feishu_xiao_downlink_t downlink;
    feishu_xiao_player_t *player;
    const volatile bool *cancel;
    bool can_play,audio,done,initialized,listed;
    bool fixed_audio,confirm_wait,business_failed,business_busy,voice_priority;
    xiao_capture_t *capture; /* Owner only; producer publishes voice_started atomically. */
    xiao_receive_t *business_receiver;
    esp_transport_handle_t business_ws;int64_t business_ping_at;
    int16_t resampled[320];
    unsigned calls,audio_frames;
 } xiao_assistant_t;
struct xiao_receive {feishu_xiao_message_t message;int remaining,deferred_kind;bool binary,fragment;xiao_assistant_t *assistant;};
static int xiao_receive(esp_transport_handle_t ws,xiao_receive_t *r,int wait);
static esp_err_t xiao_assistant_pcm(void *context,const int16_t *pcm,size_t samples)
{
    xiao_assistant_t *a=context;
    if(!a->audio) {
        esp_err_t err=bsp_audio_init();if(err!=ESP_OK)return err;
        a->audio=true;bsp_audio_set_volume((uint8_t)(a->hooks->get_volume ? a->hooks->get_volume(a->hooks->context):a->hooks->volume));
        err=bsp_audio_set_format(24000,16,1);if(err!=ESP_OK)return err;
    }
    if(a->fixed_audio){
        size_t n=feishu_assistant_resample(pcm,samples,a->resampled,320);
        return n ? bsp_audio_write(a->resampled,n*sizeof(int16_t)):ESP_ERR_INVALID_SIZE;
    }
    return bsp_audio_write(pcm,samples*sizeof(*pcm));
}
static bool xiao_assistant_opus(void *context,uint8_t *packet,size_t size)
{
    xiao_assistant_t *a=context;
    if(!a->player) {
        esp_err_t err=feishu_xiao_player_open(&a->player,24000,1,20,xiao_assistant_pcm,a,a->cancel);
        if(err!=ESP_OK) {
            ESP_LOGW(TAG,"ASSISTANT_PLAYER_OPEN result=%d heap=%u largest=%u",err,(unsigned)esp_get_free_heap_size(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
            return false;
        }
    }
    if(feishu_xiao_player_packet(a->player,packet,size)!=ESP_OK)return false;
    if(!a->audio_frames)ESP_LOGI(TAG,"ASSISTANT_AUDIO first_frame=1 heap=%u stack=%u",(unsigned)esp_get_free_heap_size(),(unsigned)uxTaskGetStackHighWaterMark(NULL));
    ++a->audio_frames;return true;
}
static bool xiao_business_progress(void *context)
{
    xiao_assistant_t *a=context;
    if(a->cancel && *a->cancel)return false;
    if(a->capture && atomic_load(&a->capture->voice_started)){
        a->voice_priority=true;return false; /* Cancel only the current read; retain every queued voice frame. */
    }
    /* A voice socket failure must not cancel an independent Feishu request. */
    if(a->business_failed)return true;
    xiao_receive_t *r=a->business_receiver;
    if(a->business_busy && r && !r->deferred_kind){
        for(unsigned i=0;i<2;++i){
            int ready=esp_transport_poll_read(a->business_ws,0);
            if(ready<0){a->business_failed=true;ESP_LOGW(TAG,"ASSISTANT_BUSINESS_RECEIVE poll_failed=1 errno=%d",errno);break;}
            if(!ready && !r->remaining)break;
            if(xiao_receive(a->business_ws,r,0)<0){a->business_failed=true;ESP_LOGW(TAG,"ASSISTANT_BUSINESS_RECEIVE read_failed=1 errno=%d last_type=%s",errno,r->message.type);break;}
            unsigned opcode=esp_transport_ws_get_read_opcode(a->business_ws);
            ESP_LOGI(TAG,"ASSISTANT_BUSINESS_PUMP ready=1 opcode=%u",opcode);
            if(opcode==WS_TRANSPORT_OPCODES_PING || opcode==WS_TRANSPORT_OPCODES_PONG)
                ESP_LOGI(TAG,"ASSISTANT_BUSINESS_CONTROL opcode=%u",opcode);
            if(r->deferred_kind)break;
        }
    }
    int64_t now=esp_timer_get_time();
    if(!a->business_failed && a->business_ws && now-a->business_ping_at>=15000000){
        if(esp_transport_ws_send_raw(a->business_ws,WS_TRANSPORT_OPCODES_PING|WS_TRANSPORT_OPCODES_FIN,"",0,1000)<0){
            a->business_failed=true;ESP_LOGW(TAG,"ASSISTANT_BUSINESS_KEEPALIVE failed=1");return true;
        }
        a->business_ping_at=now;
        if(a->hooks->waiting_confirmation && a->hooks->waiting_confirmation(a->hooks->context))ESP_LOGI(TAG,"ASSISTANT_CONFIRM_KEEPALIVE sent=1");
        else ESP_LOGI(TAG,"ASSISTANT_BUSINESS_KEEPALIVE sent=1");
    }
    return true;
}
static bool xiao_continue_business(xiao_assistant_t *a)
{
    xiao_receive_t *r=a->business_receiver;
    for(unsigned pass=0;pass<4;++pass){
        a->business_busy=true;feishu_http_set_progress(xiao_business_progress,a);
        feishu_http_set_read_preemptible(a->capture!=NULL);
        bool ok=xiao_business_progress(a) && (!a->hooks->continue_session || a->hooks->continue_session(a->hooks->context));
        feishu_http_set_progress(NULL,NULL);
        if(a->voice_priority){a->business_busy=false;a->voice_priority=false;
            ESP_LOGI(TAG,"ASSISTANT_VOICE_PRIORITY read_yielded=1 capture_retained=1");return true;}
        int64_t deadline=esp_timer_get_time()+2000000;
        while(ok && !a->business_failed && r && !r->deferred_kind && (r->remaining || r->fragment)){
            if((a->cancel && *a->cancel) || esp_timer_get_time()>deadline || xiao_receive(a->business_ws,r,20)<0){a->business_failed=true;break;}
        }
        a->business_busy=false;
        if(!ok || a->business_failed)return false;
        if(!r || !r->deferred_kind)return true;
        /* Dispatch only after the Feishu/TLS call stack has unwound. No nested
         * RPC or second HTTP request may run from the progress callback. */
        bool playing=a->can_play;a->can_play=true;
        int kind=xiao_receive(a->business_ws,r,0);a->can_play=playing;
        if(kind<0){a->business_failed=true;return false;}
    }
    a->business_failed=true;return false;
}
static bool xiao_assistant_execute(void *context,const char *name,const cJSON *arguments,char *text,size_t capacity)
{
    xiao_assistant_t *a=context;
    if(!a->can_play || !a->initialized || !a->listed || (a->cancel && *a->cancel)) {
        snprintf(text,capacity,"Voice capture is active or cancelled; no action performed.");return false;
    }
    if(a->hooks->event)a->hooks->event(a->hooks->context,FEISHU_ASSISTANT_TOOL,name);
    /* A model may speak a preamble before calling a tool. Do not keep its
     * Opus decoder beside a second TLS connection during tool execution. */
    if(a->player){feishu_xiao_player_close(&a->player);ESP_LOGI(TAG,"ASSISTANT_TOOL_PLAYER_RELEASED heap=%u",(unsigned)esp_get_free_heap_size());}
    /* cJSON owns the validated argument strings now. The original wire JSON
     * is no longer needed during a nested Feishu HTTPS request. */
    free(a->request);a->request=NULL;a->capacity=a->used=0;
    ESP_LOGI(TAG,"ASSISTANT_TOOL_BEGIN name=%s heap=%u stack=%u",name,(unsigned)esp_get_free_heap_size(),(unsigned)uxTaskGetStackHighWaterMark(NULL));
    feishu_http_set_progress(xiao_business_progress,a);
    feishu_http_set_read_preemptible(a->capture!=NULL);
    bool ok=a->hooks->execute(a->hooks->context,name,arguments,text,capacity);
    feishu_http_set_progress(NULL,NULL);
    ESP_LOGI(TAG,"ASSISTANT_TOOL_END name=%s ok=%u heap=%u stack=%u",name,(unsigned)ok,(unsigned)esp_get_free_heap_size(),(unsigned)uxTaskGetStackHighWaterMark(NULL));
    return ok;
}
static bool xiao_assistant_message(esp_transport_handle_t ws,xiao_assistant_t *a,int kind,const char *type_name)
{
    if(kind==XIAO_MESSAGE_HELLO) {
        cJSON *root=cJSON_Parse(a->request);if(!root)return false;
        const cJSON *params=cJSON_GetObjectItemCaseSensitive(root,"audio_params");
        const cJSON *rate=cJSON_GetObjectItemCaseSensitive(params,"sample_rate"),
            *channels=cJSON_GetObjectItemCaseSensitive(params,"channels"),
            *duration=cJSON_GetObjectItemCaseSensitive(params,"frame_duration"),
            *format=cJSON_GetObjectItemCaseSensitive(params,"format");
        bool valid=cJSON_IsString(format) && !strcmp(format->valuestring,"opus") &&
            cJSON_IsNumber(rate) && rate->valueint==24000 && cJSON_IsNumber(channels) && channels->valueint==1 &&
            cJSON_IsNumber(duration) && duration->valueint==20;
        cJSON_Delete(root);return valid;
    }
    if(!strcmp(type_name,"mcp")) {
        char *reply=calloc(1,FEISHU_MCP_OUTPUT_MAX);if(!reply)return false;
        feishu_mcp_result_t result=feishu_mcp_dispatch(a->request,a->used,a->session,&a->hooks->status,
            reply,FEISHU_MCP_OUTPUT_MAX,xiao_assistant_execute,a);
        if(result==FEISHU_MCP_IGNORE){free(reply);return true;}
        bool sent=a->session[0] && reply[0] &&
            esp_transport_ws_send_raw(ws,WS_TRANSPORT_OPCODES_TEXT|WS_TRANSPORT_OPCODES_FIN,reply,(int)strlen(reply),1000)==(int)strlen(reply);
        bool final_tools=result==FEISHU_MCP_LISTED && !strstr(reply,"\"nextCursor\"");
        free(reply);if(!sent)return false;
        a->initialized|=result==FEISHU_MCP_INITIALIZED;a->listed|=final_tools;
        if(result==FEISHU_MCP_TOOL)++a->calls;
        ESP_LOGI(TAG,"ASSISTANT_RPC kind=%u cloud_calls=%u heap=%u",(unsigned)result,a->calls,(unsigned)esp_get_free_heap_size());
        return true;
    }
    if(strcmp(type_name,"tts"))return true;
    cJSON *root=cJSON_Parse(a->request);if(!root)return false;
    const cJSON *type=cJSON_GetObjectItemCaseSensitive(root,"type"),*state=cJSON_GetObjectItemCaseSensitive(root,"state"),
        *text=cJSON_GetObjectItemCaseSensitive(root,"text");
    if(cJSON_IsString(type) && !strcmp(type->valuestring,"tts") && cJSON_IsString(state)) {
        /* A late playback-end packet while capture runs is not the end of
         * the command that has not even finished uploading yet. Abort ACKs
         * during reply wait still work because capture has been released. */
        if(!a->capture && !strcmp(state->valuestring,"stop"))a->done=true;
        if(a->can_play && !strcmp(state->valuestring,"sentence_start") && cJSON_IsString(text) && text->valuestring[0] && a->hooks->event)
            a->hooks->event(a->hooks->context,FEISHU_ASSISTANT_REPLY,text->valuestring);
    }
    cJSON_Delete(root);return true;
}
static int xiao_receive(esp_transport_handle_t ws,xiao_receive_t *r,int wait)
{
    if(r->deferred_kind && r->assistant && !r->assistant->business_busy){
        xiao_assistant_t *a=r->assistant;int kind=r->deferred_kind-1;r->deferred_kind=0;
        bool valid=xiao_assistant_message(ws,a,kind,r->message.type);
        free(a->request);a->request=NULL;a->capacity=a->used=0;
        return valid ? kind:-1;
    }
    if(!r->remaining) {
        int ready=esp_transport_poll_read(ws,wait);if(ready<0)return -1;if(!ready)return XIAO_MESSAGE_OTHER;
    }
    char bytes[256];int64_t read_started=esp_timer_get_time();
    int n=esp_transport_read(ws,bytes,sizeof(bytes),1000);
    int64_t read_ms=(esp_timer_get_time()-read_started)/1000;
    if(r->assistant && read_ms>100)ESP_LOGW(TAG,"ASSISTANT_WS_READ ms=%u bytes=%d",(unsigned)read_ms,n);
    int opcode=esp_transport_ws_get_read_opcode(ws);
    if(opcode==WS_TRANSPORT_OPCODES_CLOSE || n<0)return -1;
    if(!r->remaining) {
        if(opcode==WS_TRANSPORT_OPCODES_TEXT) {
            if(r->fragment)return -1;
            feishu_xiao_message_init(&r->message);r->binary=false;
            if(r->assistant){free(r->assistant->request);r->assistant->request=NULL;r->assistant->capacity=r->assistant->used=0;}
        } else if(opcode==WS_TRANSPORT_OPCODES_BINARY) {if(r->fragment)return -1;r->binary=true;}
        else if(opcode==WS_TRANSPORT_OPCODES_CONT){if(!r->fragment)return -1;}
        else return XIAO_MESSAGE_OTHER;
        r->remaining=esp_transport_ws_get_read_payload_len(ws);
    }
    if(n>r->remaining)return -1;
    if(!r->binary && n>0 && !feishu_xiao_message_feed(&r->message,bytes,(size_t)n))return -1;
    if(r->assistant && n>0) {
        xiao_assistant_t *a=r->assistant;a->received_bytes+=(uint32_t)n;a->last_payload_at=esp_timer_get_time();
        if(r->binary) {
            if(a->can_play && !feishu_xiao_downlink_feed(&a->downlink,bytes,(size_t)n))return -1;
        } else {
            if((size_t)n>FEISHU_MCP_INPUT_MAX-a->used)return -1;
            size_t need=a->used+(size_t)n+1;
            if(need>a->capacity) {
                size_t capacity=a->capacity ? a->capacity:512;
                while(capacity<need && capacity<FEISHU_MCP_INPUT_MAX+1)capacity=capacity>2048 ? FEISHU_MCP_INPUT_MAX+1:capacity*2;
                char *next=realloc(a->request,capacity);if(!next)return -1;
                a->request=next;a->capacity=capacity;
            }
            memcpy(a->request+a->used,bytes,(size_t)n);a->used+=(size_t)n;a->request[a->used]=0;
        }
    }
    r->remaining-=n;
    if(!r->remaining)r->fragment=!esp_transport_ws_get_fin_flag(ws);
    if(!r->remaining && esp_transport_ws_get_fin_flag(ws)) {
        if(r->binary) {
            if(r->assistant && r->assistant->can_play &&
               !feishu_xiao_downlink_end(&r->assistant->downlink,true,xiao_assistant_opus,r->assistant)) {
                ESP_LOGW(TAG,"ASSISTANT_WIRE audio_rejected bytes=%u version=%u",(unsigned)r->assistant->downlink.used,r->assistant->downlink.version);
                return -1;
            }
        } else {
            int kind=feishu_xiao_message_finish(&r->message);
            if(r->assistant && kind>=0) {
                if(r->assistant->business_busy && !strcmp(r->message.type,"mcp")){
                    r->deferred_kind=kind+1;ESP_LOGI(TAG,"ASSISTANT_BUSINESS_RPC deferred=1");return XIAO_MESSAGE_OTHER;
                }
                if(kind==XIAO_MESSAGE_HELLO)memcpy(r->assistant->session,r->message.session,sizeof(r->assistant->session));
                bool valid=xiao_assistant_message(ws,r->assistant,kind,r->message.type);
                free(r->assistant->request);r->assistant->request=NULL;r->assistant->capacity=r->assistant->used=0;
                if(!valid)return -1;
            }
            return kind;
        }
    }
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
static esp_err_t xiao_pcm_upload(esp_transport_handle_t ssl,xiao_capture_t *c,unsigned version,int slot);
static esp_err_t feishu_asr_record_xiaozhi(volatile bool *stop,const volatile bool *cancel,volatile unsigned *elapsed,
    char *recognition,size_t capacity,feishu_data_writer_t *draft,volatile bool *preparing,xiao_assistant_t *assistant)
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
    if(c)atomic_init(&c->extra_packets,NULL);
    if(!cfg || !c || !receiver)goto done;
    if(!xiao_capture_allocate(c,true))goto done;
    atomic_init(&c->ready_for_voice,false);
    receiver->assistant=assistant;
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
    if(assistant && !feishu_xiao_downlink_init(&assistant->downlink,(unsigned)cfg->version)){err=ESP_ERR_NOT_SUPPORTED;goto done;}
    const char hello[]="{\"type\":\"hello\",\"version\":1,\"transport\":\"websocket\",\"features\":{\"mcp\":false},\"audio_params\":{\"format\":\"pcm\",\"sample_rate\":16000,\"channels\":1,\"frame_duration\":20}}";
    stage="hello";
    char welcome[sizeof(hello)+8];snprintf(welcome,sizeof(welcome),"%s",hello);
    if(assistant) {
        char *feature=strstr(welcome,"false");
        if(feature)memmove(feature+4,feature+5,strlen(feature+5)+1),memcpy(feature,"true",4);
    }
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
    ESP_LOGI(TAG,"ASR_CAPTURE codec=pcm6 wire=pcm16 queue_rate=8000 frames=%u capacity_ms=%u audio_ram=%u heap=%u largest=%u",
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
            /* Compact queue duration stays 20ms; expand at the wire boundary
             * to the service's 16k PCM input instead of doubling speech speed. */
            size_t header=cfg->version==2 ? 16:cfg->version==3 ? 4:0,n=sizeof(c->upload_pcm)*2;
            int64_t send_started=esp_timer_get_time();errno=0;
            esp_err_t uploaded=c->error==ESP_OK ? xiao_pcm_upload(ssl,c,(unsigned)cfg->version,slot):c->error;
            int sent=uploaded==ESP_OK ? (int)(header+n):-1;
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
    if(assistant){assistant->can_play=true;memset(cfg,0,sizeof(*cfg));free(cfg);cfg=NULL;}
    stage=assistant ? "assistant_reply":"recognition";deadline=esp_timer_get_time()+(assistant ? 90000000:15000000);
    while(esp_timer_get_time()<deadline) {
        if(cancel && *cancel){err=FEISHU_HTTP_ERR_CANCELLED;goto done;}
        int kind=xiao_receive(ws,receiver,100);if(kind<0 || kind==XIAO_MESSAGE_ALERT){err=ESP_ERR_INVALID_RESPONSE;goto done;}
        if(kind==XIAO_MESSAGE_STT) {
            size_t n=strlen(receiver->message.text);
            if(!n){err=ESP_ERR_NOT_FOUND;goto done;}
            if(!draft && !assistant && n>=capacity){err=FEISHU_ERR_ASR_TOO_LONG;goto done;}
            feishu_utf8_copy(recognition,capacity,receiver->message.text);
            if(assistant) {
#ifdef FEISHU_ASSISTANT_AUTOTEST
                ESP_LOGI(TAG,"ASSISTANT_FIXTURE_STT text=%s",recognition);
#endif
                if(assistant->hooks->event)assistant->hooks->event(assistant->hooks->context,FEISHU_ASSISTANT_HEARD,recognition);
                continue;
            }
            (void)xiao_control(ws,session,NULL);xiao_ws_close(&ws,&ssl);
            if(draft) {
                err=feishu_cache_draft_begin(draft);
                if(err==ESP_OK)err=feishu_cache_text_append(draft,receiver->message.text,n);
                if(err!=ESP_OK){
                    ESP_LOGW(TAG,"ASR_DRAFT storage=%d complete_bytes=%u ram_capacity=%u",err,(unsigned)n,(unsigned)capacity);
                    feishu_cache_text_abort(draft);
                    if(n<capacity)err=ESP_OK; /* Complete short result remains in RAM. */
                    else recognition[0]=0;
                }
            }
            ESP_LOGI(TAG,"XIAO_STT result=%d text_bytes=%u after_stop_ms=%u",err,(unsigned)n,(unsigned)((esp_timer_get_time()-stopped)/1000));
            goto done;
        }
        if(assistant && assistant->done){err=ESP_OK;goto done;}
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

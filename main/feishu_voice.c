#include "feishu_api.h"
#include "feishu_http.h"
#include "feishu_cache.h"
#include "feishu_internal_asr_protocol.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_system.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const char *TAG="feishu_voice";
typedef struct {feishu_data_writer_t *writer;esp_err_t error;} voice_storage_t;
static bool voice_store(void *context,const void *data,size_t size)
{
    voice_storage_t *p=context;
    if(!p->writer)return true;
    if(!p->writer->id)p->error=feishu_cache_draft_begin(p->writer);
    if(p->error==ESP_OK)p->error=feishu_cache_text_append(p->writer,data,size);
    return p->error==ESP_OK;
}
typedef struct {const uint8_t *header;size_t header_size;feishu_asr_upload_t pcm;void *context;} voice_upload_t;
static bool voice_upload(void *context,feishu_http_write_t write,void *writer)
{
    voice_upload_t *p=context;
    return write(writer,p->header,p->header_size) && (!p->pcm || p->pcm(p->context,write,writer));
}
static esp_err_t voice_bytes(void *context,const uint8_t *data,size_t size)
{ return feishu_internal_asr_feed(context,data,size) ? ESP_OK:ESP_ERR_INVALID_RESPONSE; }
typedef struct {const volatile bool *requested;const volatile esp_err_t *capture_error;} voice_cancel_t;
static bool voice_cancelled(void *context)
{
    voice_cancel_t *p=context;
    return (p->requested && *p->requested) || (p->capture_error && *p->capture_error!=ESP_OK);
}

esp_err_t feishu_api_internal_asr_upload(const char *cookie,const char *source,const char *context_id,
    uint32_t sequence,unsigned action,size_t pcm_bytes,feishu_asr_upload_t upload_pcm,void *upload_context,
    char *recognition,size_t recognition_size,uint64_t *last_length,
    feishu_data_writer_t *final_draft,const volatile bool *cancel_requested,const volatile esp_err_t *capture_error)
{
    if(!cookie || !source || !context_id || !recognition || !recognition_size ||
       !last_length || pcm_bytes>FEISHU_INTERNAL_ASR_PCM_LIMIT || (pcm_bytes && !upload_pcm))return ESP_ERR_INVALID_ARG;
    char cid[11];snprintf(cid,sizeof(cid),"%08lx%02lx",(unsigned long)esp_random(),(unsigned long)(esp_random()&255));
    uint8_t header[384];size_t body_size=0;
    size_t header_size=feishu_internal_asr_header(header,sizeof(header),source,cid,context_id,sequence,action,pcm_bytes,&body_size);
    if(!header_size)return ESP_ERR_INVALID_ARG;
    feishu_internal_asr_parser_t *parser=calloc(1,sizeof(*parser));
    char *preview=calloc(1,recognition_size);
    if(!parser || !preview){free(parser);free(preview);return ESP_ERR_NO_MEM;}
    voice_storage_t storage={.writer=action==2 ? final_draft:NULL,.error=ESP_OK};
    feishu_internal_asr_init(parser,source,cid,sequence,preview,recognition_size,voice_store,&storage);
    const feishu_http_header_t headers[]={
        {"Content-Type","application/x-protobuf"},{"Accept","application/x-protobuf"},
        {"Accept-Encoding","identity"},{"Cookie",cookie},
        {"x-command","1305"},{"x-command-version","8.4.7"},{"x-request-id",cid},
        {"x-appid","161471"},{"x-source","web"},{"x-web-version","3.9.32"},
        {"x-lgw-os-type","1"},{"x-lgw-terminal-type","2"},
        {"Origin","https://www.feishu.cn"},{"Referer","https://www.feishu.cn/"},
        {"User-Agent","Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/150.0.0.0 Safari/537.36"}};
    voice_upload_t upload={header,header_size,upload_pcm,upload_context};
    voice_cancel_t cancellation={cancel_requested,capture_error};
    int status=0;size_t bytes=0;int64_t start=esp_timer_get_time();
    esp_err_t err=feishu_http_post_headers_stream("https://internal-api-lark-api.feishu.cn/im/gateway/",
        headers,sizeof(headers)/sizeof(headers[0]),body_size,voice_upload,&upload,true,
        FEISHU_INTERNAL_ASR_LIMIT+2048,voice_bytes,parser,voice_cancelled,&cancellation,&status,&bytes);
    if(err==ESP_OK && (status!=200 || !feishu_internal_asr_finish(parser) || (action==2 && !parser->is_end)))err=ESP_ERR_INVALID_RESPONSE;
    if(status==401 || status==403)err=FEISHU_ERR_VOICE_LOGIN;
    if(status==429 || parser->handle_action==2)err=FEISHU_ERR_ASR_LIMITED;
    if(storage.error!=ESP_OK)err=storage.error;
    /* A NULL final writer explicitly selects a bounded RAM-only reply. Never
     * publish its truncated preview as if the entire recognition had fitted. */
    if(err==ESP_OK && action==2 && !final_draft && parser->text_length>=recognition_size)err=FEISHU_ERR_ASR_TOO_LONG;
    if(err==ESP_OK) {
        if(parser->text_length) {
            *last_length=parser->text_length;
            feishu_utf8_copy(recognition,recognition_size,preview);
        } else if(action==2 && *last_length) {
            /* Empty END must never turn a truncated interim preview into a draft. */
            if(*last_length!=strlen(recognition))err=final_draft ? ESP_ERR_INVALID_SIZE:FEISHU_ERR_ASR_TOO_LONG;
            else if(!voice_store(&storage,recognition,strlen(recognition)))err=storage.error;
        }
    }
    ESP_LOGI(TAG,"internal ASR seq=%lu action=%u HTTP=%d code=%lu end=%u bytes=%u text=%u ms=%lu heap=%u",
        (unsigned long)sequence,action,status,(unsigned long)parser->error_code,(unsigned)parser->is_end,
        (unsigned)bytes,(unsigned)parser->text_length,(unsigned long)((esp_timer_get_time()-start)/1000),
        (unsigned)esp_get_free_heap_size());
    memset(preview,0,recognition_size);free(preview);memset(parser,0,sizeof(*parser));free(parser);
    if(err!=ESP_OK && final_draft)feishu_cache_text_abort(final_draft);
    return err;
}
typedef struct {const void *pcm;size_t bytes;} voice_memory_t;
static bool voice_memory(void *context,feishu_asr_write_t write,void *writer)
{voice_memory_t *p=context;return !p->bytes || write(writer,p->pcm,p->bytes);}
esp_err_t feishu_api_internal_asr_packet(const char *cookie,const char *source,const char *context_id,
    uint32_t sequence,unsigned action,const int16_t *pcm,size_t samples,
    char *recognition,size_t recognition_size,uint64_t *last_length,
    feishu_data_writer_t *draft,const volatile bool *cancel_requested,const volatile esp_err_t *capture_error)
{
    if(samples>FEISHU_INTERNAL_ASR_PCM_LIMIT/2 || (samples && !pcm))return ESP_ERR_INVALID_ARG;
    voice_memory_t memory={pcm,samples*2};
    return feishu_api_internal_asr_upload(cookie,source,context_id,sequence,action,samples*2,
        voice_memory,&memory,recognition,recognition_size,last_length,draft,cancel_requested,capture_error);
}

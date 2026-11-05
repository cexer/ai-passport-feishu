/* Explicitly enabled diagnostic only: real sends are restricted to the
 * owner-authorized chat compiled into the generated, private run config. */
#include "feishu_api.h"
#include "feishu_asr.h"
#include "feishu_cache.h"
#include "esp_system.h"
#include "business_autotest_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static esp_err_t body_matches(const feishu_message_t *m,const char *expected)
{
    uint8_t bytes[256];size_t total=strlen(expected),at=0;
    /* Botmux's inbound stimulus is a post, whose empty title and final row
     * become leading/trailing LF in the device's documented plain-text view.
     * The independent CLI reference omits those presentation separators.
     * Normalize only this known synthetic post; outbound text stays exact. */
    if(!strcmp(m->message_id,BUSINESS_IN_ID)) {
        size_t n=0;esp_err_t err=feishu_cache_text_read(m->message_id,m->text_object_id,
            m->text_mapped,0,bytes,sizeof(bytes),&n);
        if(err!=ESP_OK)return err;
        size_t first=0,last=n;
        while(first<last && bytes[first]=='\n')++first;
        while(last>first && bytes[last-1]=='\n')--last;
        printf("BUSINESS_POST bytes=%u logical=%u leading_lf=%u trailing_lf=%u unavailable=%u\n",
            (unsigned)n,(unsigned)m->text_length,(unsigned)first,(unsigned)(n-last),(unsigned)m->text_unavailable);
        return !m->text_unavailable && n==m->text_length && last-first==total &&
            !memcmp(bytes+first,expected,total) ? ESP_OK:ESP_ERR_INVALID_RESPONSE;
    }
    while(at<total) {
        size_t n=0;esp_err_t err=feishu_cache_text_read(m->message_id,m->text_object_id,
            m->text_mapped,at,bytes,sizeof(bytes),&n);
        if(err!=ESP_OK)return err;
        if(!n || n>total-at || memcmp(bytes,expected+at,n))return ESP_ERR_INVALID_RESPONSE;
        at+=n;
    }
    return ESP_OK;
}
static esp_err_t read_latest(feishu_api_session_t *s,const char *expected)
{
    feishu_message_t *m=calloc(1,sizeof(*m));feishu_page_cursor_t cursor={0};
    if(!m)return ESP_ERR_NO_MEM;
    esp_err_t err=feishu_api_message_page(s,BUSINESS_CHAT,"",m,1,&cursor);
    if(err==ESP_OK && cursor.count!=1)err=ESP_ERR_NOT_FOUND;
    if(err==ESP_OK)err=body_matches(m,expected);
    if(err==ESP_OK)printf("BUSINESS_READBACK message_id=%s bytes=%u\n",m->message_id,(unsigned)strlen(expected));
    free(m);return err;
}
#define STEP(label,expr) do {err=(expr);printf("BUSINESS_STEP name=%s err=%d\n",label,(int)err);if(err!=ESP_OK)goto done;}while(0)
#ifdef FEISHU_SEND_PERF_AUTOTEST
#include "send_perf_autotest.inc"
#endif
void feishu_business_autotest(feishu_api_session_t *s)
{
#ifdef FEISHU_SEND_PERF_AUTOTEST
    send_perf_autotest(s);return;
#endif
    esp_err_t err=ESP_OK;bool success=false;
    feishu_message_t *m=calloc(1,sizeof(*m));
    char text[FEISHU_REPLY_MAX]={0},outgoing[FEISHU_REPLY_MAX+128];
    volatile bool stop=false,cancel=false;volatile unsigned elapsed=0;
    printf("BUSINESS_BEGIN run=%s full_product=1 real_sends=1\n",BUSINESS_RUN);
    if(!m){err=ESP_ERR_NO_MEM;goto done;}
    STEP("receive",feishu_api_get_message(s,BUSINESS_IN_ID,m));
    STEP("receive_cached_exact",body_matches(m,BUSINESS_IN_TEXT));
    STEP("send_utf8",feishu_api_send_text(s,BUSINESS_CHAT,BUSINESS_OUT_TEXT,BUSINESS_OUT_UUID));
    STEP("same_uuid_ack",feishu_api_send_text(s,BUSINESS_CHAT,BUSINESS_OUT_TEXT,BUSINESS_OUT_UUID));
    STEP("utf8_readback",read_latest(s,BUSINESS_OUT_TEXT));
    STEP("reply",feishu_api_reply_text(s,BUSINESS_IN_ID,BUSINESS_REPLY_TEXT,BUSINESS_REPLY_UUID));
    feishu_asr_autotest_prepare(&stop,&cancel);
    err=feishu_asr_record(s,&stop,&cancel,&elapsed,text,sizeof(text),NULL,NULL);
    feishu_asr_autotest_finish();
    printf("BUSINESS_STEP name=voice_capture_asr err=%d exact=%u elapsed_ms=%u\n",(int)err,
        (unsigned)!strcmp(text,BUSINESS_VOICE_EXPECTED),elapsed);
    if(err!=ESP_OK)goto done;
    if(strcmp(text,BUSINESS_VOICE_EXPECTED)){err=ESP_ERR_INVALID_RESPONSE;goto done;}
    snprintf(outgoing,sizeof(outgoing),"%s%s",BUSINESS_VOICE_PREFIX,text);
    STEP("voice_send",feishu_api_send_text(s,BUSINESS_CHAT,outgoing,BUSINESS_VOICE_UUID));
    STEP("voice_readback",read_latest(s,outgoing));
    success=true;
done:
    free(m);memset(text,0,sizeof(text));
    printf("BUSINESS_DONE pass=%u err=%d heap=%u\n",(unsigned)success,(int)err,(unsigned)esp_get_free_heap_size());
}

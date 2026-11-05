/* Opt-in diagnostic: one durable send to the already authorized test chat.
 * Existing drafts are never selected or overwritten. No speech/audio needed. */
#include "feishu_flash_port.h"
#include "esp_timer.h"
#include "esp_random.h"
static feishu_flash_profile_t perf_before;
static int64_t perf_started;
static void perf_start(void)
{
    feishu_cache_pool_lock();perf_before=feishu_flash_profile();feishu_cache_pool_unlock();
    perf_started=esp_timer_get_time();
}
static void perf_end(const char *name,esp_err_t result)
{
    unsigned elapsed=(unsigned)((esp_timer_get_time()-perf_started)/1000);
    feishu_cache_pool_lock();feishu_flash_profile_t now=feishu_flash_profile();feishu_cache_pool_unlock();
    printf("SEND_PERF step=%s result=%d ms=%u reads=%u writes=%u erases=%u read_ms=%u write_ms=%u erase_ms=%u\n",
        name,result,elapsed,(unsigned)(now.reads-perf_before.reads),(unsigned)(now.writes-perf_before.writes),(unsigned)(now.erases-perf_before.erases),
        (unsigned)((now.read_us-perf_before.read_us)/1000),(unsigned)((now.write_us-perf_before.write_us)/1000),(unsigned)((now.erase_us-perf_before.erase_us)/1000));
}
#define PERF_STEP(name,call) do {perf_start();err=(call);perf_end(name,err);if(err!=ESP_OK)goto perf_done;}while(0)
static void send_perf_autotest(feishu_api_session_t *s)
{
    char text[96];
    snprintf(text,sizeof(text),"[性能测试] 飞书终端发送耗时检查 #%08lx",(unsigned long)esp_random());
    feishu_outbox_t p={0},check={0};feishu_data_writer_t body={0};
    int slot=-1;esp_err_t err=ESP_OK;bool passed=false;char uuid[24],name[48];
    printf("SEND_PERF_BEGIN sends_max=1 durable=1\n");
    /* A prior unfinished performance draft blocks automatic re-entry. */
    for(unsigned i=0;i<FEISHU_OUTBOX_SLOTS;++i) {
        esp_err_t rc=feishu_cache_pending_read(i,&check);
        if(rc==ESP_OK && !strncmp(check.body_name,"sendperf_",9)){err=ESP_ERR_INVALID_STATE;goto perf_done;}
        if(rc==ESP_ERR_NOT_FOUND && slot<0)slot=(int)i;
        else if(rc!=ESP_OK && rc!=ESP_ERR_NOT_FOUND){err=rc;goto perf_done;}
    }
    if(slot<0){err=ESP_ERR_NO_MEM;goto perf_done;}
    snprintf(uuid,sizeof(uuid),"perf-%08lx%08lx",(unsigned long)esp_random(),(unsigned long)esp_random());
    snprintf(name,sizeof(name),"sendperf_%s",uuid);
    PERF_STEP("body_begin",feishu_cache_draft_begin(&body));
    PERF_STEP("body_append",feishu_cache_text_append(&body,text,strlen(text)));
    if(!feishu_outbox_create(&p,body.id,body.raw,name,BUSINESS_CHAT,"",uuid)){err=ESP_ERR_INVALID_ARG;goto perf_done;}
    PERF_STEP("draft_publish",feishu_cache_pending_write((unsigned)slot,&p,&body));
    PERF_STEP("draft_read",feishu_cache_pending_read((unsigned)slot,&check));
    if(strcmp(check.uuid,uuid) || !feishu_outbox_attempt(&p,false)){err=ESP_ERR_INVALID_STATE;goto perf_done;}
    PERF_STEP("persist_intent",feishu_cache_pending_write((unsigned)slot,&p,NULL));
    uint64_t sent=0;bool rejected=false;
    PERF_STEP("send_api",feishu_api_send_pending_part(s,&p,&sent,&rejected));
    if(!feishu_outbox_ack(&p,sent)){err=ESP_ERR_INVALID_RESPONSE;goto perf_done;}
    PERF_STEP("persist_ack",feishu_cache_pending_write((unsigned)slot,&p,NULL));
    feishu_message_t *items=calloc(8,sizeof(*items));
    if(!items){err=ESP_ERR_NO_MEM;goto perf_done;}
    feishu_page_cursor_t cursor={0};
    perf_start();err=feishu_api_message_page(s,BUSINESS_CHAT,"",items,8,&cursor);perf_end("readback",err);
    bool found=false;
    if(err==ESP_OK)for(size_t i=0;i<cursor.count;++i)if(body_matches(&items[i],text)==ESP_OK){found=true;break;}
    free(items);if(!found){err=ESP_ERR_INVALID_RESPONSE;goto perf_done;}
    passed=true;
perf_done:
    if(body.id)feishu_cache_text_abort(&body);
    printf("SEND_PERF_DONE pass=%u err=%d slot=%d sends_max=1\n",(unsigned)passed,err,slot);
}
#undef PERF_STEP

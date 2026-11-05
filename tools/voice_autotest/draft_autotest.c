/* Full-product draft diagnostics. No send API, speaker or user-data erase. */
#include "feishu_cache.h"
#include "feishu_outbox.h"
#include "feishu_root.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

typedef struct {uint32_t magic,phase,slot,reserved;feishu_outbox_t pending;} marker_t;
static const feishu_root_t root={.directory="raw"};
static const char expected[]="今天天气不错，测试飞书语音识别。";
static int marker_read(marker_t *m,size_t *size)
{
    if(feishu_cache_pool_lock()!=ESP_OK)return FEISHU_ROOT_IO;
    /* Root reads are exact slices: a cleared zero-length marker is valid,
     * but reading a full struct from it would correctly return INVALID. */
    int rc=feishu_root_read(&root,251,0,NULL,0,size);
    if(rc==FEISHU_ROOT_OK && *size==sizeof(*m))
        rc=feishu_root_read(&root,251,0,m,sizeof(*m),size);
    feishu_cache_pool_unlock();return rc;
}
static bool marker_write(const marker_t *m)
{
    if(feishu_cache_pool_lock()!=ESP_OK)return false;
    feishu_root_part_t part={m,m ? sizeof(*m):0};
    int rc=feishu_root_write(&root,251,m ? &part:NULL,m ? 1:0);
    feishu_cache_pool_unlock();return rc==FEISHU_ROOT_OK;
}
static bool matches(const feishu_outbox_t *p,const marker_t *m)
{
    return feishu_outbox_valid(p) && p->object_id==m->pending.object_id &&
        p->length==m->pending.length && !strcmp(p->uuid,m->pending.uuid) &&
        !strcmp(p->chat,m->pending.chat) && !strcmp(p->message,m->pending.message);
}
static bool body_ok(const feishu_outbox_t *p)
{
    char data[128];size_t n=0;
    return p->length==strlen(expected) &&
        feishu_cache_text_read_stream(p->body_name,p->object_id,false,0,data,sizeof(data),&n)==ESP_OK &&
        n==strlen(expected) && !memcmp(data,expected,n);
}
void feishu_draft_autotest_saved(int slot,const feishu_outbox_t *p,const char *text,bool ui_pass)
{
    marker_t m={.magic=0x32544644,.phase=1,.slot=(uint32_t)slot,.pending=*p};
    feishu_outbox_t saved={0};
    bool pass=ui_pass && slot>=0 && slot<FEISHU_OUTBOX_SLOTS && !strcmp(text,expected) &&
        feishu_cache_pending_read((unsigned)slot,&saved)==ESP_OK && matches(&saved,&m) &&
        saved.state==FEISHU_OUTBOX_READY && body_ok(&saved) && marker_write(&m);
    printf("DRAFT_PHASE phase=1 pass=%u bytes=%u ui_offline=%u sends=0 heap=%u minimum=%u stack_remaining=%u\n",
        (unsigned)pass,(unsigned)p->length,(unsigned)ui_pass,
        (unsigned)esp_get_free_heap_size(),(unsigned)esp_get_minimum_free_heap_size(),
        (unsigned)uxTaskGetStackHighWaterMark(NULL));
}
bool feishu_draft_autotest_resume(void)
{
    marker_t m={0};size_t size=0;int rc=marker_read(&m,&size);
    if(rc==FEISHU_ROOT_MISSING || (rc==FEISHU_ROOT_OK && !size))return false;
    unsigned phase=m.phase==1 ? 2:3;bool pass=false;feishu_outbox_t p={0};
    if(rc!=FEISHU_ROOT_OK || size!=sizeof(m) || m.magic!=0x32544644 || m.reserved ||
       m.slot>=FEISHU_OUTBOX_SLOTS || (m.phase!=1 && m.phase!=2))goto done;
    if(feishu_cache_pending_read(m.slot,&p)!=ESP_OK || !matches(&p,&m) || !body_ok(&p))goto done;
    if(m.phase==1) {
        if(p.state!=FEISHU_OUTBOX_READY || !feishu_outbox_attempt(&p,false) ||
           feishu_cache_pending_write(m.slot,&p,NULL)!=ESP_OK)goto done;
        feishu_outbox_failed(&p,false);
        if(p.state!=FEISHU_OUTBOX_UNCERTAIN || feishu_cache_pending_write(m.slot,&p,NULL)!=ESP_OK)goto done;
        m.phase=2;if(!marker_write(&m))goto done;
    } else {
        if(p.state!=FEISHU_OUTBOX_UNCERTAIN || feishu_outbox_attempt(&p,false))goto done;
        p.state=FEISHU_OUTBOX_EMPTY;
        if(feishu_cache_pending_write(m.slot,&p,NULL)!=ESP_OK || !marker_write(NULL))goto done;
    }
    pass=true;
done:
    printf("DRAFT_PHASE phase=%u pass=%u bytes=%u uuid_retained=%u sends=0 cleaned=%u\n",
        phase,(unsigned)pass,(unsigned)p.length,(unsigned)pass,(unsigned)(pass && phase==3));
    return true;
}

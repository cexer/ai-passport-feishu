/* Diagnostic-only cold-reset verification; never sends or plays anything. */
#include "feishu_cache.h"
#include "feishu_store.h"
#include "feishu_root.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
typedef struct {uint32_t magic,reserved;int64_t object;uint64_t length;} marker_t;
static const feishu_root_t root={.directory="raw"};
static uint8_t block[1024];
static int config_check(bool write)
{
    uint8_t pattern[512],out[512];memset(pattern,0x59,sizeof(pattern));
    feishu_root_part_t parts[32];
    for(unsigned i=0;i<32;++i)parts[i]=(feishu_root_part_t){pattern,sizeof(pattern)};
    if(feishu_cache_pool_lock()!=ESP_OK)return -1;
    int rc=write?feishu_root_write(&root,200,parts,32):0;
    for(size_t at=0;!rc && at<16384;at+=512) {
        size_t n=0;rc=feishu_root_read(&root,200,at,out,sizeof(out),&n);
        if(!rc && (n!=16384 || memcmp(pattern,out,sizeof(out))))rc=-1;
    }
    feishu_cache_pool_unlock();return rc;
}
#define CHECK(x) do{if(!(x)){failed=__LINE__;goto done;}}while(0)
static void task(void *arg)
{
    (void)arg;vTaskDelay(pdMS_TO_TICKS(6000));int failed=0;unsigned phase=1;
    marker_t marker={0};size_t total=0;
    CHECK(feishu_cache_pool_lock()==ESP_OK);
    int rc=feishu_root_read(&root,201,0,&marker,sizeof(marker),&total);
    feishu_cache_pool_unlock();
    CHECK(rc==FEISHU_ROOT_OK || rc==FEISHU_ROOT_MISSING);
    feishu_cache_set_scope("cli_reset_test","ou_reset_test");
    CHECK(feishu_store_wifi_setup_required());
    if(rc==FEISHU_ROOT_MISSING) {
        feishu_data_writer_t body={0};feishu_outbox_t pending;
        CHECK(feishu_cache_draft_begin(&body)==ESP_OK);
        for(size_t at=0;at<6000;at+=sizeof(block)) {
            size_t n=6000-at;if(n>sizeof(block))n=sizeof(block);
            for(size_t i=0;i<n;++i)block[i]=(uint8_t)('a'+(at+i)%19);
            CHECK(feishu_cache_text_append(&body,block,n)==ESP_OK);
        }
        CHECK(feishu_outbox_create(&pending,body.id,body.raw,"reset_body","oc_test","","reset-uuid"));
        CHECK(feishu_cache_pending_write(0,&pending,&body)==ESP_OK);
        CHECK(feishu_outbox_attempt(&pending,false));feishu_outbox_failed(&pending,false);
        CHECK(feishu_cache_pending_write(0,&pending,NULL)==ESP_OK);
        CHECK(config_check(true)==0);
        marker=(marker_t){0x31545346,0,pending.object_id,pending.length};
        feishu_root_part_t part={&marker,sizeof(marker)};
        CHECK(feishu_cache_pool_lock()==ESP_OK);
        rc=feishu_root_write(&root,201,&part,1);feishu_cache_pool_unlock();CHECK(rc==FEISHU_ROOT_OK);
    } else {
        phase=2;CHECK(total==sizeof(marker) && marker.magic==0x31545346 && !marker.reserved && marker.length==6000);
        feishu_outbox_t pending;CHECK(feishu_cache_pending_read(0,&pending)==ESP_OK);
        CHECK(pending.object_id==marker.object && pending.length==marker.length && pending.state==FEISHU_OUTBOX_UNCERTAIN);
        CHECK(!feishu_outbox_attempt(&pending,false));char uuid[41];
        CHECK(feishu_outbox_uuid(&pending,uuid) && !strcmp(uuid,"reset-uuid-0000"));
        for(size_t at=0;at<6000;) {
            size_t n=0;CHECK(feishu_cache_text_read_stream(pending.body_name,pending.object_id,false,at,block,sizeof(block),&n)==ESP_OK);
            CHECK(n && n<=6000-at);
            for(size_t i=0;i<n;++i)CHECK(block[i]==(uint8_t)('a'+(at+i)%19));
            at+=n;
        }
        CHECK(config_check(false)==0);
    }
done:
    feishu_cache_unmount();
    printf("FDB_REBOOT_STAGE phase=%u pass=%u failed_line=%d heap=%u stack_remaining=%u\n",
        phase,(unsigned)(failed==0),failed,(unsigned)esp_get_free_heap_size(),(unsigned)uxTaskGetStackHighWaterMark(NULL));
    vTaskDelete(NULL);
}
void feishu_storage_autotest_start(void) {xTaskCreate(task,"storage_reset",12288,NULL,5,NULL);}

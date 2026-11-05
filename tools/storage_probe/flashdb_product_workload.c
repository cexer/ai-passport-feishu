/* Diagnostic-only, silent tests using the complete product and its real pool.
 * No network messages, credentials or audio are synthesized. */
#include "feishu_cache.h"
#include "feishu_store.h"
#include "feishu_root.h"
#include "feishu_flash_port.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
static uint8_t random_block[1024];
static const feishu_root_t root={.directory="raw"};
static int root_revision(unsigned revision)
{
    uint8_t pattern[512];
    for(size_t i=0;i<sizeof(pattern);++i)pattern[i]=(uint8_t)(i*19);
    pattern[0]=(uint8_t)revision;
    feishu_root_part_t parts[FEISHU_ROOT_MAX/512];
    for(size_t i=0;i<sizeof(parts)/sizeof(parts[0]);++i)parts[i]=(feishu_root_part_t){pattern,sizeof(pattern)};
    if(feishu_cache_pool_lock()!=ESP_OK)return -1;
    int rc=feishu_root_write(&root,200,parts,sizeof(parts)/sizeof(parts[0]));size_t total=0;uint8_t check[512];
    for(size_t at=0;!rc && at<FEISHU_ROOT_MAX;at+=sizeof(check)) {
        rc=feishu_root_read(&root,200,at,check,sizeof(check),&total);
        if(!rc && (total!=FEISHU_ROOT_MAX || memcmp(check,pattern,sizeof(check))))rc=-1;
    }
    feishu_cache_pool_unlock();return rc;
}
#define CHECK(x) do {if(!(x)){line=__LINE__;goto done;}}while(0)
static void task(void *arg)
{
    (void)arg;vTaskDelay(pdMS_TO_TICKS(6000));int line=0;int64_t pinned=0;
    unsigned before=esp_get_free_heap_size();int64_t start=esp_timer_get_time();
    printf("FDB_PRODUCT_BEGIN heap=%u largest=%u\n",before,(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    feishu_credentials_t credentials;
    CHECK(feishu_store_load_app_credentials(&credentials)==ESP_ERR_NVS_NOT_FOUND);
    CHECK(feishu_store_wifi_setup_required());
    feishu_cache_set_scope("cli_storage_test","ou_storage_test");
    feishu_data_writer_t draft={0};feishu_outbox_t p,loaded;
    CHECK(feishu_cache_draft_begin(&draft)==ESP_OK);
    CHECK(feishu_cache_text_append(&draft,"silent persistent draft",23)==ESP_OK);
    CHECK(feishu_outbox_create(&p,draft.id,draft.raw,"draft_test","oc_test","","stable-test-uuid"));
    CHECK(feishu_cache_pending_write(0,&p,&draft)==ESP_OK);
    char first[41],retry[41];CHECK(feishu_outbox_uuid(&p,first));
    CHECK(feishu_outbox_attempt(&p,false));CHECK(feishu_cache_pending_write(0,&p,NULL)==ESP_OK);
    feishu_cache_unmount();
    CHECK(feishu_cache_pending_read(0,&loaded)==ESP_OK && loaded.state==FEISHU_OUTBOX_SENDING);
    CHECK(!feishu_outbox_attempt(&loaded,false));CHECK(feishu_outbox_attempt(&loaded,true));
    CHECK(feishu_outbox_uuid(&loaded,retry) && !strcmp(first,retry));
    feishu_outbox_failed(&loaded,false);CHECK(feishu_cache_pending_write(0,&loaded,NULL)==ESP_OK);
    puts("FDB_PRODUCT_DRAFT reopen=1 uncertain_no_auto_retry=1 uuid_stable=1");
    /* Serialize fault arming with the same recursive lock as every worker. */
    CHECK(feishu_cache_pool_lock()==ESP_OK);
    feishu_flash_test_faults(0,-1,false);
    esp_err_t err=feishu_cache_pending_read(0,&p);
    unsigned triggered=feishu_flash_test_injected(),extra=feishu_flash_test_after_fault();
    feishu_flash_test_faults(-1,-1,false);feishu_cache_pool_unlock();
    CHECK(err!=ESP_OK && triggered==1 && !extra);
    CHECK(feishu_cache_pending_read(0,&p)==ESP_OK && p.state==FEISHU_OUTBOX_UNCERTAIN);
    puts("FDB_PRODUCT_READ_FAULT recovered=1");
    const int cuts[]={0,4,12};
    for(unsigned i=0;i<3;++i) {
        CHECK(feishu_outbox_attempt(&p,true));
        CHECK(feishu_cache_pool_lock()==ESP_OK);
        feishu_flash_test_faults(-1,cuts[i],i!=0);
        err=feishu_cache_pending_write(0,&p,NULL);
        triggered=feishu_flash_test_injected();extra=feishu_flash_test_after_fault();
        feishu_flash_test_faults(-1,-1,false);feishu_cache_pool_unlock();
        CHECK(!extra && (triggered?err!=ESP_OK:err==ESP_OK));
        CHECK(feishu_cache_pending_read(0,&p)==ESP_OK && p.object_id==loaded.object_id);
        CHECK(feishu_outbox_attempt(&p,true));feishu_outbox_failed(&p,false);
        CHECK(feishu_cache_pending_write(0,&p,NULL)==ESP_OK);
        printf("FDB_PRODUCT_WRITE_FAULT cut=%d injected=%u recovered=1\n",cuts[i],triggered);
    }
    uint32_t rng=0x12345678;
    for(size_t i=0;i<sizeof(random_block);++i){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;random_block[i]=(uint8_t)rng;}
    int64_t first_id=0;
    for(unsigned i=0;i<40;++i) {
        feishu_data_writer_t w={0};char key[32];int64_t id=0;snprintf(key,sizeof(key),"om_probe_%u",i);
        CHECK(feishu_cache_text_begin(&w)==ESP_OK);
        for(unsigned j=0;j<6;++j)CHECK(feishu_cache_text_append(&w,random_block,sizeof(random_block))==ESP_OK);
        CHECK(feishu_cache_text_commit(&w,key,&id)==ESP_OK);
        if(!i){uint64_t length;first_id=id;CHECK(feishu_cache_text_pin(key,id,false,&pinned,&length)==ESP_OK && length==6144);}
        if(i%10==0)printf("FDB_PRODUCT_CACHE rows=%u heap=%u\n",i+1,(unsigned)esp_get_free_heap_size());
        vTaskDelay(1);
    }
    for(unsigned i=0;i<24;++i) {
        CHECK(root_revision(i)==0);
        if(i%4==0)printf("FDB_PRODUCT_ROOT revision=%u heap=%u\n",i+1,(unsigned)esp_get_free_heap_size());
        vTaskDelay(1);
    }
    uint8_t bytes[64];size_t got=0;
    CHECK(feishu_cache_text_read("om_probe_0",first_id,false,0,bytes,sizeof(bytes),&got)==ESP_OK &&
        got==sizeof(bytes) && !memcmp(bytes,random_block,sizeof(bytes)));
    CHECK(feishu_cache_pending_read(0,&loaded)==ESP_OK && loaded.state==FEISHU_OUTBOX_UNCERTAIN);
    CHECK(feishu_store_load_app_credentials(&credentials)==ESP_ERR_NVS_NOT_FOUND && feishu_store_wifi_setup_required());
    puts("FDB_PRODUCT_PRESSURE protected_draft=1 current_reader=1 config=1");
done:
    feishu_flash_test_faults(-1,-1,false);
    if(pinned)feishu_cache_unpin(&pinned);
    feishu_cache_unmount();
    printf("FDB_PRODUCT_RESULT pass=%u failed_line=%d elapsed_us=%lld heap_before=%u heap_after=%u minimum_heap=%u largest=%u stack_remaining=%u\n",
        (unsigned)(line==0),line,(long long)(esp_timer_get_time()-start),before,(unsigned)esp_get_free_heap_size(),
        (unsigned)esp_get_minimum_free_heap_size(),(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
        (unsigned)uxTaskGetStackHighWaterMark(NULL));
    vTaskDelete(NULL);
}
void feishu_storage_autotest_start(void)
{xTaskCreate(task,"storage_test",16384,NULL,5,NULL);}

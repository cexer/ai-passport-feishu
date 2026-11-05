/* Silent hardware validation of retained DB generations, not a user firmware. */
#include "feishu_cache.h"
#include "feishu_root.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sqlite3.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define CHECK(e) do {if(!(e)) {printf("RECOVERY_PROBE failure_line=%u\n",(unsigned)__LINE__);return false;}} while(0)
static bool crc_file(const char *path,uint32_t *result)
{
    FILE *f=fopen(path,"rb");CHECK(f);uint32_t crc=UINT32_MAX;uint8_t bytes[512];size_t n;
    while((n=fread(bytes,1,sizeof(bytes),f)))for(size_t i=0;i<n;++i) {
        crc^=bytes[i];for(unsigned b=0;b<8;++b)crc=(crc>>1)^((0u-(crc&1u))&0xedb88320u);
    }
    bool okay=!ferror(f);CHECK(!fclose(f) && okay);*result=crc^UINT32_MAX;return true;
}
static bool damage(uint32_t *crc,uint32_t *journal)
{
    feishu_cache_close();CHECK(crc_file("/feishu/objects.db-journal",journal));
    FILE *f=fopen("/feishu/objects.db","r+b");CHECK(f);
    CHECK(fputc('X',f)!=EOF && !fflush(f) && !fsync(fileno(f)) && !fclose(f));
    return crc_file("/feishu/objects.db",crc);
}
static bool reopen(unsigned generation)
{
    CHECK(feishu_cache_open()==ESP_ERR_INVALID_STATE);
    CHECK(feishu_cache_open()==ESP_OK && feishu_cache_recovery_generation()==generation);
    return true;
}
static bool workload(void)
{
    CHECK(feishu_cache_init()==ESP_OK && feishu_cache_mount()==ESP_OK);
    feishu_cache_set_scope("cli_recovery","ou_fake");
    feishu_root_t root={.directory="/feishu"};const char config[]="essential config";
    feishu_root_part_t part={config,sizeof(config)};CHECK(!feishu_root_write(&root,0x7d,&part,1));
    static uint8_t staged[1024];memset(staged,'S',sizeof(staged));
    feishu_data_writer_t old={0},stale={0};int64_t old_id=0;
    CHECK(feishu_cache_text_begin(&old)==ESP_OK && feishu_cache_text_append(&old,"old body",8)==ESP_OK);
    CHECK(feishu_cache_text_commit(&old,"om_same",&old_id)==ESP_OK);
    feishu_data_writer_t draft={0};feishu_outbox_t pending,loaded;
    CHECK(feishu_cache_draft_begin(&draft)==ESP_OK && feishu_cache_text_append(&draft,"old draft",9)==ESP_OK);
    CHECK(feishu_outbox_create(&pending,draft.id,draft.raw,"old-draft","oc_saved","","old-uuid"));
    CHECK(feishu_cache_pending_write(0,&pending,&draft)==ESP_OK);
    CHECK(feishu_cache_text_begin(&stale)==ESP_OK && feishu_cache_text_append(&stale,staged,sizeof(staged))==ESP_OK);
    int64_t floor=stale.id;uint32_t crc=0,journal=0,actual=0;
    CHECK(damage(&crc,&journal) && reopen(1));
    CHECK(crc_file("/feishu/objects.q00000001.db",&actual) && actual==crc);
    CHECK(crc_file("/feishu/objects.q00000001.db-journal",&actual) && actual==journal);
    CHECK(feishu_cache_pending_read(0,&loaded)==ESP_ERR_NOT_FOUND);
    uint8_t bytes[32];size_t size=0;
    CHECK(feishu_cache_text_read("om_same",old_id,false,0,bytes,sizeof(bytes),&size)!=ESP_OK);
    feishu_data_writer_t fresh={0};int64_t fresh_id=0;
    CHECK(feishu_cache_text_begin(&fresh)==ESP_OK && fresh.id>floor);
    CHECK(feishu_cache_text_append(&fresh,"fresh body",10)==ESP_OK && feishu_cache_text_commit(&fresh,"om_same",&fresh_id)==ESP_OK);
    CHECK(feishu_cache_text_append(&stale,"must not write",14)!=ESP_OK);feishu_cache_text_abort(&stale);
    CHECK(feishu_cache_text_read("om_same",fresh_id,false,0,bytes,sizeof(bytes),&size)==ESP_OK && size==10 && !memcmp(bytes,"fresh body",10));
    CHECK(feishu_cache_draft_begin(&draft)==ESP_OK && feishu_cache_text_append(&draft,"new draft",9)==ESP_OK);
    feishu_outbox_t next;CHECK(feishu_outbox_create(&next,draft.id,draft.raw,"new-draft","oc_saved","","new-uuid"));
    CHECK(feishu_cache_pending_write(0,&next,&draft)==ESP_OK && feishu_cache_pending_write(0,&pending,NULL)!=ESP_OK);
    CHECK(feishu_cache_pending_read(0,&loaded)==ESP_OK && loaded.object_id==next.object_id);
    printf("RECOVERY_PROBE stage=first_isolation_old_bytes_and_stale_ids_safe\n");vTaskDelay(1);
    uint32_t second_crc=0,second_journal=0;CHECK(damage(&second_crc,&second_journal) && reopen(2));
    CHECK(crc_file("/feishu/objects.q00000002.db",&actual) && actual==second_crc);
    CHECK(crc_file("/feishu/objects.q00000002.db-journal",&actual) && actual==second_journal);
    CHECK(crc_file("/feishu/objects.q00000001.db",&actual) && actual==crc);
    CHECK(feishu_cache_write("normal","normal",7,1)==ESP_OK);
    size_t count=0;CHECK(feishu_cache_read("normal",bytes,7,1,&count,NULL)==ESP_OK && count==1 && !memcmp(bytes,"normal",7));
    size_t total=0;char settings[sizeof(config)];
    CHECK(!feishu_root_read(&root,0x7d,0,settings,sizeof(settings),&total) && total==sizeof(config) && !memcmp(settings,config,total));
    feishu_cache_close();CHECK(feishu_cache_read("normal",bytes,7,1,&count,NULL)==ESP_OK);
    printf("RECOVERY_PROBE stage=second_isolation_config_and_normal_reopen_safe\n");return true;
}
static void test_task(void *unused)
{
    (void)unused;printf("RECOVERY_PROBE silent=1 network=0 display=0 audio=0\n");
    bool pass=workload();feishu_cache_unmount();
    printf("RECOVERY_PROBE DONE pass=%u sql_peak=%u stack_low=%u\n",(unsigned)pass,
        (unsigned)sqlite3_memory_highwater(0),(unsigned)uxTaskGetStackHighWaterMark(NULL));
    vTaskDelete(NULL);
}
void app_main(void)
{
    if(xTaskCreate(test_task,"recovery_probe",12288,NULL,4,NULL)!=pdPASS)
        printf("RECOVERY_PROBE DONE pass=0 reason=task_create\n");
}

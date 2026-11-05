/* Silent physical pressure test. Static oracles belong to the probe only. */
#include "feishu_cache.h"
#include "feishu_page.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sqlite3.h"
#include <stdio.h>
#include <string.h>

static feishu_chat_page_t page,first,readback;
static uint32_t random_state=0x41a7c3u;
#define CHECK(e) do {if(!(e)) {printf("PAGE_CACHE_PROBE failure_line=%u\n",(unsigned)__LINE__);return false;}} while(0)
static void random_text(char *out,size_t capacity)
{
    for(size_t i=0;i+1<capacity;++i) {
        random_state^=random_state<<13;random_state^=random_state>>17;random_state^=random_state<<5;
        out[i]=(char)(33+random_state%94);
    }
    out[capacity-1]=0;
}
static bool make_page(unsigned number,char key[64])
{
    memset(&page,0,sizeof(page));char token[64],next[64];
    snprintf(token,sizeof(token),"cursor+%u/=",number);snprintf(next,sizeof(next),"cursor+%u/=",number+1);
    CHECK(feishu_page_response(&page.page,number ? token:"",next,true,FEISHU_MAX_CHATS));
    page.page.number=number;
    for(size_t i=0;i<FEISHU_MAX_CHATS;++i) {
        random_text(page.chats[i].chat_id,sizeof(page.chats[i].chat_id));
        random_text(page.chats[i].name,sizeof(page.chats[i].name));
        random_text(page.chats[i].summary,sizeof(page.chats[i].summary));
    }
    feishu_page_key("chats","",page.page.token,key);return true;
}
static bool workload(void)
{
    CHECK(feishu_cache_init()==ESP_OK && feishu_cache_mount()==ESP_OK);
    feishu_cache_set_scope("cli_pages","ou_fake");
    feishu_data_writer_t draft={0};CHECK(feishu_cache_draft_begin(&draft)==ESP_OK);
    CHECK(feishu_cache_text_append(&draft,"protected draft",15)==ESP_OK);
    feishu_outbox_t pending,loaded;
    CHECK(feishu_outbox_create(&pending,draft.id,draft.raw,"draft_page_guard","oc_saved","","fake-uuid"));
    CHECK(feishu_cache_pending_write(0,&pending,&draft)==ESP_OK);
    char key[64],first_key[64],cold_key[64];size_t count=0;int64_t pin=0;
    CHECK(make_page(0,first_key));first=page;
    CHECK(feishu_cache_write(first_key,&page,sizeof(page),1)==ESP_OK);
    CHECK(feishu_cache_read(first_key,&readback,sizeof(readback),1,&count,&pin)==ESP_OK && count==1);
    for(unsigned i=1;i<=100;++i) {
        CHECK(make_page(i,key));if(i==1)strcpy(cold_key,key);
        CHECK(feishu_cache_write(key,&page,sizeof(page),1)==ESP_OK);
        vTaskDelay(1);
        if(i%10==0)printf("PAGE_CACHE_PROBE committed_pages=%u\n",i);
    }
    CHECK(feishu_cache_read(cold_key,&readback,sizeof(readback),1,&count,NULL)==ESP_ERR_NOT_FOUND);
    CHECK(feishu_cache_read(first_key,&readback,sizeof(readback),1,&count,NULL)==ESP_OK && !memcmp(&first,&readback,sizeof(first)));
    CHECK(feishu_cache_pending_read(0,&loaded)==ESP_OK && loaded.object_id==pending.object_id);
    CHECK(feishu_cache_read(key,&readback,sizeof(readback),1,&count,NULL)==ESP_OK && !memcmp(&page,&readback,sizeof(page)));
    printf("PAGE_CACHE_PROBE stage=pinned_page_and_draft_survived\n");
    feishu_cache_unpin(&pin);feishu_cache_close();
    CHECK(feishu_cache_pending_read(0,&loaded)==ESP_OK && loaded.object_id==pending.object_id);
    CHECK(feishu_cache_read(key,&readback,sizeof(readback),1,&count,NULL)==ESP_OK && !memcmp(&page,&readback,sizeof(page)));
    CHECK(feishu_cache_write("normal-after-pressure",&first,sizeof(first),1)==ESP_OK);
    CHECK(feishu_cache_read("normal-after-pressure",&readback,sizeof(readback),1,&count,NULL)==ESP_OK && !memcmp(&first,&readback,sizeof(first)));
    printf("PAGE_CACHE_PROBE stage=reopened_and_normal_write_recovered\n");
    return true;
}
static void test_task(void *unused)
{
    (void)unused;printf("PAGE_CACHE_PROBE silent=1 network=0 display=0 audio=0\n");
    bool pass=workload();feishu_cache_unmount();
    printf("PAGE_CACHE_PROBE DONE pass=%u sql_peak=%u stack_low=%u\n",(unsigned)pass,
        (unsigned)sqlite3_memory_highwater(0),(unsigned)uxTaskGetStackHighWaterMark(NULL));
    vTaskDelete(NULL);
}
void app_main(void)
{
    if(xTaskCreate(test_task,"page_cache_probe",12288,NULL,4,NULL)!=pdPASS)
        printf("PAGE_CACHE_PROBE DONE pass=0 reason=task_create\n");
}

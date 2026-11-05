/* Test-only reference vectors, no networking, display or audio. The large
 * oracle arrays belong to this probe, not the production text iterator. */
#include "feishu_cache.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#define FEISHU_TEXT_STORE_YIELD() vTaskDelay(1)
#define FEISHU_TEXT_STORE_STAGE(name) printf("TEXT_STORE_PROBE stage=%s\n",name)
#define main text_store_reference
#include "text_store_reference.c"
#undef main

static void test_task(void *unused)
{
    (void)unused;
    printf("TEXT_STORE_PROBE silent=1 network=0 display=0 audio=0\n");
    bool ready=feishu_cache_init()==ESP_OK && feishu_cache_mount()==ESP_OK;
    char *args[]={"text-store-probe","/feishu/text-smoke.db"};
    int rc=ready ? text_store_reference(2,args):1;
    feishu_cache_unmount();
    printf("TEXT_STORE_PROBE DONE pass=%u sql_peak=%u stack_low=%u\n",(unsigned)(rc==0),
        (unsigned)sqlite3_memory_highwater(0),(unsigned)uxTaskGetStackHighWaterMark(NULL));
    vTaskDelete(NULL);
}
void app_main(void)
{
    if(xTaskCreate(test_task,"text_store_probe",12288,NULL,4,NULL)!=pdPASS)
        printf("TEXT_STORE_PROBE DONE pass=0 reason=task_create\n");
}

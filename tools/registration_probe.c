/* Silent physical-device protocol/config validation, not a product firmware. */
#include "feishu_store.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int registration_reference(void);
#define CHECK(e) do {if(!(e)){printf("REGISTER_PROBE failure_line=%u\n",(unsigned)__LINE__);return false;}} while(0)
static bool settings(void)
{
    feishu_credentials_t *before=calloc(1,sizeof(*before)),*after=calloc(1,sizeof(*after));
    CHECK(before && after);
    esp_err_t app_status=feishu_store_load_app_credentials(before);
    CHECK(app_status==ESP_OK || app_status==ESP_ERR_NVS_NOT_FOUND);
    wifi_config_t wifi={0},next_wifi={0};
    CHECK(feishu_store_load_wifi(&wifi,sizeof(wifi))==ESP_OK);
    unsigned original=0;CHECK(feishu_store_get_auth_request(&original)==ESP_OK);
    for(unsigned i=0;i<40;++i) {
        unsigned mode=i%4,read=99;
        CHECK(feishu_store_request_auth(mode)==ESP_OK);
        CHECK(feishu_store_get_auth_request(&read)==ESP_OK && read==mode);
        CHECK(feishu_store_load_app_credentials(after)==app_status);
        CHECK(!memcmp(before,after,sizeof(*before)));
        CHECK(feishu_store_load_wifi(&next_wifi,sizeof(next_wifi))==ESP_OK);
        CHECK(!memcmp(&wifi,&next_wifi,sizeof(wifi)));
        vTaskDelay(1);
    }
    CHECK(feishu_store_request_auth(255)==ESP_ERR_INVALID_ARG);
    CHECK(feishu_store_request_auth(original)==ESP_OK);
    memset(before,0,sizeof(*before));memset(after,0,sizeof(*after));free(before);free(after);
    memset(&wifi,0,sizeof(wifi));memset(&next_wifi,0,sizeof(next_wifi));
    return true;
}
static void run(void *argument)
{
    (void)argument;int64_t start=esp_timer_get_time();
    printf("REGISTER_PROBE START\n");
    bool pass=registration_reference()==0;
    printf("REGISTER_PROBE protocol_pass=%u\n",(unsigned)pass);
    if(pass)pass=settings();
    printf("REGISTER_PROBE DONE pass=%u elapsed_ms=%u stack_remaining=%u minimum_free_heap=%u\n",
        (unsigned)pass,(unsigned)((esp_timer_get_time()-start)/1000),
        (unsigned)uxTaskGetStackHighWaterMark(NULL),(unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
    vTaskDelete(NULL);
}
void app_main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);
    if(xTaskCreate(run,"register_probe",16384,NULL,4,NULL)!=pdPASS)
        printf("REGISTER_PROBE DONE pass=0 allocation_failed=1\n");
}

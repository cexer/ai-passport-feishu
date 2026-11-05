/* Silent RF comparison on the real device. No synthetic credentials or audio. */
#include "feishu_network.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

bool wifi_probe_early_awake;
static void run(void *argument)
{
    (void)argument;unsigned passed=0;
    const unsigned waits[]={12000,12000,30000,30000};
    for(unsigned i=0;i<4;++i) {
        wifi_probe_early_awake=(i%2)!=0;
        int64_t start=esp_timer_get_time();
        esp_err_t err=feishu_network_start(waits[i]);int rssi=-127;
        if(err==ESP_OK) {
            wifi_ap_record_t ap={0};
            if(esp_wifi_sta_get_ap_info(&ap)==ESP_OK)rssi=ap.rssi;
            memset(&ap,0,sizeof(ap));++passed;
        }
        printf("WIFI_PROBE case=%u awake=%u budget_ms=%u result=%s elapsed_ms=%u rssi=%d\n",
            i,(unsigned)wifi_probe_early_awake,waits[i],esp_err_to_name(err),
            (unsigned)((esp_timer_get_time()-start)/1000),rssi);
        feishu_network_stop();vTaskDelay(pdMS_TO_TICKS(1000));
    }
    printf("WIFI_PROBE DONE connected_cases=%u total=4 stack_remaining=%u\n",passed,
           (unsigned)uxTaskGetStackHighWaterMark(NULL));
    vTaskDelete(NULL);
}
void app_main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);
    if(xTaskCreate(run,"wifi_probe",12288,NULL,4,NULL)!=pdPASS)
        printf("WIFI_PROBE DONE allocation_failed=1\n");
}

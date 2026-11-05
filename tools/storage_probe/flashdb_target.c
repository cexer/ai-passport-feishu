/* Emulator-only qualification; mounted on a fresh synthetic data pool. */
#include "esp_heap_caps.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include "flashdb.h"

extern int flashdb_probe_run(const char *path);

static void task(void *unused)
{
    (void)unused;
    vTaskDelay(pdMS_TO_TICKS(4000));
#ifdef FDB_USING_FILE_MODE
    esp_vfs_spiffs_conf_t conf = {.base_path="/feishu", .partition_label="feishu_img",
                                 .max_files=5, .format_if_mount_failed=true};
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        printf("STORAGE_PROBE_MOUNT_FAIL err=%d\n", (int)err);
        vTaskDelete(NULL);
        return;
    }
#endif
    unsigned before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    int64_t start = esp_timer_get_time();
    int rc = flashdb_probe_run(
#ifdef FDB_USING_FILE_MODE
        "/feishu"
#else
        "pool"
#endif
    );
    printf("STORAGE_PROBE_TARGET backend=flashdb rc=%d elapsed_us=%lld heap_before=%u heap_after=%u largest=%u stack_free_bytes=%u\n",
           rc, (long long)(esp_timer_get_time()-start), before,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
           (unsigned)uxTaskGetStackHighWaterMark(NULL));
    printf("STORAGE_PROBE_DONE\n");
    vTaskDelete(NULL);
}

void sqlite_probe_start(void)
{
    if (xTaskCreate(task, "fdb_probe", 16384, NULL, 2, NULL) != pdPASS)
        printf("STORAGE_PROBE_TASK_FAIL\n");
}

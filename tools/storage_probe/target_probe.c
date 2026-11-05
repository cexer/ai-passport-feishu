/* Emulator-only qualification harness. Never included by the product build. */
#include "sqlite3.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

extern sqlite3_vfs *sqlite3_demovfs(void);
extern int sqlite_probe_run(const char *, const char *, int);

int sqlite3_os_init(void) { return sqlite3_vfs_register(sqlite3_demovfs(), 1); }
int sqlite3_os_end(void) { return SQLITE_OK; }

static void probe_task(void *unused)
{
    (void)unused;
    vTaskDelay(pdMS_TO_TICKS(4000));
    esp_vfs_spiffs_conf_t config = {
        .base_path = "/feishu", .partition_label = "feishu_img",
        .max_files = 5, .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_spiffs_register(&config);
    if (err != ESP_OK) {
        printf("STORAGE_PROBE_MOUNT_FAIL err=%d\n", (int)err);
        vTaskDelete(NULL);
        return;
    }
    static const int budgets[] = {32768, 49152, 65536};
    for (unsigned i = 0; i < sizeof(budgets)/sizeof(budgets[0]); ++i) {
        /* Only these synthetic files exist in this emulator fixture. */
        remove("/feishu/sqlprobe.db-journal");
        remove("/feishu/sqlprobe.db");
        unsigned before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
        int64_t start = esp_timer_get_time();
        int rc = sqlite_probe_run("/feishu/sqlprobe.db", "demo", budgets[i]);
        printf("STORAGE_PROBE_TARGET budget=%d rc=%d elapsed_us=%lld heap_before=%u heap_after=%u largest=%u stack_words=%u\n",
               budgets[i], rc, (long long)(esp_timer_get_time()-start), before,
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
               (unsigned)uxTaskGetStackHighWaterMark(NULL));
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    printf("STORAGE_PROBE_DONE\n");
    vTaskDelete(NULL);
}

void sqlite_probe_start(void)
{
    if (xTaskCreate(probe_task, "storage_probe", 16384, NULL, 2, NULL) != pdPASS)
        printf("STORAGE_PROBE_TASK_FAIL\n");
}

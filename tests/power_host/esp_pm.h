#include "esp_err.h"
#include <stdbool.h>
typedef struct power_test_lock *esp_pm_lock_handle_t;
enum {ESP_PM_CPU_FREQ_MAX,ESP_PM_NO_LIGHT_SLEEP};
typedef struct {int max_freq_mhz,min_freq_mhz;bool light_sleep_enable;} esp_pm_config_t;
esp_err_t esp_pm_lock_create(int,int,const char*,esp_pm_lock_handle_t*);
esp_err_t esp_pm_lock_acquire(esp_pm_lock_handle_t);esp_err_t esp_pm_lock_release(esp_pm_lock_handle_t);esp_err_t esp_pm_configure(const esp_pm_config_t*);

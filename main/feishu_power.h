#pragma once
#include "feishu_power_policy.h"
#include "esp_err.h"
void feishu_power_init(void);
void feishu_power_activity(void);
/* Called from the LVGL dispatch timer; never changes network state. */
void feishu_power_ui_tick(void);
bool feishu_power_key(void); /* true: consume wake and trailing double/long event */
bool feishu_power_screen_off(void);
void feishu_power_set_settings(const feishu_power_settings_t *s);
feishu_power_settings_t feishu_power_get_settings(void);
bool feishu_power_background_allowed(void);
bool feishu_power_wifi_requested(void);
bool feishu_power_network_pending(void);
void feishu_power_network_poll(void); /* existing network/business worker only */
void feishu_power_busy_begin(void);
void feishu_power_busy_end(void);
typedef struct {uint32_t idle_ms;bool screen_off,cpu_saving,wifi_saving,background,pm_ready;} feishu_power_status_t;
feishu_power_status_t feishu_power_status(void);

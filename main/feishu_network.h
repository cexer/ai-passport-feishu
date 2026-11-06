#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#define FEISHU_WIFI_SCAN_MAX 8
typedef struct {char ssid[33];int rssi;bool secured;} feishu_wifi_ap_t;
esp_err_t feishu_network_scan(feishu_wifi_ap_t *items,size_t capacity,size_t *count);

esp_err_t feishu_network_start(unsigned timeout_ms);
typedef bool (*feishu_network_cancel_t)(void *context);
esp_err_t feishu_network_switch(size_t index,const char *ssid,const char *password,
    feishu_network_cancel_t cancel,void *context,bool *restored);
esp_err_t feishu_network_start_cancelable(unsigned timeout_ms,
    feishu_network_cancel_t cancel,void *context);
esp_err_t feishu_network_forget_wifi(void);
void feishu_network_stop(void);
bool feishu_network_connected(void);
bool feishu_network_user_offline(void);
esp_err_t feishu_network_disconnect(void);

#pragma once

#include "esp_err.h"

#include <stdbool.h>

esp_err_t feishu_network_start(unsigned timeout_ms);
typedef bool (*feishu_network_cancel_t)(void *context);
esp_err_t feishu_network_start_cancelable(unsigned timeout_ms,
    feishu_network_cancel_t cancel,void *context);
esp_err_t feishu_network_forget_wifi(void);
void feishu_network_stop(void);
bool feishu_network_connected(void);

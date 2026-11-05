#pragma once
#include "feishu_registration_protocol.h"
#include "esp_err.h"
typedef bool (*feishu_registration_cancel_t)(void *context);
esp_err_t feishu_registration_begin_cancelable(feishu_register_result_t *request,
    const char *preferred_app,feishu_registration_cancel_t cancel,void *context);
esp_err_t feishu_registration_poll_cancelable(const feishu_register_result_t *request,
    feishu_register_result_t *result,feishu_registration_cancel_t cancel,void *context);
esp_err_t feishu_registration_begin(feishu_register_result_t *request,const char *preferred_app);
esp_err_t feishu_registration_poll(const feishu_register_result_t *request,
                                   feishu_register_result_t *result);

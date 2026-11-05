#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FEISHU_REGISTER_URL_MAX 512
#define FEISHU_REGISTER_CODE_MAX 1024
typedef enum {
    FEISHU_REGISTER_PENDING, FEISHU_REGISTER_COMPLETE, FEISHU_REGISTER_SLOW,
    FEISHU_REGISTER_EXPIRED, FEISHU_REGISTER_DENIED, FEISHU_REGISTER_UNSUPPORTED,
    FEISHU_REGISTER_FAILED
} feishu_register_status_t;
typedef struct {
    char device_code[FEISHU_REGISTER_CODE_MAX];
    char verification_url[FEISHU_REGISTER_URL_MAX];
    char app_id[64], app_secret[128];
    uint32_t expires_in, interval;
    feishu_register_status_t status;
} feishu_register_result_t;
bool feishu_register_decode(const void *json, size_t length, bool beginning,
                            feishu_register_result_t *result);
bool feishu_register_url(char *output, size_t capacity, const char *base,
                        const char *preferred_app);
bool feishu_register_poll_form(char *output, size_t capacity, const char *code);
/* Exact encoded form allocation, including the terminating NUL; 0 on invalid input. */
size_t feishu_register_poll_form_size(const char *code);

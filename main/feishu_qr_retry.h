#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {uint32_t attempted;bool failed;} feishu_qr_retry_t;
void feishu_qr_retry_reset(feishu_qr_retry_t *state);
bool feishu_qr_retry_due(const feishu_qr_retry_t *state,uint32_t now);
void feishu_qr_retry_result(feishu_qr_retry_t *state,uint32_t now,bool success);

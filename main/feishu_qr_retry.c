#include "feishu_qr_retry.h"
void feishu_qr_retry_reset(feishu_qr_retry_t *s) {*s=(feishu_qr_retry_t){0};}
bool feishu_qr_retry_due(const feishu_qr_retry_t *s,uint32_t now)
{return !s->failed || (uint32_t)(now-s->attempted)>=1000u;}
void feishu_qr_retry_result(feishu_qr_retry_t *s,uint32_t now,bool success)
{s->failed=!success;s->attempted=now;}

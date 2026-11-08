#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct {uint32_t tag,inverse;} feishu_boot_cookie_t;
#define FEISHU_AUTH_BOOT_TAG UINT32_C(0x46414231)
static inline void feishu_boot_cookie_arm(feishu_boot_cookie_t *c)
{c->tag=FEISHU_AUTH_BOOT_TAG;c->inverse=~FEISHU_AUTH_BOOT_TAG;}
/* Consume once even on an unexpected reset; no account state lives here. */
static inline bool feishu_boot_cookie_take(feishu_boot_cookie_t *c,bool software_reset)
{
 bool valid=software_reset && c->tag==FEISHU_AUTH_BOOT_TAG && c->inverse==~FEISHU_AUTH_BOOT_TAG;
 c->tag=c->inverse=0;return valid;
}

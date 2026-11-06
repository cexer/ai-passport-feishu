#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Foreground-only renewal; callers serialize requests and schedule a cooldown
 * after every completed attempt. Signed differences handle millisecond wrap. */
static inline bool feishu_pairing_auto_due(uint32_t now,bool active,bool busy,
    bool paired,bool has_code,uint32_t expiry,uint32_t retry_at)
{
    if(!active || busy || paired)return false;
    if(retry_at && (int32_t)(now-retry_at)<0)return false;
    return !has_code || (expiry && (int32_t)(now-expiry)>=0);
}

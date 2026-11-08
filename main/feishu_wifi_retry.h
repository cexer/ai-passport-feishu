#pragma once
#include <stddef.h>
#include <stdint.h>
/* Continue a bounded batch next time instead of starving later saved entries. */
static inline size_t feishu_wifi_retry_first(size_t active,size_t cursor,size_t count)
{return !count ? SIZE_MAX:cursor<count ? cursor:active<count ? active:0;}
static inline unsigned feishu_wifi_retry_budget(unsigned remaining)
{return remaining>8000 ? 8000:remaining;}

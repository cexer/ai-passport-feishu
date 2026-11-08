#pragma once
#include <stdbool.h>
/* A UI-only warm-restart hint. Durable auth state remains authoritative. */
void feishu_boot_authorization_entry(void);
bool feishu_boot_take_authorization_entry(void);

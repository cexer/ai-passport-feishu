#include "feishu_boot.h"
#include "feishu_boot_policy.h"
#include "esp_attr.h"
#include "esp_system.h"
static RTC_NOINIT_ATTR feishu_boot_cookie_t cookie;
void feishu_boot_authorization_entry(void) {feishu_boot_cookie_arm(&cookie);}
bool feishu_boot_take_authorization_entry(void)
{return feishu_boot_cookie_take(&cookie,esp_reset_reason()==ESP_RST_SW);}

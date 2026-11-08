#include "feishu_boot_policy.h"
#include <assert.h>
#include <stdio.h>
int main(void){
 feishu_boot_cookie_t c={0};assert(!feishu_boot_cookie_take(&c,true));
 feishu_boot_cookie_arm(&c);assert(feishu_boot_cookie_take(&c,true));assert(!feishu_boot_cookie_take(&c,true));
 feishu_boot_cookie_arm(&c);assert(!feishu_boot_cookie_take(&c,false));assert(!feishu_boot_cookie_take(&c,true));
 feishu_boot_cookie_arm(&c);c.inverse^=1;assert(!feishu_boot_cookie_take(&c,true));
 feishu_boot_cookie_arm(&c);c.tag^=1;assert(!feishu_boot_cookie_take(&c,true));
 puts("authorization warm-entry: software-reset only, corruption reject, cold reset and one-shot PASS");
}

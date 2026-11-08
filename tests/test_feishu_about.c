#include "feishu_model.h"
#include <assert.h>
#include <stdio.h>
int main(void){
 feishu_nav_t n={.page=FEISHU_PAGE_SETTINGS,.settings_index=FEISHU_SETTING_ABOUT};
 assert(feishu_nav_handle(&n,FEISHU_NAV_OK)==FEISHU_NAV_NONE && n.page==FEISHU_PAGE_ABOUT);
 feishu_nav_handle(&n,FEISHU_NAV_UP);feishu_nav_handle(&n,FEISHU_NAV_DOWN);feishu_nav_handle(&n,FEISHU_NAV_OK);
 assert(n.page==FEISHU_PAGE_ABOUT);
 feishu_nav_handle(&n,FEISHU_NAV_BACK);assert(n.page==FEISHU_PAGE_SETTINGS && n.settings_index==FEISHU_SETTING_ABOUT);
 feishu_nav_handle(&n,FEISHU_NAV_DOWN);assert(n.settings_index==FEISHU_SETTING_RETURN);
 feishu_nav_handle(&n,FEISHU_NAV_OK);assert(n.page==FEISHU_PAGE_CHATS);
 puts("About: entry/stable read-only/back focus/return neighbor PASS");
}

#include "feishu_model.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void){
 feishu_message_t full={0},preview={0};full.type=FEISHU_MESSAGE_TEXT;
 feishu_utf8_copy(full.message_id,sizeof(full.message_id),"om_cache");
 full.create_time_ms=10;full.update_time_ms=20;full.text_object_id=55;full.text_length=5000;
 full.text_mapped=true;full.text_truncated=true;feishu_utf8_copy(full.text,sizeof(full.text),"resolved preview");
 preview=full;preview.text_object_id=0;preview.text_mapped=false;preview.text_unavailable=true;
 assert(feishu_message_restore_body(&preview,&full) && preview.text_object_id==55 && preview.text_mapped);
 assert(!strcmp(preview.text,"resolved preview") && preview.text_length==5000);
 preview.update_time_ms=21;preview.text_object_id=0;
 assert(!feishu_message_restore_body(&preview,&full) && !preview.text_object_id);
 preview.update_time_ms=0;assert(!feishu_message_restore_body(&preview,&full));
 preview.update_time_ms=20;preview.deleted=true;assert(!feishu_message_restore_body(&preview,&full));
 preview.deleted=false;feishu_utf8_copy(preview.message_id,sizeof(preview.message_id),"other");
 assert(!feishu_message_restore_body(&preview,&full));
 preview=full;full.text_unavailable=true;assert(!feishu_message_restore_body(&preview,&full));
 assert(feishu_chat_next_page(0,3,true)==0);
 assert(feishu_chat_next_page(2,3,true)==1);
 assert(feishu_chat_next_page(2,3,false)==-1);
 assert(feishu_chat_next_page(0,0,true)==0);
 assert(feishu_chat_next_page(99,3,true)==0);
 /* Exact regression: the old STATUS page swallows DOWN and double DOWN. */
 feishu_nav_t nav={.page=FEISHU_PAGE_STATUS,.message_count=3,.message_index=1};
 feishu_nav_handle(&nav,FEISHU_NAV_DOWN);assert(nav.page==FEISHU_PAGE_STATUS && nav.message_index==1);
 feishu_nav_message_load_failed(&nav,true);
 assert(nav.page==FEISHU_PAGE_MESSAGES && nav.message_count==3 && nav.message_index==1);
 feishu_nav_handle(&nav,FEISHU_NAV_DOWN);assert(nav.message_index==2);
 feishu_nav_handle(&nav,FEISHU_NAV_COMPOSE);assert(nav.composer_selected);
 feishu_nav_handle(&nav,FEISHU_NAV_BROWSE);assert(!nav.composer_selected);
 feishu_nav_message_load_failed(&nav,false);
 assert(nav.page==FEISHU_PAGE_MESSAGES && !nav.message_count && !nav.message_index && nav.composer_selected);
 feishu_nav_handle(&nav,FEISHU_NAV_BACK);assert(nav.page==FEISHU_PAGE_CHATS);
 feishu_nav_handle(&nav,FEISHU_NAV_BACK);assert(nav.page==FEISHU_PAGE_SETTINGS);
 feishu_nav_message_load_failed(NULL,false);
 puts("lazy body cache: revision/edit/missing/deleted/ID; chat edge/last/empty PASS");
}
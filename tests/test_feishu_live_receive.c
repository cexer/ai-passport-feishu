#include "feishu_model.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void){
 feishu_message_t rows[3]={0},m={0};size_t n=0,sel=0;
 feishu_utf8_copy(m.message_id,sizeof(m.message_id),"a");m.create_time_ms=10;
 assert(feishu_message_receive(rows,&n,3,&sel,true,&m) && n==1 && sel==0);
 assert(!feishu_message_receive(rows,&n,3,&sel,true,&m) && n==1);
 feishu_utf8_copy(m.message_id,sizeof(m.message_id),"b");m.create_time_ms=20;
 assert(feishu_message_receive(rows,&n,3,&sel,true,&m) && sel==1);
 sel=0;feishu_utf8_copy(m.message_id,sizeof(m.message_id),"c");m.create_time_ms=30;
 assert(feishu_message_receive(rows,&n,3,&sel,false,&m) && sel==0);
 sel=1;feishu_utf8_copy(m.message_id,sizeof(m.message_id),"d");m.create_time_ms=40;
 assert(feishu_message_receive(rows,&n,3,&sel,false,&m) && n==3 && sel==0 && !strcmp(rows[sel].message_id,"b"));
 feishu_utf8_copy(m.message_id,sizeof(m.message_id),"older");m.create_time_ms=1;
 assert(!feishu_message_receive(rows,&n,3,&sel,true,&m) && n==3);
 feishu_utf8_copy(m.message_id,sizeof(m.message_id),"deleted");m.create_time_ms=50;m.deleted=true;
 assert(!feishu_message_receive(rows,&n,3,&sel,true,&m));
 m.deleted=false;feishu_utf8_copy(m.message_id,sizeof(m.message_id),"e");
 assert(feishu_message_receive(rows,&n,3,&sel,true,&m) && sel==2 && !strcmp(rows[sel].message_id,"e"));
 assert(!feishu_message_receive(rows,&n,0,&sel,true,&m));
 puts("live preview: dedup, order, deletion, bounded rows, selection and follow-latest PASS");
}

#include "feishu_network_error.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void){
 assert(!feishu_network_error_hint(true,false,201));
 assert(!feishu_network_error_hint(true,true,202));
 assert(strstr(feishu_network_error_hint(false,false,201),"找不到"));
 assert(strstr(feishu_network_error_hint(false,false,202),"验证失败"));
 assert(strstr(feishu_network_error_hint(false,false,15),"握手超时"));
 assert(strstr(feishu_network_error_hint(false,false,204),"握手超时"));
 assert(strstr(feishu_network_error_hint(false,false,210),"不兼容"));
 assert(strstr(feishu_network_error_hint(false,false,211),"不兼容"));
 assert(strstr(feishu_network_error_hint(false,false,212),"信号"));
 assert(strstr(feishu_network_error_hint(false,true,201),"主动断开"));
 assert(strstr(feishu_network_error_hint(false,false,0),"重连"));
 assert(strstr(feishu_network_error_hint(false,false,999),"重连"));
 unsigned reason=feishu_network_record_reason(0,201,false);assert(reason==201);
 reason=feishu_network_record_reason(reason,36,true);assert(reason==201);
 assert(strstr(feishu_network_error_hint(false,false,reason),"找不到"));
 assert(feishu_network_record_reason(reason,202,false)==202);
 assert(!feishu_network_error_hint(true,false,reason));
 puts("network error: known/unknown/user-offline/recovered PASS");
}

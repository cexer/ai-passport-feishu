#include "feishu_wifi_retry.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
int main(void){
 assert(feishu_wifi_retry_first(0,SIZE_MAX,0)==SIZE_MAX);
 assert(feishu_wifi_retry_first(1,SIZE_MAX,2)==1);
 assert(feishu_wifi_retry_first(1,0,2)==0);
 assert(feishu_wifi_retry_first(9,99,2)==0);
 for(size_t count=1;count<=10;++count)for(size_t active=0;active<count;++active){
  bool seen[10]={0};size_t i=feishu_wifi_retry_first(active,SIZE_MAX,count);
  for(size_t n=0;n<count;++n){assert(i<count && !seen[i]);seen[i]=true;i=(i+1)%count;}
  assert(i==active);
 }
 assert(feishu_wifi_retry_budget(20000)==8000);
 assert(feishu_wifi_retry_budget(3000)==3000);
 assert(feishu_wifi_retry_budget(0)==0);
 puts("Wi-Fi retry: active first/continuation/no starvation/empty/time budget PASS");
}

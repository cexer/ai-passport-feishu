#include "feishu_power_policy.h"
#include <assert.h>
#include <stdio.h>
int main(void){
 feishu_power_settings_t s=feishu_power_defaults();assert(feishu_power_valid(&s) && s.screen_minutes==2 && s.cpu==2 && s.refresh==0 && s.wifi==2);
 feishu_power_plan_t p=feishu_power_plan(s,119999,false,false);assert(!p.screen_off&&!p.cpu_save&&!p.wifi_save&&!p.background);
 p=feishu_power_plan(s,120000,false,false);assert(p.screen_off&&p.cpu_save&&p.wifi_save&&!p.background);
 p=feishu_power_plan(s,120000,true,false);assert(p.screen_off&&!p.cpu_save&&!p.wifi_save);
 p=feishu_power_plan(s,120000,false,true);assert(p.screen_off&&!p.cpu_save&&p.wifi_save);
 for(unsigned cpu=0;cpu<3;++cpu)for(unsigned wifi=0;wifi<3;++wifi)for(unsigned r=0;r<3;++r){
  s=(feishu_power_settings_t){10,cpu,r,wifi};assert(feishu_power_valid(&s));
  p=feishu_power_plan(s,0,false,false);assert(p.cpu_save==(cpu==1));assert(p.wifi_save==(wifi==1));assert(p.background==(r!=0));
  p=feishu_power_plan(s,120000,false,false);assert(!p.screen_off&&p.cpu_save==(cpu!=0)&&p.wifi_save==(wifi!=0)&&p.background==(r==1));
  p=feishu_power_plan(s,600000,false,false);assert(p.screen_off);
 }
 uint8_t b[]={1,10,1,2,0};assert(feishu_power_decode(b,5,&s)&&s.screen_minutes==10&&s.refresh==2);
 for(size_t n=0;n<5;++n){assert(!feishu_power_decode(b,n,&s));assert(s.screen_minutes==2&&s.cpu==2);}
 b[0]=2;assert(!feishu_power_decode(b,5,&s));b[0]=1;b[1]=1;assert(!feishu_power_decode(b,5,&s));
 b[1]=11;assert(!feishu_power_decode(b,5,&s));b[1]=2;b[2]=3;assert(!feishu_power_decode(b,5,&s));
 assert(!feishu_power_decode(NULL,0,&s));assert(!feishu_power_decode(b,5,NULL));
 assert(!feishu_power_prewarm_expired(60999,1000));assert(feishu_power_prewarm_expired(61000,1000));
 assert(feishu_power_prewarm_expired(59900,UINT32_MAX-100));
 puts("power policy: defaults/all modes/idle boundaries/busy audio/record corruption/prewarm wrap PASS");
}

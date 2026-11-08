#include "feishu_assistant_policy.h"
#include <assert.h>
#include <stdio.h>
int main(void){
 uint32_t activity=100;
 assert(!feishu_assistant_idle_expired(599999,&activity,10,true,false));
 assert(!feishu_assistant_idle_expired(600100,&activity,10,true,true) && activity==600100);
 assert(!feishu_assistant_idle_expired(1200099,&activity,10,true,false));
 assert(feishu_assistant_idle_expired(1200100,&activity,10,true,false));
 activity=UINT32_MAX-100;
 assert(!feishu_assistant_idle_expired(200,&activity,1,true,true) && activity==200);
 assert(!feishu_assistant_idle_expired(999999,&activity,0,true,false));

 /* Start -> queued -> cancelled before worker: the queued item must not
  * re-enable listening. Active teardown is serialized until the owner exits. */
 assert(feishu_assistant_toggle_action(false,false,false)==FEISHU_ASSISTANT_ACTIVATE);
 assert(feishu_assistant_toggle_action(false,true,false)==FEISHU_ASSISTANT_DEACTIVATE);
 assert(feishu_assistant_toggle_action(true,false,false)==FEISHU_ASSISTANT_DEACTIVATE);
 assert(feishu_assistant_toggle_action(true,true,false)==FEISHU_ASSISTANT_DEACTIVATE);
 assert(feishu_assistant_toggle_action(true,false,true)==FEISHU_ASSISTANT_WAIT_FOR_STOP);
 assert(feishu_assistant_toggle_action(false,false,true)==FEISHU_ASSISTANT_ACTIVATE);
 assert(!feishu_assistant_expired(599999,0,10,true));
 assert(feishu_assistant_expired(600000,0,10,true));
 assert(!feishu_assistant_expired(600000,0,0,true));
 assert(!feishu_assistant_expired(600000,0,10,false));
 uint32_t activation=UINT32_MAX-1000u;
 assert(!feishu_assistant_expired(activation+599999u,activation,10,true));
 assert(feishu_assistant_expired(activation+600000u,activation,10,true));
 assert(feishu_assistant_breath_opacity(0)==48 && feishu_assistant_breath_opacity(1000)==255);
 assert(feishu_assistant_breath_dots(0)==0 && feishu_assistant_breath_dots(1000)==3);
 assert(feishu_assistant_breath_opacity(500)==feishu_assistant_breath_opacity(1500));
 for(uint32_t t=0;t<10000;t+=17){assert(feishu_assistant_breath_opacity(t)>=48 && feishu_assistant_breath_opacity(t)<=255);assert(feishu_assistant_breath_dots(t)<=3);}
 assert(!feishu_assistant_reply_interrupt(false,false));
 assert(!feishu_assistant_reply_interrupt(false,true));
 assert(!feishu_assistant_reply_interrupt(true,false));
 assert(feishu_assistant_reply_interrupt(true,true));
 uint32_t received=0,progress=0;
 for(uint32_t t=1000;t<=600000;t+=1000)assert(!feishu_assistant_reply_stalled(t,t/1000,&received,&progress));
 assert(!feishu_assistant_reply_stalled(689999,600,&received,&progress));
 assert(feishu_assistant_reply_stalled(690000,600,&received,&progress));
 for(int phase=FEISHU_ASSISTANT_OFF;phase<=FEISHU_ASSISTANT_STOPPING;++phase)
  assert((strcmp(feishu_assistant_phase_notice((feishu_assistant_phase_t)phase),"小智聆听中")==0)==(phase==FEISHU_ASSISTANT_READY));
 assert(feishu_assistant_memory_recovery(true,false,false,false));
 assert(!feishu_assistant_memory_recovery(true,false,false,true));
 assert(!feishu_assistant_memory_recovery(true,true,false,false));
 assert(!feishu_assistant_memory_recovery(true,false,true,false));
 assert(!feishu_assistant_memory_recovery(false,false,false,false));
 unsigned retries=0;
 assert(feishu_assistant_transport_retry(&retries,false) && retries==1);
 assert(feishu_assistant_transport_retry(&retries,false) && retries==2);
 assert(!feishu_assistant_transport_retry(&retries,false));
 assert(feishu_assistant_transport_retry(&retries,true) && retries==1);
 int16_t quiet[320]={0},voice[320];for(unsigned i=0;i<320;++i)voice[i]=i%2 ? 1200:-1200;
 for(unsigned sec=2;sec<=5;++sec){
  feishu_assistant_vad_t v={0};
  for(unsigned i=0;i<100000;++i)assert(!feishu_assistant_vad_frame(&v,quiet,320,sec));
  assert(!v.heard);
  for(unsigned i=0;i<3;++i)assert(!feishu_assistant_vad_frame(&v,voice,320,sec));
  assert(v.heard);
  for(unsigned i=0;i<sec*50-1;++i)assert(!feishu_assistant_vad_frame(&v,quiet,320,sec));
  assert(feishu_assistant_vad_frame(&v,quiet,320,sec));
 }
 feishu_assistant_vad_t manual={0};
 for(unsigned i=0;i<3;++i)assert(!feishu_assistant_vad_frame(&manual,voice,320,0));
 for(unsigned i=0;i<10000;++i)assert(!feishu_assistant_vad_frame(&manual,quiet,320,0));
 assert(feishu_assistant_goodbye("小智，再见！"));assert(feishu_assistant_goodbye("BYE!"));assert(feishu_assistant_goodbye("再见。"));assert(feishu_assistant_goodbye("小智再见"));
 assert(!feishu_assistant_goodbye("不要说再见"));assert(!feishu_assistant_goodbye("发送消息再见"));
 int16_t src[480],dst[320];for(unsigned i=0;i<480;++i)src[i]=(int16_t)(i*20);
 assert(feishu_assistant_resample(src,480,dst,320)==320);
 assert(dst[0]==0 && dst[1]==30 && dst[319]==9570);
 assert(!feishu_assistant_resample(src,479,dst,320));
 assert(!feishu_assistant_resample(src,480,dst,319));
 puts("assistant toggle/silence/manual/farewell/resampling policy: PASS");
}

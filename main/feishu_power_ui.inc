static bool s_power_editing;
static void power_render(char *hint,size_t size)
{
 set_label_text(s_title,"省电设置");feishu_power_settings_t p=feishu_power_get_settings();
 const char *mode[]={"否","是","自动"};const char *refresh[]={"仅前台（省电）","自动后台","自动"};
 char a[64],b[64],c[64],d[64];
 snprintf(a,sizeof(a),"闲置熄屏    %u分钟",p.screen_minutes);
 snprintf(b,sizeof(b),"CPU省电    %s",mode[p.cpu]);
 snprintf(c,sizeof(c),"刷新    %s",refresh[p.refresh]);
 snprintf(d,sizeof(d),"Wi-Fi省电    %s",mode[p.wifi]);
 const char *rows[]={a,b,c,d};render_choice_rows(rows,4,s_state.nav.settings_index,false);
 snprintf(hint,size,s_power_editing ? "上下调整  确定保存  长按返回":"上下选择  确定修改  长按返回");
}
static bool power_navigation(feishu_nav_event_t event)
{
 bool save=false;xSemaphoreTake(s_state_lock,portMAX_DELAY);
 if(s_state.nav.page!=FEISHU_PAGE_POWER_SETTINGS){xSemaphoreGive(s_state_lock);return false;}
 feishu_power_settings_t p=feishu_power_get_settings();unsigned index=s_state.nav.settings_index%4;
 if(event==FEISHU_NAV_BACK){
  if(s_power_editing){s_power_editing=false;save=true;}
  else {s_state.nav.page=FEISHU_PAGE_SETTINGS;s_state.nav.settings_index=FEISHU_SETTING_POWER;s_state.status[0]=0;}
 } else if(event==FEISHU_NAV_OK){save=s_power_editing;s_power_editing=!s_power_editing;}
 else if(event==FEISHU_NAV_UP || event==FEISHU_NAV_DOWN){
  if(!s_power_editing)s_state.nav.settings_index=feishu_focus_step(index,4,event==FEISHU_NAV_UP ? -1:1,true);
  else {
   uint8_t *field=index==0 ? &p.screen_minutes:index==1 ? &p.cpu:index==2 ? &p.refresh:&p.wifi;
   if(index==0)*field=(uint8_t)(2+(*field-2+(event==FEISHU_NAV_UP ? 1:8))%9);
   else *field=(uint8_t)((*field+(event==FEISHU_NAV_UP ? 1:2))%3);
   feishu_power_set_settings(&p);
  }
 }
 if(s_state.nav.page==FEISHU_PAGE_POWER_SETTINGS && !save){
  static const char *help[]={"任意按键唤醒，首键不执行","录音、传输时保持性能","仅前台：进入列表时刷新","自动：闲置2分钟后省电"};
  feishu_utf8_copy(s_state.status,sizeof(s_state.status),help[s_state.nav.settings_index%4]);s_state.status_error=false;
 }
 xSemaphoreGive(s_state_lock);if(save)enqueue(WORK_POWER_SAVE);return true;
}

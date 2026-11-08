#include "feishu_power.h"
#include "feishu_network.h"
#include "bsp_display.h"
#include "bsp_audio.h"
#include "esp_timer.h"
#include "esp_pm.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include <stdatomic.h>
static const char *TAG="feishu_power";
static portMUX_TYPE s_lock=portMUX_INITIALIZER_UNLOCKED;
static feishu_power_settings_t s_settings={2,2,0,2};
static atomic_uint s_activity,s_busy,s_retry_at;
static atomic_bool s_dark,s_cpu_saving,s_wifi_saving,s_retry_pending;
static esp_pm_lock_handle_t s_cpu,s_awake;
static bool s_pm_ready,s_locked=true,s_wake_guard;
static uint32_t s_guard_at;
static uint32_t now_ms(void){return (uint32_t)(esp_timer_get_time()/1000);}
feishu_power_settings_t feishu_power_get_settings(void)
{taskENTER_CRITICAL(&s_lock);feishu_power_settings_t s=s_settings;taskEXIT_CRITICAL(&s_lock);return s;}
void feishu_power_set_settings(const feishu_power_settings_t *s)
{if(!feishu_power_valid(s))return;taskENTER_CRITICAL(&s_lock);s_settings=*s;taskEXIT_CRITICAL(&s_lock);}
static feishu_power_plan_t plan(void)
{return feishu_power_plan(feishu_power_get_settings(),(uint32_t)(now_ms()-atomic_load(&s_activity)),atomic_load(&s_busy)!=0,bsp_audio_active());}
void feishu_power_init(void)
{
 atomic_store(&s_activity,now_ms());
 esp_err_t e=esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX,0,"feishu_work",&s_cpu);
 if(e==ESP_OK)e=esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP,0,"feishu_awake",&s_awake);
 if(e==ESP_OK)e=esp_pm_lock_acquire(s_cpu);
 if(e==ESP_OK)e=esp_pm_lock_acquire(s_awake);
 const esp_pm_config_t cfg={.max_freq_mhz=160,.min_freq_mhz=40,.light_sleep_enable=true};
 if(e==ESP_OK)e=esp_pm_configure(&cfg);
 s_pm_ready=e==ESP_OK;ESP_LOGI(TAG,"POWER_INIT ready=%u result=%d",s_pm_ready,e);
}
void feishu_power_activity(void){atomic_store(&s_activity,now_ms());}
void feishu_power_busy_begin(void){atomic_fetch_add(&s_busy,1);}
void feishu_power_busy_end(void){if(atomic_load(&s_busy))atomic_fetch_sub(&s_busy,1);}
void feishu_power_ui_tick(void)
{
 feishu_power_plan_t p=plan();
 if(p.screen_off!=atomic_load(&s_dark)){
  atomic_store(&s_dark,p.screen_off);bsp_display_backlight(p.screen_off ? 0:70);
  ESP_LOGI(TAG,"POWER_SCREEN off=%u",p.screen_off);
 }
 if(s_pm_ready && p.cpu_save==s_locked){
  if(p.cpu_save){esp_pm_lock_release(s_awake);esp_pm_lock_release(s_cpu);}
  else {esp_pm_lock_acquire(s_cpu);esp_pm_lock_acquire(s_awake);}
  s_locked=!p.cpu_save;atomic_store(&s_cpu_saving,p.cpu_save);
  ESP_LOGI(TAG,"POWER_CPU saving=%u",p.cpu_save);
 }
}
bool feishu_power_key(void)
{
 uint32_t n=now_ms();bool consume=false;
 if(atomic_load(&s_dark)){s_wake_guard=true;s_guard_at=n;consume=true;}
 else if(s_wake_guard){consume=(uint32_t)(n-s_guard_at)<600u;if(!consume)s_wake_guard=false;}
 feishu_power_activity();feishu_power_ui_tick();return consume;
}
bool feishu_power_screen_off(void){return atomic_load(&s_dark);}
bool feishu_power_background_allowed(void){return plan().background;}
bool feishu_power_wifi_requested(void){return plan().wifi_save;}
bool feishu_power_network_pending(void)
{return feishu_network_connected() && plan().wifi_save!=atomic_load(&s_wifi_saving) && (!atomic_load(&s_retry_pending) || (int32_t)(now_ms()-atomic_load(&s_retry_at))>=0);}
void feishu_power_network_poll(void)
{
 if(!feishu_network_connected()){atomic_store(&s_wifi_saving,false);return;}
 if(atomic_load(&s_retry_pending) && (int32_t)(now_ms()-atomic_load(&s_retry_at))<0)return;
 atomic_store(&s_retry_pending,false);
 bool save=plan().wifi_save;if(save==atomic_load(&s_wifi_saving))return;
 wifi_ps_type_t current;
 esp_err_t e=esp_wifi_get_ps(&current);
 if(e==ESP_OK && current!=(save ? WIFI_PS_MIN_MODEM:WIFI_PS_NONE)){
  e=esp_wifi_set_ps(save ? WIFI_PS_MIN_MODEM:WIFI_PS_NONE);
  if(e==ESP_OK)ESP_LOGI(TAG,"POWER_WIFI saving=%u reconnect=0",save);
 }
 if(e==ESP_OK)atomic_store(&s_wifi_saving,save);
 else {atomic_store(&s_retry_pending,true);atomic_store(&s_retry_at,now_ms()+5000u);ESP_LOGW(TAG,"POWER_WIFI result=%d retry_ms=5000",e);}
}
feishu_power_status_t feishu_power_status(void)
{return (feishu_power_status_t){(uint32_t)(now_ms()-atomic_load(&s_activity)),atomic_load(&s_dark),atomic_load(&s_cpu_saving),atomic_load(&s_wifi_saving),feishu_power_background_allowed(),s_pm_ready};}

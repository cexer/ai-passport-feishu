#include "feishu_power.h"
#include "esp_pm.h"
#include "esp_wifi.h"
#include <assert.h>
#include <stdio.h>
static uint32_t time_ms=1000;static bool audio,connected=true,fail_wifi;static unsigned backlight=70,sets;static wifi_ps_type_t mode;
struct power_test_lock {unsigned count;};static struct power_test_lock locks[2];
int64_t esp_timer_get_time(void){return (int64_t)time_ms*1000;}
bool bsp_audio_active(void){return audio;}bool feishu_network_connected(void){return connected;}
void bsp_display_backlight(uint8_t n){backlight=n;}
esp_err_t esp_pm_lock_create(int k,int unused,const char *name,esp_pm_lock_handle_t *p){(void)unused;(void)name;*p=&locks[k];return ESP_OK;}
esp_err_t esp_pm_lock_acquire(esp_pm_lock_handle_t p){++p->count;return ESP_OK;}
esp_err_t esp_pm_lock_release(esp_pm_lock_handle_t p){assert(p->count);--p->count;return ESP_OK;}
esp_err_t esp_pm_configure(const esp_pm_config_t *c){assert(c->max_freq_mhz==160&&c->min_freq_mhz==40&&c->light_sleep_enable);return ESP_OK;}
esp_err_t esp_wifi_get_ps(wifi_ps_type_t *p){*p=mode;return ESP_OK;}
esp_err_t esp_wifi_set_ps(wifi_ps_type_t p){++sets;if(fail_wifi)return ESP_FAIL;mode=p;return ESP_OK;}
int main(void){
 feishu_power_init();assert(locks[0].count==1&&locks[1].count==1);
 time_ms+=119999;feishu_power_ui_tick();assert(backlight==70&&!feishu_power_screen_off());
 ++time_ms;feishu_power_ui_tick();assert(backlight==0&&feishu_power_screen_off()&&!locks[0].count&&!locks[1].count);
 assert(feishu_power_network_pending());feishu_power_network_poll();assert(mode==WIFI_PS_MIN_MODEM&&sets==1);
 for(unsigned i=0;i<1000;++i){feishu_power_ui_tick();feishu_power_network_poll();}assert(sets==1); /* no mode thrash */
 assert(feishu_power_key()&&backlight==70&&locks[0].count==1);
 time_ms+=200;assert(feishu_power_key());time_ms+=601;assert(!feishu_power_key());
 feishu_power_network_poll();assert(mode==WIFI_PS_NONE&&sets==2);
 time_ms+=120000;audio=true;feishu_power_ui_tick();assert(backlight==0&&!feishu_power_status().cpu_saving&&locks[0].count==1);
 audio=false;feishu_power_ui_tick();assert(feishu_power_status().cpu_saving);
 feishu_power_busy_begin();feishu_power_ui_tick();assert(backlight==0&&!feishu_power_status().cpu_saving);feishu_power_busy_end();feishu_power_ui_tick();assert(feishu_power_status().cpu_saving);
 fail_wifi=true;feishu_power_network_poll();unsigned failed=sets;assert(!feishu_power_network_pending());time_ms+=4999;feishu_power_network_poll();assert(sets==failed);time_ms+=1;fail_wifi=false;feishu_power_network_poll();assert(mode==WIFI_PS_MIN_MODEM&&sets==failed+1);
 feishu_power_settings_t s=feishu_power_get_settings();s.cpu=0;s.wifi=0;s.refresh=1;s.screen_minutes=10;feishu_power_set_settings(&s);feishu_power_ui_tick();feishu_power_network_poll();assert(backlight==70&&!feishu_power_status().cpu_saving&&mode==WIFI_PS_NONE&&feishu_power_background_allowed());
 connected=false;feishu_power_network_poll();assert(!feishu_power_network_pending());
 puts("power runtime: wake safety/mode edge/no thrash/audio lock/background/retry/offline PASS");
}

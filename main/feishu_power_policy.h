#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define FEISHU_POWER_IDLE_MS 120000u
#define FEISHU_POWER_PREWARM_MS 60000u
enum {FEISHU_POWER_NO,FEISHU_POWER_YES,FEISHU_POWER_AUTO};
enum {FEISHU_REFRESH_FRONT,FEISHU_REFRESH_BACKGROUND,FEISHU_REFRESH_AUTO};
typedef struct {uint8_t screen_minutes,cpu,refresh,wifi;} feishu_power_settings_t;
typedef struct {bool screen_off,cpu_save,wifi_save,background;} feishu_power_plan_t;
static inline feishu_power_settings_t feishu_power_defaults(void)
{return (feishu_power_settings_t){2,FEISHU_POWER_AUTO,FEISHU_REFRESH_FRONT,FEISHU_POWER_AUTO};}
static inline bool feishu_power_valid(const feishu_power_settings_t *s)
{return s && s->screen_minutes>=2 && s->screen_minutes<=10 && s->cpu<=2 && s->refresh<=2 && s->wifi<=2;}
static inline bool feishu_power_decode(const uint8_t *b,size_t n,feishu_power_settings_t *s)
{
 if(!s){return false;}
 *s=feishu_power_defaults();if(!b || n!=5 || b[0]!=1)return false;
 feishu_power_settings_t t={b[1],b[2],b[3],b[4]};if(!feishu_power_valid(&t))return false;*s=t;return true;
}
static inline feishu_power_plan_t feishu_power_plan(feishu_power_settings_t s,uint32_t idle,bool busy,bool audio)
{
 bool automatic=idle>=FEISHU_POWER_IDLE_MS;
 return (feishu_power_plan_t){idle>=(uint32_t)s.screen_minutes*60000u,
  (s.cpu==FEISHU_POWER_YES || (s.cpu==FEISHU_POWER_AUTO && automatic)) && !busy && !audio,
  s.wifi==FEISHU_POWER_YES || (s.wifi==FEISHU_POWER_AUTO && automatic && !busy),
  s.refresh==FEISHU_REFRESH_BACKGROUND || (s.refresh==FEISHU_REFRESH_AUTO && !automatic)};
}
static inline bool feishu_power_prewarm_expired(uint32_t now,uint32_t started)
{return (uint32_t)(now-started)>=FEISHU_POWER_PREWARM_MS;}

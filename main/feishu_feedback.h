#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef enum { FEISHU_SOUND_KEY,FEISHU_SOUND_SUCCESS,FEISHU_SOUND_FAILURE,FEISHU_SOUND_BOOT } feishu_sound_t;
typedef struct {bool success,failure,key;} feishu_sound_settings_t;
unsigned feishu_feedback_samples(feishu_sound_t kind);
int16_t feishu_feedback_sample(feishu_sound_t kind,unsigned offset);
bool feishu_feedback_enabled(const feishu_sound_settings_t *s,feishu_sound_t kind);
/* Wrap-safe expiry: stale key clicks must not play after a long HTTPS request. */
bool feishu_feedback_fresh(feishu_sound_t kind,uint32_t requested,uint32_t now);

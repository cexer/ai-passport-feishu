#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct feishu_xiao_player feishu_xiao_player_t;
typedef esp_err_t (*feishu_xiao_pcm_sink_t)(void *context,const int16_t *pcm,size_t samples);
/* Worker-owned, synchronous, no task or audio driver ownership. Caller reserves
 * exclusive audio resources, negotiates Opus/24 kHz/mono/20 ms and owns mute.
 * PCM sink must copy/consume before returning; cancellation is checked per frame.
 * Always close on failure, cancellation, connection loss, or normal completion. */
esp_err_t feishu_xiao_player_open(feishu_xiao_player_t **out,unsigned rate,unsigned channels,
                                unsigned frame_ms,feishu_xiao_pcm_sink_t sink,void *context,
                                const volatile bool *cancel);
esp_err_t feishu_xiao_player_packet(feishu_xiao_player_t *p,uint8_t *packet,size_t size);
void feishu_xiao_player_close(feishu_xiao_player_t **p);

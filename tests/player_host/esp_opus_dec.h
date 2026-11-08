#pragma once
/* Decoder I/O fake only; actual Opus decoding requires an ESP target test. */
#include <stdint.h>
#include <stdbool.h>
typedef enum { ESP_AUDIO_ERR_OK,ESP_AUDIO_ERR_MEM_LACK,ESP_AUDIO_ERR_FAIL } esp_audio_err_t;
typedef enum { ESP_OPUS_DEC_FRAME_DURATION_20_MS=3 } esp_opus_dec_frame_duration_t;
typedef struct {uint32_t sample_rate;uint8_t channel;esp_opus_dec_frame_duration_t frame_duration;bool self_delimited;} esp_opus_dec_cfg_t;
typedef struct {uint8_t *buffer;uint32_t len,consumed;int frame_recover;} esp_audio_dec_in_raw_t;
typedef struct {uint8_t *buffer;uint32_t len,needed_size,decoded_size;} esp_audio_dec_out_frame_t;
typedef struct {uint32_t sample_rate;uint8_t bits_per_sample,channel;uint32_t bitrate,frame_size;} esp_audio_dec_info_t;
esp_audio_err_t esp_opus_dec_open(void *cfg,uint32_t size,void **handle);
esp_audio_err_t esp_opus_dec_decode(void *handle,esp_audio_dec_in_raw_t *raw,esp_audio_dec_out_frame_t *frame,esp_audio_dec_info_t *info);
esp_audio_err_t esp_opus_dec_close(void *handle);

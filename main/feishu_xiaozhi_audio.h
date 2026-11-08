#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FEISHU_XIAO_CAPTURE_SAMPLES 320
#define FEISHU_XIAO_QUEUE_SAMPLES 160
#define FEISHU_XIAO_QUEUE_FRAMES 120
#define FEISHU_XIAO_IDLE_FRAMES 80
#define FEISHU_XIAO_FRAME_MS 20
#define FEISHU_XIAO_PACKET_CAP (2+FEISHU_XIAO_QUEUE_SAMPLES*6/8)
typedef struct {uint16_t size;uint8_t bytes[FEISHU_XIAO_PACKET_CAP];} feishu_xiao_packet_t;

/* Same pair-average conversion as the PCM8 wire, before frame-scaled PCM6.
 * Input contains exactly one 16kHz/20ms frame. Its first half is overwritten.
 * A LE16 peak and 160 six-bit signed levels bound per-sample error by
 * peak/62 + 1. Code 63 and peaks above 32768 are rejected. No shared state.
 * Queue depth and wire duration stay unchanged; this is a RAM-only format. */
size_t feishu_xiao_audio_encode(int16_t *pcm,size_t samples,
    feishu_xiao_packet_t *packet);
bool feishu_xiao_audio_decode(const feishu_xiao_packet_t *packet,int16_t *pcm,size_t capacity);

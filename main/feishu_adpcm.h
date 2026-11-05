#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define FEISHU_ADPCM_SAMPLES 1600u
#define FEISHU_ADPCM_BYTES (4u+FEISHU_ADPCM_SAMPLES/2u)
typedef struct {int predictor;unsigned index;} feishu_adpcm_t;
/* Self-contained IMA 4-bit blocks: LE predictor, step index, reserved zero,
 * then high-nibble-first codes. State is carried by the encoder and included
 * in every block. This is lossy local buffering; wire audio remains PCM. */
size_t feishu_adpcm_encode(feishu_adpcm_t *state,const int16_t *pcm,size_t samples,uint8_t *out,size_t capacity);
bool feishu_adpcm_decode(const uint8_t *block,size_t bytes,size_t samples,int16_t *pcm,size_t capacity);

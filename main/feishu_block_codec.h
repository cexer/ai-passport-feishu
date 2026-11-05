#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FEISHU_BLOCK_BYTES 1024
#define FEISHU_CODEC_RAW 0
#define FEISHU_CODEC_HEATSHRINK_8_4 1

/* Serialized field values; callers encode fields explicitly, not struct bytes. */
typedef struct {
    uint32_t crc32;
    uint16_t raw_size;
    uint16_t stored_size;
    uint8_t codec;
} feishu_block_info_t;

/* Buffers must not overlap. No heap allocation; bounded work for one block. */
bool feishu_block_pack(const uint8_t *input, size_t size, bool compress,
                       uint8_t *output, size_t capacity, feishu_block_info_t *info);
bool feishu_block_unpack(const uint8_t *input, size_t size, const feishu_block_info_t *info,
                         uint8_t *output, size_t capacity);
uint32_t feishu_block_crc32(const uint8_t *data,size_t size);

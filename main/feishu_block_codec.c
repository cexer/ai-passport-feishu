#include "feishu_block_codec.h"
#include "heatshrink_encoder.h"
#include "heatshrink_decoder.h"
#include <string.h>

uint32_t feishu_block_crc32(const uint8_t *data, size_t size)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static size_t encode(const uint8_t *input, size_t size, uint8_t *output, size_t capacity)
{
    heatshrink_encoder enc;
    heatshrink_encoder_reset(&enc);
    size_t pos = 0, used = 0;
    for (unsigned steps = 0; steps < FEISHU_BLOCK_BYTES * 4; ++steps) {
        size_t count = 0;
        if (pos < size) {
            if (heatshrink_encoder_sink(&enc, (uint8_t *)input + pos, size-pos, &count) < 0) return 0;
            pos += count;
        }
        if (pos == size && heatshrink_encoder_finish(&enc) == HSER_FINISH_DONE) return used;
        HSE_poll_res poll;
        do {
            if (used == capacity) return 0;
            poll = heatshrink_encoder_poll(&enc, output+used, capacity-used, &count);
            if (poll < 0) return 0;
            used += count;
        } while (poll == HSER_POLL_MORE);
    }
    return 0;
}

bool feishu_block_pack(const uint8_t *input, size_t size, bool compress,
                       uint8_t *output, size_t capacity, feishu_block_info_t *info)
{
    if (!input || !output || !info || size == 0 || size > FEISHU_BLOCK_BYTES || capacity < size) return false;
    size_t stored = compress ? encode(input, size, output, size) : 0;
    info->codec = stored > 0 && stored < size ? FEISHU_CODEC_HEATSHRINK_8_4 : FEISHU_CODEC_RAW;
    if (info->codec == FEISHU_CODEC_RAW) {
        memcpy(output, input, size);
        stored = size;
    }
    info->raw_size = (uint16_t)size;
    info->stored_size = (uint16_t)stored;
    info->crc32 = feishu_block_crc32(input, size);
    return true;
}

bool feishu_block_unpack(const uint8_t *input, size_t size, const feishu_block_info_t *info,
                         uint8_t *output, size_t capacity)
{
    if (!input || !info || !output || info->raw_size == 0 || info->raw_size > FEISHU_BLOCK_BYTES ||
        info->stored_size == 0 || info->stored_size > FEISHU_BLOCK_BYTES ||
        size != info->stored_size || capacity < info->raw_size) return false;
    if (info->codec == FEISHU_CODEC_RAW) {
        if (info->stored_size != info->raw_size) return false;
        memcpy(output, input, info->raw_size);
    } else if (info->codec == FEISHU_CODEC_HEATSHRINK_8_4) {
        heatshrink_decoder dec;
        heatshrink_decoder_reset(&dec);
        size_t pos = 0, used = 0;
        bool finished = false;
        for (unsigned steps = 0; steps < FEISHU_BLOCK_BYTES * 4; ++steps) {
            size_t count = 0;
            if (pos < info->stored_size) {
                if (heatshrink_decoder_sink(&dec, (uint8_t *)input+pos, info->stored_size-pos, &count) < 0) return false;
                pos += count;
            }
            HSD_poll_res poll;
            do {
                /* Extra byte detects a stream expanding past its declared size. */
                uint8_t extra;
                size_t remaining = info->raw_size-used;
                poll = heatshrink_decoder_poll(&dec, remaining ? output+used : &extra,
                                                remaining ? remaining : 1, &count);
                if (poll < 0 || count > remaining) return false;
                used += count;
            } while (poll == HSDR_POLL_MORE);
            if (pos == info->stored_size && heatshrink_decoder_finish(&dec) == HSDR_FINISH_DONE) {
                finished = true;
                break;
            }
        }
        if (!finished || used != info->raw_size) return false;
    } else return false;
    return feishu_block_crc32(output, info->raw_size) == info->crc32;
}

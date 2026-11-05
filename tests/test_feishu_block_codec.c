#include "feishu_block_codec.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    uint8_t input[FEISHU_BLOCK_BYTES], stored[FEISHU_BLOCK_BYTES], output[FEISHU_BLOCK_BYTES+1];
    feishu_block_info_t info;
    memset(input, 'a', sizeof(input));
    assert(feishu_block_pack(input, sizeof(input), true, stored, sizeof(stored), &info));
    assert(info.codec == FEISHU_CODEC_HEATSHRINK_8_4 && info.stored_size < 128);
    assert(feishu_block_unpack(stored, info.stored_size, &info, output, sizeof(output)));
    assert(!memcmp(input, output, sizeof(input)));
    assert(!feishu_block_unpack(stored, info.stored_size-1, &info, output, sizeof(output)));
    /* CRC corruption, truncated input and expansion bound all fail, then recover. */
    info.crc32 ^= 1;
    assert(!feishu_block_unpack(stored, info.stored_size, &info, output, sizeof(output)));
    info.crc32 ^= 1;
    uint16_t saved = info.stored_size;
    info.stored_size /= 2;
    assert(!feishu_block_unpack(stored, info.stored_size, &info, output, sizeof(output)));
    info.stored_size = saved;
    info.raw_size = 3;
    output[3] = 0x7b;
    assert(!feishu_block_unpack(stored, info.stored_size, &info, output, sizeof(output)));
    assert(output[3] == 0x7b);
    uint32_t rng = 42;
    /* Every block length, including arbitrary binary data and raw fallback. */
    for (size_t size = 1; size <= FEISHU_BLOCK_BYTES; ++size) {
        for (size_t i = 0; i < size; ++i) {
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            input[i] = (uint8_t)rng;
        }
        assert(feishu_block_pack(input, size, true, stored, sizeof(stored), &info));
        assert(info.stored_size <= size);
        assert(feishu_block_unpack(stored, info.stored_size, &info, output, sizeof(output)));
        assert(!memcmp(input, output, size));
    }
    assert(feishu_block_pack(input, sizeof(input), false, stored, sizeof(stored), &info));
    assert(info.codec == FEISHU_CODEC_RAW);
    assert(!feishu_block_pack(input, sizeof(input)+1, true, stored, sizeof(stored), &info));
    assert(!feishu_block_unpack(stored, info.stored_size, &info, output, 2));
    /* Process a large logical body with the same three fixed buffers. */
    size_t total_stored = 0;
    for (unsigned block = 0; block < 300; ++block) {
        for (size_t i = 0; i < sizeof(input); ++i)
            input[i] = (uint8_t)("message text repeated for bounded compression "[i % 45]);
        assert(feishu_block_pack(input, sizeof(input), true, stored, sizeof(stored), &info));
        total_stored += info.stored_size;
        assert(feishu_block_unpack(stored, info.stored_size, &info, output, sizeof(output)));
        assert(!memcmp(input, output, sizeof(input)));
    }
    printf("300 KiB synthetic text -> %zu bytes; fixed block buffers\n", total_stored);
    puts("feishu block codec: PASS");
    return 0;
}

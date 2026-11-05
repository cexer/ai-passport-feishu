/* FAL driver scoped strictly to the synthetic probe's named app-data pool. */
#include "fal.h"
#include "esp_partition.h"
#include <string.h>

#define PROBE_BYTES (640u * 1024u)
static const esp_partition_t *pool;

static int init(void)
{
    pool = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                    ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "feishu_img");
    return pool && pool->size >= PROBE_BYTES ? 0 : -1;
}

static bool valid(long offset, size_t size)
{
    return pool && offset >= 0 && (size_t)offset <= PROBE_BYTES && size <= PROBE_BYTES-(size_t)offset;
}

static int read_bytes(long offset, uint8_t *buffer, size_t size)
{
    return valid(offset, size) && esp_partition_read(pool, offset, buffer, size) == ESP_OK ? (int)size : -1;
}

static int write_bytes(long offset, const uint8_t *buffer, size_t size)
{
    return valid(offset, size) && esp_partition_write(pool, offset, buffer, size) == ESP_OK ? (int)size : -1;
}

static int erase_bytes(long offset, size_t size)
{
    return valid(offset, size) && offset % 4096 == 0 && size % 4096 == 0 &&
           esp_partition_erase_range(pool, offset, size) == ESP_OK ? (int)size : -1;
}

const struct fal_flash_dev probe_flash = {
    .name="probe-flash", .addr=0, .len=PROBE_BYTES, .blk_size=4096,
    .ops={init, read_bytes, write_bytes, erase_bytes}, .write_gran=8,
};

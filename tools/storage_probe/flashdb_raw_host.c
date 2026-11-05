/* Host-only NOR model: backing memory is test equipment, never device RAM. */
#include "fal.h"
#include <stdbool.h>
#include <string.h>
#define CAPACITY (640u*1024u)
static uint8_t flash[CAPACITY];
static int writes_left = -1;
static bool partial_write;

void probe_flash_reset(void)
{
    memset(flash, 0xff, sizeof(flash));
    writes_left = -1;
}

void probe_flash_fault(int count, bool partial)
{
    writes_left = count;
    partial_write = partial;
}

static int init(void) { return 0; }
static bool valid(long pos, size_t size)
{
    return pos >= 0 && (size_t)pos <= CAPACITY && size <= CAPACITY-(size_t)pos;
}
static int read_bytes(long pos, uint8_t *out, size_t size)
{
    if (!valid(pos, size)) return -1;
    memcpy(out, flash+pos, size);
    return (int)size;
}
static int write_bytes(long pos, const uint8_t *input, size_t size)
{
    if (!valid(pos, size)) return -1;
    bool fail = writes_left == 0;
    if (writes_left > 0) --writes_left;
    size_t actual = fail ? (partial_write ? (size+1)/2 : 0) : size;
    for (size_t i = 0; i < actual; ++i) flash[pos+i] &= input[i];
    return fail ? -1 : (int)size;
}
static int erase_bytes(long pos, size_t size)
{
    if (!valid(pos, size) || pos % 4096 || size % 4096) return -1;
    if (writes_left == 0) return -1;
    if (writes_left > 0) --writes_left;
    memset(flash+pos, 0xff, size);
    return (int)size;
}
const struct fal_flash_dev probe_flash = {
    .name="probe-flash", .len=CAPACITY, .blk_size=4096,
    .ops={init, read_bytes, write_bytes, erase_bytes}, .write_gran=8,
};

/* Single-partition host adapter; target tests use upstream FAL itself. */
static const struct fal_partition partition = {
    .magic_word=0x45503130, .name="pool", .flash_name="probe-flash", .len=CAPACITY,
};
int fal_init(void) { return 1; }
const struct fal_flash_dev *fal_flash_device_find(const char *name)
{ return !strcmp(name, probe_flash.name) ? &probe_flash : NULL; }
const struct fal_partition *fal_partition_find(const char *name)
{ return !strcmp(name, partition.name) ? &partition : NULL; }
int fal_partition_read(const struct fal_partition *part, uint32_t pos, uint8_t *out, size_t size)
{ return part == &partition ? read_bytes(pos, out, size) : -1; }
int fal_partition_write(const struct fal_partition *part, uint32_t pos, const uint8_t *data, size_t size)
{ return part == &partition ? write_bytes(pos, data, size) : -1; }
int fal_partition_erase(const struct fal_partition *part, uint32_t pos, size_t size)
{ return part == &partition ? erase_bytes(pos, size) : -1; }

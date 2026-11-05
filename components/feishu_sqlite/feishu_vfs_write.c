#include "feishu_vfs_write.h"
#include <errno.h>
#include <stdint.h>

static unsigned recovery_depth, recovery_remaining;

void feishu_vfs_recovery_begin(void)
{ if (recovery_depth++ == 0) recovery_remaining = 2; }
void feishu_vfs_recovery_end(void)
{ if (recovery_depth && --recovery_depth == 0) recovery_remaining = 0; }
bool feishu_vfs_recovery_claim(void)
{
    if (!recovery_depth || !recovery_remaining) return false;
    --recovery_remaining;
    return true;
}

ptrdiff_t feishu_vfs_write_complete(const feishu_vfs_writer_t *writer,
                                   const void *data, size_t size)
{
    if (!writer || !writer->write || (!data && size) || size > PTRDIFF_MAX) {
        errno = EINVAL;
        return -1;
    }
    size_t offset = 0;
    unsigned collections = 0, interrupts = 0;
    while (offset < size) {
        ptrdiff_t written = writer->write(writer->context,
            (const unsigned char *)data + offset, size - offset);
        if (written > 0 && (size_t)written <= size - offset) {
            offset += (size_t)written;
            continue;
        }
        if (written >= 0) {
            errno = EIO;
            return -1;
        }
        int error = errno;
        if (error == EINTR && interrupts++ < 8) continue;
        if (error == ENOSPC && writer->collect && collections < 2) {
            unsigned attempt = ++collections;
            if (writer->collect(writer->context, attempt)) continue;
        }
        errno = error;
        return -1;
    }
    return (ptrdiff_t)offset;
}

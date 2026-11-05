#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    ptrdiff_t (*write)(void *context, const void *data, size_t size);
    /* At most two collection attempts per complete write. A partial
     * collection may return true to permit one more actual write attempt. */
    bool (*collect)(void *context, unsigned attempt);
    void *context;
} feishu_vfs_writer_t;

/* No allocation. Complete short writes, bound EINTR/ENOSPC retries, preserve
 * the terminal errno. An error may follow a written prefix; SQLite must roll
 * back that transaction, never acknowledge it as durable. */
ptrdiff_t feishu_vfs_write_complete(const feishu_vfs_writer_t *writer,
                                   const void *data, size_t size);

/* The cache adapter serializes these scopes with its recursive mutex.
 * Nested calls, and workers admitted while a reclamation loop yields that
 * mutex, share one recovery budget until all admitted operations finish. */
void feishu_vfs_recovery_begin(void);
void feishu_vfs_recovery_end(void);
bool feishu_vfs_recovery_claim(void);

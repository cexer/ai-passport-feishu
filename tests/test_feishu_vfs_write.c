#include "feishu_vfs_write.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const int *results;
    size_t result_count, next, used, calls;
    unsigned collected;
    bool collect_ok;
    unsigned char output[64];
} fixture_t;

static ptrdiff_t scripted_write(void *context, const void *data, size_t size)
{
    fixture_t *f = context;
    ++f->calls;
    int result = f->next < f->result_count ? f->results[f->next++] : (int)size;
    if (result < 0) { errno = -result; return -1; }
    if ((size_t)result > size) result = (int)size;
    assert(f->used + (size_t)result <= sizeof(f->output));
    memcpy(f->output + f->used, data, (size_t)result);
    f->used += (size_t)result;
    return result;
}
static bool collect(void *context, unsigned attempt)
{
    fixture_t *f = context;
    assert(attempt == ++f->collected && attempt <= 2);
    errno = ENOMEM; /* Recovery's errno must not replace the write failure. */
    return f->collect_ok;
}
static feishu_vfs_writer_t writer(fixture_t *f)
{ return (feishu_vfs_writer_t){scripted_write, collect, f}; }

int main(void)
{
    assert(!feishu_vfs_recovery_claim());
    feishu_vfs_recovery_begin();
    assert(feishu_vfs_recovery_claim());
    feishu_vfs_recovery_begin();
    assert(feishu_vfs_recovery_claim() && !feishu_vfs_recovery_claim());
    feishu_vfs_recovery_end();
    assert(!feishu_vfs_recovery_claim());
    feishu_vfs_recovery_end();
    feishu_vfs_recovery_begin();
    assert(feishu_vfs_recovery_claim() && feishu_vfs_recovery_claim());
    assert(!feishu_vfs_recovery_claim());
    feishu_vfs_recovery_end();
    assert(!feishu_vfs_recovery_claim());
    const unsigned char payload[] = {0, 1, 2, '"', '\\', 0xe4, 0xb8, 0xad, 255, 9, 10};
    const int mixed[] = {3, -EINTR, -ENOSPC, 2, -ENOSPC};
    fixture_t f = {.results=mixed, .result_count=5, .collect_ok=true};
    feishu_vfs_writer_t w = writer(&f);
    assert(feishu_vfs_write_complete(&w, payload, sizeof(payload)) == sizeof(payload));
    assert(f.collected == 2 && f.used == sizeof(payload));
    assert(!memcmp(f.output, payload, sizeof(payload)));

    const int full[] = {-ENOSPC, -ENOSPC, -ENOSPC, -ENOSPC};
    f = (fixture_t){.results=full, .result_count=4, .collect_ok=true}; w=writer(&f);
    assert(feishu_vfs_write_complete(&w, payload, sizeof(payload)) == -1);
    assert(errno == ENOSPC && f.collected == 2 && f.calls == 3 && !f.used);
    /* A failed transaction must not poison the next write or its budget. */
    f.results=NULL; f.result_count=0; f.next=0; f.collected=0;
    assert(feishu_vfs_write_complete(&w, payload, sizeof(payload)) == sizeof(payload));
    assert(f.used == sizeof(payload) && !memcmp(f.output, payload, sizeof(payload)));

    f = (fixture_t){.results=full, .result_count=4, .collect_ok=false}; w=writer(&f);
    assert(feishu_vfs_write_complete(&w, payload, sizeof(payload)) == -1);
    assert(errno == ENOSPC && f.collected == 1 && f.calls == 1);

    const int failed[] = {2, -EIO};
    f = (fixture_t){.results=failed, .result_count=2, .collect_ok=true}; w=writer(&f);
    assert(feishu_vfs_write_complete(&w, payload, sizeof(payload)) == -1);
    assert(errno == EIO && f.used == 2 && f.collected == 0);

    const int zero[] = {1, 0};
    f = (fixture_t){.results=zero, .result_count=2}; w=writer(&f);
    assert(feishu_vfs_write_complete(&w, payload, sizeof(payload)) == -1);
    assert(errno == EIO && f.used == 1 && f.calls == 2);

    int interrupted[10];for (unsigned i=0;i<10;++i)interrupted[i]=-EINTR;
    f = (fixture_t){.results=interrupted, .result_count=10}; w=writer(&f);
    assert(feishu_vfs_write_complete(&w, payload, sizeof(payload)) == -1);
    assert(errno == EINTR && f.calls == 9 && f.collected == 0);

    f = (fixture_t){0}; w=writer(&f); w.collect=NULL;
    assert(feishu_vfs_write_complete(&w, payload, sizeof(payload)) == sizeof(payload));
    assert(!memcmp(f.output, payload, sizeof(payload)));
    assert(feishu_vfs_write_complete(&w, NULL, 0) == 0);
    assert(feishu_vfs_write_complete(&w, NULL, 1) == -1 && errno == EINVAL);
    assert(feishu_vfs_write_complete(NULL, payload, sizeof(payload)) == -1 && errno == EINVAL);
    puts("VFS writes: short writes, binary identity, bounded EINTR/space recovery and terminal failures PASS");
    return 0;
}

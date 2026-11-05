#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Serialized, single-owner raw KV engine. Callbacks return the exact number
 * of transferred bytes, or a negative error. Addresses are pool-relative.
 * Callers must keep ctx alive until close; no SPIFFS or filesystem is used. */
typedef struct {
    void *ctx;
    size_t size;
    int (*read)(void *, uint32_t, void *, size_t);
    int (*write)(void *, uint32_t, const void *, size_t);
    int (*erase)(void *, uint32_t, size_t);
} feishu_fdb_io_t;
enum {
    FEISHU_FDB_OK, FEISHU_FDB_ERROR, FEISHU_FDB_IO,
    FEISHU_FDB_FORMAT, FEISHU_FDB_FULL, FEISHU_FDB_NOTFOUND,
    FEISHU_FDB_TOOBIG, FEISHU_FDB_BUSY, FEISHU_FDB_INVALID
};
/* allow_blank only permits formatting after checking EVERY byte is erased.
 * Occupied or unrecognized sectors are never automatically reformatted. A
 * recognized raw DB may complete a canonical EMPTY header only after proving
 * its entire sector payload is erased, as required after interrupted GC.
 * An IO failure latches until close/reopen; no further raw mutations occur. */
int feishu_fdb_open(const feishu_fdb_io_t *io, bool allow_blank);
void feishu_fdb_close(void);
int feishu_fdb_status(void);
int feishu_fdb_put(const char *key, const void *data, size_t size);
int feishu_fdb_get(const char *key, void *data, size_t capacity, size_t *size);
int feishu_fdb_delete(const char *key);
/* A serialized visitor may perform get calls, but must not mutate the engine
 * or start another visit. Return true to stop early. */
typedef bool (*feishu_fdb_visit_t)(const char *key, size_t size, void *ctx);
int feishu_fdb_visit(feishu_fdb_visit_t visitor,void *ctx);
/* Shared working room; two erase sectors are held for garbage collection. */
bool feishu_fdb_room(size_t margin);
/* Changes on every raw mutation attempt and open/close. RAM proofs must not
 * survive this epoch; every requested chunk still receives its own CRC check. */
uint64_t feishu_fdb_revision(void);

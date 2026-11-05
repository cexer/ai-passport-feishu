#pragma once
#include "feishu_block_codec.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    FEISHU_DATA_METADATA = 0, FEISHU_DATA_TEXT, FEISHU_DATA_IMAGE,
    FEISHU_DATA_AUDIO, FEISHU_DATA_REQUIRED, FEISHU_DATA_KINDS
} feishu_data_kind_t;

/* Backend-independent results. Values retain the existing storage ABI while
 * callers migrate away from the SQLite implementation. Native diagnostics
 * are available separately and must never drive business decisions. */
enum {
    FEISHU_DATA_OK=0, FEISHU_DATA_ERROR=1, FEISHU_DATA_BUSY=5,
    FEISHU_DATA_LOCKED=6, FEISHU_DATA_NOMEM=7, FEISHU_DATA_READONLY=8,
    FEISHU_DATA_IOERR=10, FEISHU_DATA_CORRUPT=11, FEISHU_DATA_NOTFOUND=12,
    FEISHU_DATA_FULL=13, FEISHU_DATA_CANTOPEN=14, FEISHU_DATA_TOOBIG=18,
    FEISHU_DATA_CONSTRAINT=19, FEISHU_DATA_MISUSE=21, FEISHU_DATA_BAD_FORMAT=26,
    FEISHU_DATA_DONE=101
};

/* One serialized owner uses this store. IDs identify immutable versions.
 * db is an opaque legacy ABI slot: only the backend and its own fault tests
 * may inspect it. Application code uses is_open/diagnostics instead. */
typedef struct { void *db; } feishu_data_t;
typedef struct { int64_t id; uint64_t raw, stored; unsigned sequence; feishu_data_kind_t kind; bool quota_limited; } feishu_data_writer_t;
typedef struct { int64_t id; uint64_t raw, stored; bool protected_data; } feishu_data_object_t;

typedef struct {int native_code,system_code;} feishu_data_diagnostics_t;
bool feishu_data_is_open(const feishu_data_t *store);
/* An engine may require reopen after an IO failure, without corruption or an
 * implicit destructive rebuild. Reopening invalidates outstanding leases. */
bool feishu_data_needs_reopen(const feishu_data_t *store);
feishu_data_diagnostics_t feishu_data_diagnostics(const feishu_data_t *store);
const char *feishu_data_backend_name(void);
/* Serialized operation scopes, including nested calls. A backend uses them
 * to bound its recovery work; they do not create a business transaction. */
void feishu_data_io_begin(void);
void feishu_data_io_end(void);

/* Return FEISHU_DATA_* results; no implicit format or destructive recovery.
 * path/vfs identify the existing file backend; a raw backend may treat path
 * as a namespace and reject an unsupported vfs. */
int feishu_data_open(feishu_data_t *store, const char *path, const char *vfs);
int feishu_data_close(feishu_data_t *store);
/* Engine corruption only; a bad object CRC does not set this flag. */
bool feishu_data_broken(feishu_data_t *store);
/* Preserve IDs still referenced by live readers/writers across a DB rebuild. */
int feishu_data_id_floor(feishu_data_t *store,uint64_t *floor);
void feishu_data_trim(feishu_data_t *store);
int feishu_data_shrink(feishu_data_t *store);
int feishu_data_begin(feishu_data_t *store, const char *key, feishu_data_kind_t kind,
                       bool protected_data, feishu_data_writer_t *writer);
int feishu_data_append(feishu_data_t *store, feishu_data_writer_t *writer,
                        const uint8_t *data, size_t size, bool compress);
/* Up to eight blocks in one durable transaction, using the caller's existing
 * contiguous buffer. Failure restores both disk rows and the writer cursor. */
int feishu_data_append_span(feishu_data_t *store,feishu_data_writer_t *writer,
                             const uint8_t *data,size_t size,bool compress);
int feishu_data_commit(feishu_data_t *store, feishu_data_writer_t *writer);
int feishu_data_commit_as(feishu_data_t *store, feishu_data_writer_t *writer,const char *key);
/* Publish a body plus small recovery metadata atomically, optionally releasing
 * a predecessor's protection. NULL second means metadata-only state update. */
int feishu_data_commit_pair(feishu_data_t *store,feishu_data_writer_t *first,const char *first_key,
                             feishu_data_writer_t *second,const char *second_key,int64_t release);
int feishu_data_abort(feishu_data_t *store, feishu_data_writer_t *writer);
int feishu_data_find(feishu_data_t *store, const char *key, feishu_data_object_t *object);
int feishu_data_stat(feishu_data_t *store,int64_t id,const char *key,feishu_data_object_t *object);
int feishu_data_discard_pending(feishu_data_t *store);
int feishu_data_release_orphan(feishu_data_t *store,const int64_t *active,size_t count,int64_t *released);
int feishu_data_read(feishu_data_t *store, int64_t id, unsigned sequence,
                      uint8_t *data, size_t capacity, size_t *size);
int feishu_data_pin(feishu_data_t *store, int64_t id, bool pinned);
int feishu_data_reclaim(feishu_data_t *store, size_t *bytes);
int feishu_data_reclaim_category(feishu_data_t *store,feishu_data_kind_t kind,size_t *bytes);

/* Transient mention maps become part of one immutable incoming text version.
 * Auxiliary metadata uses the existing METADATA quota and CRC protection. */
typedef struct {uint64_t raw,logical;} feishu_data_text_point_t;
typedef struct {int64_t map;uint64_t raw,logical;uint32_t metadata;} feishu_data_text_view_t;
int feishu_data_mention_put(feishu_data_t *store,feishu_data_writer_t *map,const char *key,const char *value);
/* NULL value at put records a verified literal marker. Get returns DONE for
 * that sentinel and NOTFOUND for a missing/corruptly removed dictionary key. */
int feishu_data_mention_get(feishu_data_t *store,int64_t map,const char *key,void *value,size_t capacity,size_t *size);
int feishu_data_text_index(feishu_data_t *store,const feishu_data_writer_t *body,feishu_data_writer_t *map,
                            const feishu_data_text_point_t *points,size_t count);
int feishu_data_text_index_clear(feishu_data_t *store,int64_t object);
int feishu_data_text_info(feishu_data_t *store,int64_t object,uint64_t raw,feishu_data_text_view_t *view);
int feishu_data_text_seek(feishu_data_t *store,int64_t object,const feishu_data_text_view_t *view,
                           uint64_t offset,feishu_data_text_point_t *point);
int feishu_data_text_publish(feishu_data_t *store,feishu_data_writer_t *body,feishu_data_writer_t *map,
                              uint64_t logical,const char *key);

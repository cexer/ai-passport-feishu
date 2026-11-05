#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
#include "feishu_data.h"
#include "feishu_outbox.h"

/* Worker-only cache API. Mount never formats a nonblank, damaged pool. */
esp_err_t feishu_cache_init(void);
void feishu_cache_set_scope(const char *app,const char *owner);
uint32_t feishu_cache_scope_version(void);
esp_err_t feishu_cache_mount(void);
/* Recovery configuration and DB operations share this recursive pool lock. */
esp_err_t feishu_cache_pool_lock(void);
void feishu_cache_pool_unlock(void);
esp_err_t feishu_cache_release_space(void);
esp_err_t feishu_cache_open(void);
void feishu_cache_close(void);
void feishu_cache_unmount(void);
/* UI-safe, no disk access. Nonzero means failed DB generations are retained. */
uint32_t feishu_cache_recovery_generation(void);
esp_err_t feishu_cache_read(const char *name,
                             void *data, size_t unit, size_t capacity, size_t *count,
                             int64_t *pin);
esp_err_t feishu_cache_write(const char *name,
                              const void *data, size_t unit, size_t count);
esp_err_t feishu_cache_write_if_scope(uint32_t version,const char *name,
                                       const void *data,size_t unit,size_t count);
void feishu_cache_unpin(int64_t *pin);
void feishu_cache_protect(const char *name,int64_t *pin);
esp_err_t feishu_cache_text_begin(feishu_data_writer_t *writer);
esp_err_t feishu_cache_draft_begin(feishu_data_writer_t *writer);
esp_err_t feishu_cache_pending_read(unsigned slot,feishu_outbox_t *pending);
/* new_body publishes a newly recorded draft and metadata in one transaction.
 * NULL updates durable progress; EMPTY releases its previous body's protection. */
esp_err_t feishu_cache_pending_write(unsigned slot,const feishu_outbox_t *pending,
                                      feishu_data_writer_t *new_body);
esp_err_t feishu_cache_text_append(feishu_data_writer_t *writer,const void *data,size_t size);
esp_err_t feishu_cache_text_commit(feishu_data_writer_t *writer,const char *message,int64_t *id);
esp_err_t feishu_cache_text_add_mention(feishu_data_writer_t *map,uint32_t scope,const char *key,const char *name);
/* Uses the caller's existing 1 KiB parser block. On metadata/resource failure,
 * preserve the original text/preview and return a literal representation. */
esp_err_t feishu_cache_text_prepare(feishu_data_writer_t *body,feishu_data_writer_t *map,uint32_t scope,
                                     bool metadata_valid,char *preview,size_t preview_capacity,
                                     void *workspace,size_t workspace_size,uint64_t *length,bool *mapped,
                                     bool (*cancelled)(void *),void *cancel_context);
esp_err_t feishu_cache_text_publish(feishu_data_writer_t *body,feishu_data_writer_t *map,uint32_t scope,
                                     uint64_t logical,const char *message,int64_t *id);
void feishu_cache_text_abort(feishu_data_writer_t *writer);
void feishu_cache_reap_abandoned(void);
esp_err_t feishu_cache_text_pin(const char *message,int64_t id,bool mapped,int64_t *pin,uint64_t *size);
esp_err_t feishu_cache_text_read(const char *message,int64_t id,bool mapped,uint64_t offset,
                                 void *data,size_t capacity,size_t *size);
/* Consecutive streaming reads reuse the existing bounded DB page cache.
 * Call feishu_cache_trim before TLS setup and after the stream completes. */
esp_err_t feishu_cache_text_read_stream(const char *message,int64_t id,bool mapped,uint64_t offset,
                                        void *data,size_t capacity,size_t *size);
/* Opaque fixed-block objects use the same pool, codec, quotas and reader leases. */
esp_err_t feishu_cache_blob_begin(feishu_data_kind_t kind,feishu_data_writer_t *writer);
/* Release the existing bounded SQLite page cache after a streaming reader
 * finishes, rather than throwing it away after every scanline/chunk. */
void feishu_cache_trim(void);
/* Append up to 8 KiB from an existing buffer in one transaction. All earlier
 * appends must end on a block boundary; only the final append may be short. */
esp_err_t feishu_cache_blob_append(feishu_data_writer_t *writer,const void *data,size_t size,bool compress);
esp_err_t feishu_cache_blob_find(const char *name,int64_t *id,uint64_t *size);
esp_err_t feishu_cache_blob_pin(const char *name,int64_t id,int64_t *pin,uint64_t *size);
esp_err_t feishu_cache_blob_read(const char *name,int64_t id,uint64_t offset,void *data,size_t capacity,size_t *size);
esp_err_t feishu_cache_blob_commit(feishu_data_writer_t *first,const char *name,
                                    feishu_data_writer_t *second,const char *second_name,int64_t *pin);

/* Lock-free: true only when all four slots were verified empty for this scope. */
bool feishu_cache_pending_empty(void);

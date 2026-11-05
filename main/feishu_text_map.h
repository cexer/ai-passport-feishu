#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FEISHU_TEXT_MAP_WORKSPACE 1024u
#define FEISHU_TEXT_MAP_NAME_BYTES 128u

typedef struct {
    bool (*read)(void *context,uint64_t offset,void *data,size_t capacity,size_t *size);
    /* -1 error, 0 unknown marker, 1 found. The value excludes the leading @. */
    int (*lookup)(void *context,const char *key,void *value,size_t capacity,size_t *size);
    bool (*checkpoint)(void *context,uint64_t raw_offset,uint64_t logical_offset);
    bool (*cancelled)(void *context);
    void *context;
} feishu_text_map_io_t;

/* Scan immutable original text, keeping only a prefix and sparse checkpoints.
 * Checkpoints occur between complete raw units, including complete mentions. */
bool feishu_text_map_scan(const feishu_text_map_io_t *io,uint64_t raw_size,
                           void *workspace,size_t workspace_size,char *preview,size_t preview_capacity,
                           uint64_t *logical_size);
/* Read normalized bytes from a preceding checkpoint. The caller may request
 * byte offsets inside UTF-8; the existing viewport layer aligns text edges. */
bool feishu_text_map_read(const feishu_text_map_io_t *io,uint64_t raw_size,
                           uint64_t start_raw,uint64_t start_logical,uint64_t offset,
                           void *data,size_t capacity,void *workspace,size_t workspace_size,size_t *size);

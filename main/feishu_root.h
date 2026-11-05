#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Small recovery records share the application filesystem with the database.
 * The caller serializes access. They remain readable if the disposable DB fails.
 * No preallocated region, malloc, or file-size-dependent workspace. */
enum { FEISHU_ROOT_OK, FEISHU_ROOT_MISSING, FEISHU_ROOT_CORRUPT,
       FEISHU_ROOT_IO, FEISHU_ROOT_FULL, FEISHU_ROOT_INVALID };
#define FEISHU_ROOT_MAX 16384u
typedef struct { const void *data; size_t size; } feishu_root_part_t;
typedef struct {
    const char *directory;
    /* Optional platform/fault-injection hooks; NULL selects ordinary file IO. */
    size_t (*write)(const void *data,size_t size,FILE *file);
    int (*sync)(FILE *file);
} feishu_root_t;

int feishu_root_read(const feishu_root_t *root,unsigned key,size_t offset,
                     void *data,size_t capacity,size_t *total);
/* NULL part data writes zeros. New generation is read back before replacing
 * its predecessor. Failure before that point preserves the previous record. */
int feishu_root_write(const feishu_root_t *root,unsigned key,
                      const feishu_root_part_t *parts,size_t count);

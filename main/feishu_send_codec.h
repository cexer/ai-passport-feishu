#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Below the documented 150 KB message request limit, including both JSON
 * encoding layers. Multipart boundaries always preserve whole UTF-8 scalars. */
#define FEISHU_SEND_BODY_LIMIT (128u*1024u)
typedef struct {
    bool (*read)(void *context,uint64_t offset,void *data,size_t capacity,size_t *size);
    void *context;
    uint64_t length;
} feishu_send_source_t;
typedef struct {
    uint64_t offset,length;
    size_t body_size,header_size;
    char header[320];
} feishu_send_part_t;
typedef bool (*feishu_send_sink_t)(void *context,const void *data,size_t size);

bool feishu_send_plan(const feishu_send_source_t *source,uint64_t offset,
                      const char *destination,const char *uuid,bool reply,
                      feishu_send_part_t *part);
bool feishu_send_emit(const feishu_send_source_t *source,const feishu_send_part_t *part,
                      feishu_send_sink_t sink,void *context);

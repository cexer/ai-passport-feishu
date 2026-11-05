#pragma once
#include "feishu_cache.h"
#include "feishu_messages_stream.h"

typedef struct {
    feishu_data_writer_t bodies[FEISHU_MAX_MESSAGES],maps[FEISHU_MAX_MESSAGES];
    uint32_t scope;
    bool (*cancelled)(void *);
    void *cancel_context;
} feishu_message_store_t;

void feishu_message_store_init(feishu_message_store_t *store,bool (*cancelled)(void *),void *context);
const feishu_stream_storage_t *feishu_message_store_operations(void);

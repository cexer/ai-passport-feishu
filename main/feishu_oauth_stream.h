#pragma once
#include "feishu_json.h"

typedef struct {
    feishu_json_t json;
    char *outputs[3];
    size_t capacities[3],lengths[3];
    unsigned selected,seen;
    bool root_object,overflow,invalid;
} feishu_oauth_stream_t;

void feishu_oauth_stream_init(feishu_oauth_stream_t *parser,char *access,size_t access_capacity,
                               char *refresh,size_t refresh_capacity,char *error,size_t error_capacity);
bool feishu_oauth_stream_feed(feishu_oauth_stream_t *parser,const void *data,size_t size);
bool feishu_oauth_stream_finish(feishu_oauth_stream_t *parser);

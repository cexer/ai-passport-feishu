#pragma once
#include "feishu_json.h"

/* Bounded envelope reader for data.recognition_text / data.message_id. The
 * consumer must not publish streamed bytes until finish succeeds. */
typedef bool (*feishu_result_sink_t)(void *context,const void *data,size_t size);
typedef struct {
    feishu_json_t json;
    feishu_result_sink_t sink;
    void *context;
    char field[32],*preview;
    size_t preview_capacity,preview_size,used;
    uint64_t length,limit;
    int code;
    uint8_t block[1024];
    unsigned key;
    bool root,code_seen,data_seen,in_data,field_seen,selected,in_string,preview_full,failed;
} feishu_result_stream_t;
void feishu_result_stream_init(feishu_result_stream_t *stream,const char *field,uint64_t limit,
                                char *preview,size_t capacity,feishu_result_sink_t sink,void *context);
bool feishu_result_stream_feed(feishu_result_stream_t *stream,const void *data,size_t size);
bool feishu_result_stream_finish(feishu_result_stream_t *stream);

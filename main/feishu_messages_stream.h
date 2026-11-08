#pragma once
#include "feishu_json.h"
#include "feishu_model.h"
#include "feishu_page.h"
#include "feishu_text_map.h"

typedef struct { char key[32],name[FEISHU_TEXT_MAP_NAME_BYTES+1]; } feishu_stream_mention_t;
typedef struct {
    bool (*begin)(void *context,size_t slot);
    bool (*write)(void *context,size_t slot,const uint8_t *data,size_t size);
    void (*reset)(void *context,size_t slot);
    bool (*mention)(void *context,size_t slot,const feishu_stream_mention_t *mention);
    bool (*prepare)(void *context,size_t slot,bool metadata_valid,uint64_t *length,bool *mapped,
                       char *preview,size_t preview_capacity,void *workspace,size_t workspace_size);
    bool (*commit)(void *context,size_t slot,const char *message,
                   int64_t *id,uint64_t *length);
    void (*abort)(void *context,size_t slot);
} feishu_stream_storage_t;

typedef struct {
    feishu_json_t outer,inner;
    feishu_stream_storage_t storage;
    void *storage_context;
    feishu_message_t *messages,current;
    size_t capacity,count,item_count,preview_size,text_used,mention_count,small_size;
    uint64_t text_length;
    int api_code;
    char self[FEISHU_CHAT_ID_MAX],type[32],small[FEISHU_TEXT_MAP_NAME_BYTES+1];
    feishu_stream_mention_t mention;
    uint8_t outer_role[FEISHU_JSON_DEPTH],outer_key[FEISHU_JSON_DEPTH];
    uint8_t inner_role[FEISHU_JSON_DEPTH],inner_key[FEISHU_JSON_DEPTH];
    uint8_t text_block[1024];
    char next_page[FEISHU_PAGE_TOKEN_MAX+1];
    size_t next_page_size;
    bool more,more_seen,next_seen,pagination_required;
    unsigned outer_route,inner_route,locale;
    bool failed,finished,code_seen,data_seen,items_seen,item_active,sender_seen,type_seen;
    bool item_invalid,body_seen,body_invalid,have_text,text_started,text_failed;
    bool preview_full,small_overflow,text_ready[FEISHU_MAX_MESSAGES];
    bool mapping_failed,mention_invalid,mentions_seen;
    bool update_seen,update_invalid;
} feishu_messages_stream_t;

void feishu_messages_stream_init(feishu_messages_stream_t *stream,feishu_message_t *messages,
                                  size_t capacity,const char *self,
                                  const feishu_stream_storage_t *storage,void *context);
bool feishu_messages_stream_feed(feishu_messages_stream_t *stream,const void *data,size_t size);
bool feishu_messages_stream_finish(feishu_messages_stream_t *stream,size_t *count);
void feishu_messages_stream_abort(feishu_messages_stream_t *stream);

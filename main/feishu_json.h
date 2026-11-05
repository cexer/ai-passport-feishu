#pragma once
#include "jsonsl.h"
#include <stdbool.h>
#include <stdint.h>

#define FEISHU_JSON_DEPTH 32
#define FEISHU_JSON_SMALL 64
typedef enum {
    FEISHU_JSON_OBJECT_BEGIN,FEISHU_JSON_OBJECT_END,
    FEISHU_JSON_ARRAY_BEGIN,FEISHU_JSON_ARRAY_END,FEISHU_JSON_KEY,
    FEISHU_JSON_STRING_BEGIN,FEISHU_JSON_STRING_PART,FEISHU_JSON_STRING_END,
    FEISHU_JSON_ATOM
} feishu_json_event_t;
typedef bool (*feishu_json_callback_t)(void *context,feishu_json_event_t event,
                                       unsigned depth,const uint8_t *data,size_t size);

typedef struct {
    /* Library storage is fixed and caller-owned; jsonsl_new's allocation path
     * is not used, so allocation failure cannot dereference a null parser. */
    union {
        struct jsonsl_st alignment;
        uint8_t bytes[sizeof(struct jsonsl_st)+(FEISHU_JSON_DEPTH-1)*sizeof(struct jsonsl_state_st)];
    } work;
    feishu_json_callback_t callback;
    void *context;
    uint64_t bytes_seen;
    uint32_t unicode,utf8_value,utf8_min;
    uint16_t high_surrogate;
    uint8_t escaped,hex_count,utf8_need,utf8_size;
    uint8_t utf8[4],small[FEISHU_JSON_SMALL];
    size_t small_size;
    unsigned string_depth,atom_depth;
    bool failed,complete,string_active,key_active,atom_active,small_overflow;
    bool string_popped,atom_popped;
} feishu_json_t;

void feishu_json_init(feishu_json_t *parser,feishu_json_callback_t callback,void *context);
void feishu_json_reset(feishu_json_t *parser);
bool feishu_json_feed(feishu_json_t *parser,const void *data,size_t size);
bool feishu_json_finish(feishu_json_t *parser);

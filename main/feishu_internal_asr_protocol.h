#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define FEISHU_INTERNAL_ASR_COMMAND 1305
#define FEISHU_INTERNAL_ASR_LIMIT (300u*1024u)
#define FEISHU_INTERNAL_ASR_PCM_LIMIT 25600u
typedef bool (*feishu_internal_asr_sink_t)(void *,const void *,size_t);
/* Header ends immediately before PCM bytes: upload header + PCM without copying. */
size_t feishu_internal_asr_header(uint8_t *out,size_t capacity,const char *source,
    const char *cid,const char *context_id,uint32_t sequence,unsigned action,size_t pcm_size,size_t *body_size);
typedef struct {
    feishu_internal_asr_sink_t sink;void *context;
    char source[65],cid[65],small[65],*preview;
    size_t preview_capacity,preview_size,used,small_size;
    uint8_t block[1024],utf8[4];
    uint64_t value,remaining,payload_left,total,text_length;
    uint32_t sequence,response_sequence,field,top_seen,inner_seen,status,error_code,handle_action;
    unsigned shift,wire,state,route,utf8_need,utf8_size;
    bool inner,failed,payload_seen,text_seen,is_end,preview_full;
} feishu_internal_asr_parser_t;
void feishu_internal_asr_init(feishu_internal_asr_parser_t *,const char *,const char *,uint32_t,
    char *,size_t,feishu_internal_asr_sink_t,void *);
bool feishu_internal_asr_feed(feishu_internal_asr_parser_t *,const void *,size_t);
bool feishu_internal_asr_finish(feishu_internal_asr_parser_t *);

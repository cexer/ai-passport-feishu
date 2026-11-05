#pragma once
#include <stdbool.h>
#include <stddef.h>
/* next returns a producer slot, -1 after all audio, or -2 on failure. An end
 * marker guarantees the producer will not touch zero_scratch again. */
typedef int (*feishu_asr_next_t)(void *,const void **);
typedef void (*feishu_asr_release_t)(void *,int);
typedef bool (*feishu_asr_batch_write_t)(void *,const void *,size_t);
bool feishu_asr_batch(unsigned blocks,size_t bytes,void *zero_scratch,
    feishu_asr_next_t next,feishu_asr_release_t release,void *source,
    feishu_asr_batch_write_t write,void *writer,bool *done);

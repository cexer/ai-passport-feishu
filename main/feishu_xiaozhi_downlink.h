#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One complete XiaoZhi binary WebSocket message, after WS unmasking.
 * No allocation based on network lengths. Reset after any stream error. */
#define FEISHU_XIAO_OPUS_CAP 2048u
typedef bool (*feishu_xiao_opus_sink_t)(void *context,uint8_t *packet,size_t size);
typedef struct {
    uint8_t bytes[FEISHU_XIAO_OPUS_CAP+16];
    size_t used;
    unsigned version;
    bool failed;
} feishu_xiao_downlink_t;
bool feishu_xiao_downlink_init(feishu_xiao_downlink_t *p,unsigned version);
bool feishu_xiao_downlink_feed(feishu_xiao_downlink_t *p,const void *data,size_t size);
/* fin=false permits WebSocket continuation; fin=true delivers exactly one packet.
 * Sink failure latches the error. Successful delivery immediately reuses storage. */
bool feishu_xiao_downlink_end(feishu_xiao_downlink_t *p,bool fin,
                            feishu_xiao_opus_sink_t sink,void *context);

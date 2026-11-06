#pragma once
#include "feishu_json.h"
enum { XIAO_MESSAGE_OTHER,XIAO_MESSAGE_HELLO,XIAO_MESSAGE_STT,XIAO_MESSAGE_ALERT };
#define XIAO_TEXT_CAP 2048u
typedef struct {
    feishu_json_t json;
    char type[24],session[96],text[XIAO_TEXT_CAP];
    size_t lengths[3];unsigned selected,seen,strings;
    bool root,invalid;
} feishu_xiao_message_t;
void feishu_xiao_message_init(feishu_xiao_message_t *p);
bool feishu_xiao_message_feed(feishu_xiao_message_t *p,const void *data,size_t size);
int feishu_xiao_message_finish(feishu_xiao_message_t *p);
/* Move payload forward in-place and prepend one masked FIN binary WS frame.
 * Failure leaves the buffer unchanged. Supports payloads up to 65535 bytes. */
size_t feishu_xiao_binary_frame(uint8_t *buffer,size_t capacity,size_t payload_size,const uint8_t mask[4]);

/* A server activation object takes precedence over a provisional WS config.
 * 1: needs pairing, 0: valid paired config, -1: malformed/unknown response. */
int feishu_xiao_pairing_classify(bool activation,const char *code,bool websocket_valid,char output[17]);

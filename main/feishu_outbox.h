#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define FEISHU_OUTBOX_SLOTS 4
typedef enum { FEISHU_OUTBOX_EMPTY,FEISHU_OUTBOX_READY,FEISHU_OUTBOX_SENDING,FEISHU_OUTBOX_UNCERTAIN } feishu_outbox_state_t;
typedef struct {
    uint32_t version,state,segment,reserved;
    int64_t object_id;
    uint64_t length,offset;
    char body_name[64],chat[64],message[64],uuid[36];
} feishu_outbox_t;

bool feishu_outbox_valid(const feishu_outbox_t *pending);
bool feishu_outbox_create(feishu_outbox_t *pending,int64_t object,uint64_t length,
                           const char *name,const char *chat,const char *message,const char *uuid);
bool feishu_outbox_uuid(const feishu_outbox_t *pending,char uuid[41]);
bool feishu_outbox_attempt(feishu_outbox_t *pending,bool confirm_uncertain);
bool feishu_outbox_ack(feishu_outbox_t *pending,uint64_t bytes);
void feishu_outbox_failed(feishu_outbox_t *pending,bool definitely_rejected);

#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Complete editable text only. A preview must never replace a longer draft. */
#define FEISHU_COMPOSE_CAP 512
#define FEISHU_COMPOSE_REV_CAP 18
typedef struct {
    char text[FEISHU_COMPOSE_CAP];
    char chat[64],message[64],uuid[41];
    uint64_t length,offset;
    unsigned state;
} feishu_compose_snapshot_t;

bool feishu_compose_editable(const feishu_compose_snapshot_t *draft);
bool feishu_compose_revision(const feishu_compose_snapshot_t *draft,
                             char revision[FEISHU_COMPOSE_REV_CAP]);
/* Candidate keeps the immutable recipient and reply context. Discussion has no
 * mutation API; only an explicit validated edit reaches this function. */
bool feishu_compose_replace(const feishu_compose_snapshot_t *current,
    const char *revision,const char *text,feishu_compose_snapshot_t *candidate);
bool feishu_compose_undo(const feishu_compose_snapshot_t *current,
    const char *revision,const feishu_compose_snapshot_t *previous,
    feishu_compose_snapshot_t *candidate);

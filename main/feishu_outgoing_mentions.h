#pragma once
#include <stddef.h>
#include <stdbool.h>
#define FEISHU_MENTION_MAX 2
#define FEISHU_MENTION_NAME 64
typedef struct {char id[64],name[FEISHU_MENTION_NAME];} feishu_mention_t;
bool feishu_mention_plain_at(const char *text);
bool feishu_mentions_compose(const char *text,const feishu_mention_t *mentions,size_t count,char *out,size_t capacity);
void feishu_mentions_display(const char *text,char *out,size_t capacity);
bool feishu_mentions_split(const char *text,feishu_mention_t mentions[FEISHU_MENTION_MAX],
                          size_t *count,const char **body);

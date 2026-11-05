#pragma once
#include "feishu_model.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Opaque server cursors are bounded independently of the number of pages.
 * Oversized or cyclic replies fail without replacing the displayed page. */
#define FEISHU_PAGE_TOKEN_MAX 512
typedef struct {
    uint32_t version,number,count;
    bool more,previous_known;
    char token[FEISHU_PAGE_TOKEN_MAX+1];
    char next[FEISHU_PAGE_TOKEN_MAX+1];
    char previous[FEISHU_PAGE_TOKEN_MAX+1];
} feishu_page_cursor_t;

/* One compressed, version-checked snapshot; no all-pages RAM collection. */
typedef struct {feishu_page_cursor_t page;feishu_chat_t chats[FEISHU_MAX_CHATS];} feishu_chat_page_t;
typedef struct {feishu_page_cursor_t page;char chat[FEISHU_CHAT_ID_MAX];feishu_message_t messages[FEISHU_MAX_MESSAGES];} feishu_message_page_t;

bool feishu_page_token_valid(const char *token);
bool feishu_page_valid(const feishu_page_cursor_t *page,size_t capacity);
/* The destination is unchanged on failure. */
bool feishu_page_response(feishu_page_cursor_t *page,const char *token,const char *next,
                            bool more,size_t count);
bool feishu_page_url(const char *base,const char *token,char *output,size_t capacity);
void feishu_page_key(const char *kind,const char *scope,const char *token,char output[64]);

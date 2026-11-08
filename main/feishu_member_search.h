#pragma once
#include "feishu_outgoing_mentions.h"
#include "feishu_json.h"
typedef struct {
    feishu_json_t json;
    unsigned char role[FEISHU_JSON_DEPTH+1],key[FEISHU_JSON_DEPTH+1];
    char query[64],id[64],name[256],token[513];
    feishu_mention_t exact,partial;
    unsigned exact_count,partial_count,selected,seen;
    size_t used;
    int code;
    bool code_seen,more,data_seen,items_seen,overflow;
} feishu_member_search_t;
/* Search is accumulated across all user/bot pages; only a complete unique
 * result can resolve a name. Parser storage is constant regardless of group size. */
bool feishu_member_search_init(feishu_member_search_t *search,const char *query);
void feishu_member_search_page(feishu_member_search_t *search);
bool feishu_member_search_feed(feishu_member_search_t *search,const void *bytes,size_t size);
bool feishu_member_search_finish(feishu_member_search_t *search);
bool feishu_member_search_result(const feishu_member_search_t *search,feishu_mention_t *result);

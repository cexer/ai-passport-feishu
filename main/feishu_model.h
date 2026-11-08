#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FEISHU_MAX_CHATS 10
#define FEISHU_MAX_MESSAGES 8
#define FEISHU_CHAT_ID_MAX 64
#define FEISHU_MESSAGE_ID_MAX 64
#define FEISHU_RESOURCE_KEY_MAX 128
#define FEISHU_CHAT_NAME_MAX 64
#define FEISHU_SENDER_NAME_MAX 64
#define FEISHU_TEXT_MAX 256
#define FEISHU_REPLY_MAX 512


static inline size_t feishu_focus_step(size_t selected,size_t count,int direction,bool wrap)
{
    if(!count)return 0;
    if(selected>=count)selected=count-1;
    if(direction<0)return selected ? selected-1:(wrap ? count-1:0);
    if(direction>0)return selected+1<count ? selected+1:(wrap ? 0:selected);
    return selected;
}

typedef enum {
    FEISHU_MESSAGE_TEXT = 0,
    FEISHU_MESSAGE_AUDIO,
    FEISHU_MESSAGE_IMAGE,
    FEISHU_MESSAGE_FILE,
    FEISHU_MESSAGE_OTHER,
} feishu_message_type_t;

typedef struct {
    char chat_id[FEISHU_CHAT_ID_MAX];
    char name[FEISHU_CHAT_NAME_MAX];
    char summary[FEISHU_TEXT_MAX];
    uint64_t latest_time_ms;
    uint32_t unread_count;
    bool p2p;
} feishu_chat_t;

typedef struct {
    char message_id[FEISHU_MESSAGE_ID_MAX];
    char resource_key[FEISHU_RESOURCE_KEY_MAX];
    char sender_id[FEISHU_CHAT_ID_MAX];
    char sender_name[FEISHU_SENDER_NAME_MAX];
    char text[FEISHU_TEXT_MAX];
    uint64_t create_time_ms;
    uint64_t update_time_ms;
    int64_t text_object_id;
    uint64_t text_length;
    feishu_message_type_t type;
    bool mine;
    bool deleted;
    bool text_unavailable;
    bool text_truncated;
    bool text_mapped;
    bool text_needs_mapping;
} feishu_message_t;

/* Display-only merge of an acknowledged local send into a possibly stale
 * latest page. Caller supplies the actual server ID, never a fabricated ID. */
bool feishu_message_merge_ack(feishu_message_t *rows,size_t *count,size_t capacity,
                              const feishu_message_t *ack);

typedef struct {
    char chat_id[FEISHU_CHAT_ID_MAX];
    uint64_t last_seen_time_ms;
} feishu_seen_t;

typedef enum {
    FEISHU_PAGE_CHATS = 0,
    FEISHU_PAGE_MESSAGES,
    FEISHU_PAGE_MESSAGE_DETAIL,
    FEISHU_PAGE_REPLY_READY,
    FEISHU_PAGE_RECORDING,
    FEISHU_PAGE_TRANSCRIBING,
    FEISHU_PAGE_REVIEW,
    FEISHU_PAGE_STATUS,
    FEISHU_PAGE_IMAGE,
    FEISHU_PAGE_SETTINGS,
    FEISHU_PAGE_CONFIRM,
    FEISHU_PAGE_DRAFT_ACTIONS,
    FEISHU_PAGE_IMAGE_ACTIONS,
    FEISHU_PAGE_DRAFTS,
    FEISHU_PAGE_XIAOZHI,
    FEISHU_PAGE_ACCOUNT,
    FEISHU_PAGE_NETWORK,
    FEISHU_PAGE_NETWORK_SCAN,
    FEISHU_PAGE_NETWORK_PASSWORD,
    FEISHU_PAGE_NETWORK_ACTIONS,
    FEISHU_PAGE_ASSISTANT,
    FEISHU_PAGE_XIAOZHI_SETTINGS,
    FEISHU_PAGE_ASSISTANT_SETTINGS,
    FEISHU_PAGE_SOUND_SETTINGS,
    FEISHU_PAGE_ABOUT,
    FEISHU_PAGE_POWER_SETTINGS,
} feishu_page_t;

typedef enum {
    FEISHU_NAV_UP = 0,
    FEISHU_NAV_DOWN,
    FEISHU_NAV_OK,
    FEISHU_NAV_BACK,
    FEISHU_NAV_COMPOSE,
    FEISHU_NAV_BROWSE,
} feishu_nav_event_t;

typedef enum {
    FEISHU_SETTING_ACCOUNT = 0,
    FEISHU_SETTING_XIAOZHI,
    FEISHU_SETTING_ASSISTANT,
    FEISHU_SETTING_NETWORK,
    FEISHU_SETTING_DRAFTS,
    FEISHU_SETTING_SOUNDS,
    FEISHU_SETTING_POWER,
    FEISHU_SETTING_ABOUT,
    FEISHU_SETTING_RETURN,
    FEISHU_SETTING_COUNT,
} feishu_setting_t;

typedef enum {
    FEISHU_NAV_NONE = 0,
    FEISHU_NAV_LOAD_MESSAGES,
    FEISHU_NAV_LOAD_IMAGE,
    FEISHU_NAV_START_RECORDING,
    FEISHU_NAV_STOP_RECORDING,
    FEISHU_NAV_SEND_REPLY,
    FEISHU_NAV_RETRY_RECORDING,
    FEISHU_NAV_CANCEL_REPLY,
    FEISHU_NAV_RESET_WIFI,
    FEISHU_NAV_UNBIND,
    FEISHU_NAV_IMAGE_ZOOM_IN,
    FEISHU_NAV_IMAGE_ZOOM_OUT,
    FEISHU_NAV_IMAGE_AXIS,
    FEISHU_NAV_EXIT,
    FEISHU_NAV_LIST_DRAFTS,
    FEISHU_NAV_OPEN_DRAFT,
    FEISHU_NAV_REAUTHORIZE,
    FEISHU_NAV_CHECK_XIAOZHI,
    FEISHU_NAV_LIST_NETWORK,
    FEISHU_NAV_ENTER_ASSISTANT,
    FEISHU_NAV_CLEAR_DRAFTS,
} feishu_nav_action_t;

typedef struct {
    feishu_page_t page;
    size_t chat_index;
    size_t message_index;
    size_t chat_count;
    size_t message_count;
    bool composer_selected;
    bool reply_to_message;
    size_t settings_index;
    size_t confirm_action;
    size_t draft_index,draft_count;
    bool draft_manager;
} feishu_nav_t;

uint32_t feishu_unread_count(const feishu_message_t *messages, size_t count,
                             uint64_t last_seen_time_ms);
uint64_t feishu_latest_visible_time(const feishu_message_t *messages,
                                    size_t count);
size_t feishu_utf8_copy(char *dst, size_t dst_size, const char *src);
feishu_nav_action_t feishu_nav_handle(feishu_nav_t *nav,
                                      feishu_nav_event_t event);

/* Normalize whitespace and wrap a bounded UTF-8 prefix in display cells. */
size_t feishu_text_preview(char *dst,size_t capacity,const char *src,bool truncated,unsigned cells,unsigned lines);

/* Avatar initials skip punctuation; ASCII pairs or one supported CJK glyph. */
void feishu_avatar_text(char output[5],const char *text,const char *fallback);

/* Fit a contiguous variable-height window; selected==count follows latest. */
size_t feishu_message_window(const uint16_t *heights,size_t count,size_t selected,
    unsigned pixels,size_t max_rows,size_t *start);

/* Append a newly received server preview without moving a reader's selection.
 * Reject duplicates, deleted rows and older server snapshots. */
bool feishu_message_receive(feishu_message_t *rows,size_t *count,size_t capacity,
                            size_t *selection,bool follow_latest,const feishu_message_t *incoming);

/* Reuse only an exact, nonzero server revision within the caller-validated scope. */
bool feishu_message_restore_body(feishu_message_t *preview,const feishu_message_t *cached);
/* 0: ordinary movement; 1: next page; -1: last page reached. */
int feishu_chat_next_page(size_t index,size_t count,bool more);

/* Failed reads stay navigable; never expose rows from another conversation. */
void feishu_nav_message_load_failed(feishu_nav_t *nav,bool same_chat);

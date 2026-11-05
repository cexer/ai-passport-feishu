// Direct-to-Feishu messenger demo. Network and ASR work never runs in the
// button/LVGL task; the physical OK key is the final send confirmation.
#include "demo.h"
#include "bsp_battery.h"
#include "feishu_api.h"
#include "feishu_asr.h"
#include "feishu_network.h"
#include "feishu_store.h"
#include "feishu_cache.h"
#include "feishu_text_view.h"
#include "feishu_image_fs.h"
#include "feishu_http.h"
#include "ui_pixel.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

static const char *TAG = "demo_feishu";
#ifdef FEISHU_BUSINESS_AUTOTEST
void feishu_business_autotest(feishu_api_session_t *session);
#endif
#ifdef FEISHU_DRAFT_AUTOTEST
bool feishu_draft_autotest_resume(void);
void feishu_draft_autotest_saved(int slot,const feishu_outbox_t *pending,const char *text,bool ui_pass);
#endif
#define RAM_REPLY_SLOT (-2)
#ifdef FEISHU_VOICE_AUTOTEST
static bool s_voice_ui_probe;
static volatile bool s_voice_force_offline;
#endif
static void handle_navigation(feishu_nav_event_t event);

typedef enum {
    WORK_INIT = 0,
    WORK_LOAD_MESSAGES,
    WORK_REFRESH_MESSAGES,
    WORK_LOAD_TEXT_PAGE,
    WORK_FETCH_TEXT,
    WORK_LOAD_DRAFT,
    WORK_LOAD_DRAFT_PAGE,
    WORK_LIST_DRAFTS,
    WORK_OPEN_DRAFT,
    WORK_CHAT_PAGE,
    WORK_FETCH_CHAT_PAGE,
    WORK_MESSAGE_PAGE,
    WORK_FETCH_MESSAGE_PAGE,
    WORK_DISCARD_DRAFT,
    WORK_LOAD_IMAGE,
    WORK_FETCH_IMAGE,
    WORK_RECORD,
    WORK_SEND,
    WORK_RESET_WIFI,
    WORK_UNBIND,
    WORK_REAUTHORIZE,
    WORK_EXIT,
} work_command_t;
static void enqueue(work_command_t command);
typedef feishu_message_page_t message_page_frame_t;

typedef struct {
    feishu_nav_t nav;
    feishu_chat_t chats[FEISHU_MAX_CHATS];
    feishu_message_t messages[FEISHU_MAX_MESSAGES];
    char reply[FEISHU_REPLY_MAX];
    char reply_uuid[41];
    char status[96];
    char loaded_chat[FEISHU_CHAT_ID_MAX],last_messages_chat[FEISHU_CHAT_ID_MAX];
    TickType_t last_messages_tick;
    bool loading;
    bool status_error;
    bool ready;
    bool closing;
    volatile bool cancel_recording;
    volatile bool record_pending;
    bool record_replaces_pending,pending_retry_warning;
    bool cancel_send,sending;
    int pending_slot;
    feishu_outbox_t pending;
    uint8_t draft_slots[FEISHU_OUTBOX_SLOTS];
    char draft_labels[FEISHU_OUTBOX_SLOTS][96];
    char chat_page_key[64];
    uint32_t chat_page_number,chat_page_request;
    int chat_page_direction;
    bool chat_page_more;
    char message_page_key[64];
    uint32_t message_page_number,message_page_request;
    int message_page_direction;
    bool message_page_more,message_page_loading;
    uint32_t recovery_generation;
    bool recovering_operation;
    bool image_ready;
    unsigned image_version;
    bool image_busy,image_horizontal,image_cache_queued,image_fetch_queued;
    uint32_t image_request;
    feishu_image_view_t image_view;
    feishu_prepared_image_t image;
    char detail_text[FEISHU_TEXT_PAGE_CAP];
    uint32_t detail_page,detail_pages;
    int32_t detail_scroll,detail_scroll_max;
    bool detail_loaded,detail_restore,detail_refetch_attempted;
    char bookmark_id[FEISHU_MESSAGE_ID_MAX];
    uint32_t bookmark_page,bookmark_epoch,bookmark_scope;
    int32_t bookmark_scroll;
    bool bookmark_dirty;
    volatile bool stop_recording;
    volatile unsigned elapsed_ms;
    int battery_percent;
} feishu_ui_state_t;

static lv_obj_t *s_scr;
static uint32_t s_screen_generation;
static lv_obj_t *s_title;
static lv_obj_t *s_page_info;
static lv_obj_t *s_content;
static lv_obj_t *s_viewport;
static lv_obj_t *s_image;
static lv_obj_t *s_hint;
static lv_obj_t *s_notice;
static lv_obj_t *s_header_rule;
#define FEISHU_VISIBLE_ROWS 4
#define FEISHU_CONTENT_HEIGHT 226
static lv_obj_t *s_rows[FEISHU_VISIBLE_ROWS];
static lv_obj_t *s_row_avatar[FEISHU_VISIBLE_ROWS];
static lv_obj_t *s_row_avatar_text[FEISHU_VISIBLE_ROWS];
static lv_obj_t *s_row_text[FEISHU_VISIBLE_ROWS];
static lv_obj_t *s_row_subtext[FEISHU_VISIBLE_ROWS];
static lv_obj_t *s_row_mark[FEISHU_VISIBLE_ROWS];
static lv_timer_t *s_timer;
static QueueHandle_t s_work_queue;
static SemaphoreHandle_t s_state_lock;
static SemaphoreHandle_t s_worker_done;
static TaskHandle_t s_worker;
static TaskHandle_t s_cache_worker;
static QueueHandle_t s_cache_queue;
static SemaphoreHandle_t s_cache_done;
static feishu_ui_state_t s_state;
static feishu_api_session_t s_session;
static char s_cache_app[FEISHU_APP_ID_MAX];
static char s_cache_owner[FEISHU_CHAT_ID_MAX];
static int64_t s_chat_pin, s_message_pin,s_text_pin;
static int64_t s_image_pin;
static atomic_bool s_authenticated;
static unsigned s_displayed_image_version;

/* The timer also runs while a scanline image is visible. Re-setting unchanged
 * labels needlessly invalidates display areas and repeats flash-backed drawing. */
static void set_label_text(lv_obj_t *label,const char *text)
{
    if(strcmp(lv_label_get_text(label),text))lv_label_set_text(label,text);
}

static uint32_t s_owner_epoch;
static bool online(void) {
#ifdef FEISHU_VOICE_AUTOTEST
    if(s_voice_force_offline)return false;
#endif
    return atomic_load(&s_authenticated) && feishu_network_connected();
}

#define FEISHU_MESSAGE_ROWS 3
#define FEISHU_REFRESH_BATCH 1

static void make_reply_uuid(char output[41])
{
    snprintf(output, 41, "%08lx-%08lx-%08lx-%08lx",
             (unsigned long)esp_random(), (unsigned long)esp_random(),
             (unsigned long)esp_random(), (unsigned long)esp_random());
}

static void set_status(const char *text, bool loading)
{
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    feishu_utf8_copy(s_state.status, sizeof(s_state.status), text);
    s_state.loading = loading;
    s_state.status_error=!loading;
    xSemaphoreGive(s_state_lock);
}

static void set_connection_status(const char *text)
{
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    if(s_state.nav.page==FEISHU_PAGE_CHATS && !s_state.loading)
        feishu_utf8_copy(s_state.status,sizeof(s_state.status),text);
    xSemaphoreGive(s_state_lock);
}

static uint64_t seen_time(const feishu_seen_t *seen, size_t count,
                          const char *chat_id)
{
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(seen[i].chat_id, chat_id) == 0) {
            return seen[i].last_seen_time_ms;
        }
    }
    return 0;
}

static esp_err_t load_messages_for_chat(size_t chat_index, bool mark_seen,bool prefer_cache)
{
    message_page_frame_t *frame=calloc(1,sizeof(*frame));
    feishu_message_t *messages=frame ? frame->messages:NULL;
    char chat_id[FEISHU_CHAT_ID_MAX];
    size_t count = 0;
    char selected_id[FEISHU_MESSAGE_ID_MAX] = { 0 };
    bool followed_latest = true;
    bool ram_hit=false;
    uint32_t epoch,request,scope=feishu_cache_scope_version();
    bool framed=false;
    esp_err_t err;

    if (messages == NULL) return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    if (chat_index >= s_state.nav.chat_count) {
        xSemaphoreGive(s_state_lock);
        free(frame);
        return ESP_ERR_INVALID_ARG;
    }
    feishu_utf8_copy(chat_id, sizeof(chat_id), s_state.chats[chat_index].chat_id);
    request=s_state.message_page_request;
    if(!prefer_cache && !strcmp(s_state.loaded_chat,chat_id) && (s_state.message_page_number>0 || s_state.message_page_loading)) {
        xSemaphoreGive(s_state_lock);free(frame);return ESP_OK;
    }
    if(prefer_cache && chat_id[0] && !strcmp(s_state.loaded_chat,chat_id)) {
        memcpy(messages,s_state.messages,sizeof(s_state.messages));count=s_state.nav.message_count;ram_hit=true;
    }
    epoch=s_owner_epoch;
    if (s_state.nav.message_count > 0 &&
        s_state.nav.message_index < s_state.nav.message_count) {
        followed_latest = s_state.nav.composer_selected || s_state.nav.message_index + 1 ==
                          s_state.nav.message_count;
        feishu_utf8_copy(selected_id, sizeof(selected_id),
                         s_state.messages[s_state.nav.message_index].message_id);
    }
    xSemaphoreGive(s_state_lock);

    char cache_key[FEISHU_CHAT_ID_MAX+16];
    snprintf(cache_key,sizeof(cache_key),"messages/%s",chat_id);
    char page_key[64];feishu_page_key("messages",chat_id,"",page_key);
    bool cached=ram_hit;
    if(prefer_cache && !ram_hit) {
        size_t rows=0;
        framed=feishu_cache_read(page_key,frame,sizeof(*frame),1,&rows,&s_message_pin)==ESP_OK && rows==1 &&
            feishu_page_valid(&frame->page,FEISHU_MAX_MESSAGES) && !strcmp(frame->chat,chat_id) && !frame->page.token[0];
        if(framed) {count=frame->page.count;cached=true;}
        else cached=feishu_cache_read(cache_key,messages,sizeof(*messages),FEISHU_MAX_MESSAGES,&count,&s_message_pin)==ESP_OK;
    }
    err=cached ? ESP_OK:prefer_cache ? ESP_ERR_NOT_FOUND:!online() ? ESP_ERR_INVALID_STATE:
        feishu_api_message_page(&s_session,chat_id,"",messages,FEISHU_MAX_MESSAGES,&frame->page);
    bool saved=false;
    if(err==ESP_OK && !cached) {
        count=frame->page.count;feishu_utf8_copy(frame->chat,sizeof(frame->chat),chat_id);
        saved=feishu_cache_write_if_scope(scope,page_key,frame,sizeof(*frame),1)==ESP_OK;framed=saved;
    }
    if(cached) ESP_LOGI(TAG,"cache hit: message snapshot");
    if (err != ESP_OK) {
        free(frame);
        return err;
    }

    uint64_t latest = feishu_latest_visible_time(messages, count);
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    bool applied=epoch==s_owner_epoch && request==s_state.message_page_request && chat_index<s_state.nav.chat_count &&
        s_state.nav.chat_index==chat_index && !strcmp(s_state.chats[chat_index].chat_id,chat_id) &&
        s_state.nav.page==FEISHU_PAGE_MESSAGES;
    if(applied) {
    memcpy(s_state.messages, messages, sizeof(s_state.messages));
    feishu_utf8_copy(s_state.loaded_chat,sizeof(s_state.loaded_chat),chat_id);
    s_state.nav.message_count = count;
    if(!ram_hit) {
        s_state.message_page_key[0]=0;s_state.message_page_number=0;s_state.message_page_more=false;
        if(framed) {
            feishu_utf8_copy(s_state.message_page_key,sizeof(s_state.message_page_key),page_key);
            s_state.message_page_number=frame->page.number;s_state.message_page_more=frame->page.more;
        }
    }
    s_state.nav.message_index = count == 0 ? 0 : count - 1;
    if (!followed_latest && selected_id[0] != '\0') {
        for (size_t i = 0; i < count; ++i) {
            if (strcmp(messages[i].message_id, selected_id) == 0) {
                s_state.nav.message_index = i;
                break;
            }
        }
    }
    s_state.chats[chat_index].latest_time_ms = latest;
    if (count > 0) {
        feishu_utf8_copy(s_state.chats[chat_index].summary,
                         sizeof(s_state.chats[chat_index].summary),
                         messages[count - 1].text);
    }
    if (mark_seen) s_state.chats[chat_index].unread_count = 0;
    }
    xSemaphoreGive(s_state_lock);
    if (applied && mark_seen && latest > 0) feishu_store_mark_seen(chat_id, latest);
    if(!cached) {
        if(applied && saved)feishu_cache_protect(page_key,&s_message_pin);
        if(saved) {
            xSemaphoreTake(s_state_lock,portMAX_DELAY);
            feishu_utf8_copy(s_state.last_messages_chat,sizeof(s_state.last_messages_chat),chat_id);
            s_state.last_messages_tick=xTaskGetTickCount();
            xSemaphoreGive(s_state_lock);
        }
    }
    free(frame);
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    bool recent=!strcmp(s_state.last_messages_chat,chat_id) &&
        xTaskGetTickCount()-s_state.last_messages_tick<pdMS_TO_TICKS(2000);
    xSemaphoreGive(s_state_lock);
    if(cached && applied && online() && !recent) {
        work_command_t refresh=WORK_REFRESH_MESSAGES;
        xQueueSend(s_work_queue,&refresh,0);
    }
    return cached && !applied ? ESP_ERR_INVALID_STATE : ESP_OK;
}

typedef feishu_chat_page_t chat_page_frame_t;
static bool apply_conversations(const feishu_chat_t *chats,size_t count,
                                const feishu_page_cursor_t *page,uint32_t request,uint32_t epoch)
{
    char selected[FEISHU_CHAT_ID_MAX]={0};
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    if(s_state.nav.page!=FEISHU_PAGE_CHATS || request!=s_state.chat_page_request || epoch!=s_owner_epoch) {
        xSemaphoreGive(s_state_lock);
        return false;
    }
    if(s_state.nav.chat_index<s_state.nav.chat_count)
        feishu_utf8_copy(selected,sizeof(selected),s_state.chats[s_state.nav.chat_index].chat_id);
    memcpy(s_state.chats,chats,sizeof(s_state.chats));
    s_state.nav.chat_count=count;
    s_state.nav.chat_index=0;
    for(size_t i=0;i<count;++i) if(!strcmp(chats[i].chat_id,selected)) s_state.nav.chat_index=i;
    if(page) {
        feishu_page_key("chats","",page->token,s_state.chat_page_key);
        s_state.chat_page_number=page->number;s_state.chat_page_more=page->more;
    }
    s_state.ready=true;
    s_state.loading=false;s_state.status[0]=0;
    xSemaphoreGive(s_state_lock);
    return true;
}

static esp_err_t refresh_conversations(void)
{
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    bool visible=s_state.nav.page==FEISHU_PAGE_CHATS && s_state.chat_page_number==0 && !s_state.loading;
    uint32_t page_request=s_state.chat_page_request,page_epoch=s_owner_epoch;
    xSemaphoreGive(s_state_lock);
    if(!visible)return ESP_OK;
    static size_t refresh_cursor;
    chat_page_frame_t *frame=calloc(1,sizeof(*frame));
    feishu_chat_t *chats=frame ? frame->chats:NULL;
    feishu_seen_t *seen = NULL;
    message_page_frame_t *message_frame=NULL;
    feishu_message_t *messages=NULL;
    size_t chat_count = 0;
    size_t seen_count = 0;
    esp_err_t err;

    if (chats == NULL) {
        err = ESP_ERR_NO_MEM;
        goto done;
    }
    err=feishu_api_chat_page(&s_session,"",chats,FEISHU_MAX_CHATS,&frame->page);
    chat_count=frame->page.count;
    if (err != ESP_OK) goto done;
    // Do not retain message-enrichment buffers during the chat TLS handshake.
    seen=calloc(FEISHU_MAX_CHATS,sizeof(*seen));
    if(!seen) {err=ESP_ERR_NO_MEM;goto done;}
    feishu_store_load_seen(seen, FEISHU_MAX_CHATS, &seen_count);

    // Preserve metadata for chats outside this small refresh batch. Fetching
    // ten message pages back-to-back fragments the ESP32-C3 heap enough to
    // starve a later TLS record allocation.
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    for (size_t i = 0; i < chat_count; ++i) {
        for (size_t old = 0; old < s_state.nav.chat_count; ++old) {
            if (strcmp(chats[i].chat_id, s_state.chats[old].chat_id) == 0) {
                chats[i].latest_time_ms = s_state.chats[old].latest_time_ms;
                chats[i].unread_count = s_state.chats[old].unread_count;
                feishu_utf8_copy(chats[i].summary, sizeof(chats[i].summary),
                                 s_state.chats[old].summary);
                break;
            }
        }
    }
    xSemaphoreGive(s_state_lock);

    uint64_t baselines[FEISHU_MAX_CHATS]={0};
    for(size_t i=0;i<chat_count;++i)baselines[i]=seen_time(seen,seen_count,chats[i].chat_id);
    memset(seen,0,FEISHU_MAX_CHATS*sizeof(*seen));free(seen);seen=NULL;
    bool applied=apply_conversations(chats,chat_count,&frame->page,page_request,page_epoch);
    char page_key[64];feishu_page_key("chats","","",page_key);
    if(feishu_cache_write(page_key,frame,sizeof(*frame),1)==ESP_OK && applied)
        feishu_cache_protect(page_key,&s_chat_pin);
    // Publish the list before enrichment; never retain both full page frames
    // during TLS. Summaries are persisted on the following list refresh.
    free(frame);frame=NULL;chats=NULL;
    if(!applied) {err=ESP_OK;goto done;}
    message_frame=calloc(1,sizeof(*message_frame));
    messages=message_frame ? message_frame->messages:NULL;
    if(!messages) {err=ESP_ERR_NO_MEM;goto done;}

    size_t batch = chat_count < FEISHU_REFRESH_BATCH ?
                   chat_count : FEISHU_REFRESH_BATCH;
    for (size_t offset = 0; offset < batch; ++offset) {
        size_t i = chat_count == 0 ? 0 : (refresh_cursor + offset) % chat_count;
        feishu_chat_t chat;
        xSemaphoreTake(s_state_lock,portMAX_DELAY);
        bool current=page_epoch==s_owner_epoch && page_request==s_state.chat_page_request &&
            s_state.nav.page==FEISHU_PAGE_CHATS && i<s_state.nav.chat_count;
        if(current)chat=s_state.chats[i];
        xSemaphoreGive(s_state_lock);
        if(!current)break;
        size_t message_count = 0;
        memset(message_frame,0,sizeof(*message_frame));
        err=feishu_api_message_page(&s_session,chat.chat_id,"",messages,FEISHU_MAX_MESSAGES,&message_frame->page);
        message_count=message_frame->page.count;
        if (err == FEISHU_ERR_AUTH_REVOKED) goto done;
        if (err != ESP_OK) continue;
        chat.latest_time_ms = feishu_latest_visible_time(messages, message_count);
        uint64_t baseline = baselines[i];
        if (baseline == 0 && chat.latest_time_ms > 0) {
            // The first successful sync establishes a baseline. Historical
            // messages must not all appear as new on a newly bound device.
            feishu_store_mark_seen(chat.chat_id, chat.latest_time_ms);
            chat.unread_count = 0;
        } else {
            chat.unread_count = feishu_unread_count(
                messages, message_count, baseline);
        }
        if (message_count > 0) {
            feishu_utf8_copy(chat.summary, sizeof(chat.summary),
                             messages[message_count - 1].text);
        }
        xSemaphoreTake(s_state_lock,portMAX_DELAY);
        current=page_epoch==s_owner_epoch && page_request==s_state.chat_page_request &&
            i<s_state.nav.chat_count && !strcmp(s_state.chats[i].chat_id,chat.chat_id);
        if(current)s_state.chats[i]=chat;
        xSemaphoreGive(s_state_lock);
        if(!current)break;
        char cache_key[64];feishu_page_key("messages",chat.chat_id,"",cache_key);
        feishu_utf8_copy(message_frame->chat,sizeof(message_frame->chat),chat.chat_id);
        if(feishu_cache_write(cache_key,message_frame,sizeof(*message_frame),1)==ESP_OK) {
            xSemaphoreTake(s_state_lock,portMAX_DELAY);
            feishu_utf8_copy(s_state.last_messages_chat,sizeof(s_state.last_messages_chat),chat.chat_id);
            s_state.last_messages_tick=xTaskGetTickCount();
            xSemaphoreGive(s_state_lock);
        }
    }
    if (chat_count > 0) refresh_cursor = (refresh_cursor + batch) % chat_count;
    err = ESP_OK;

done:
    if (seen != NULL) memset(seen, 0, FEISHU_MAX_CHATS * sizeof(*seen));
    free(message_frame);
    free(seen);
    free(frame);
    return err;
}

/* Flash holds cursor history with each disposable page. Only one bounded
 * row window and a small current-page key remain in UI RAM. */
static esp_err_t load_chat_page(bool allow_network)
{
    char current_key[64];int direction;uint32_t request,epoch,scope=feishu_cache_scope_version();
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    if(s_state.nav.page!=FEISHU_PAGE_CHATS || !s_state.loading) {xSemaphoreGive(s_state_lock);return ESP_ERR_INVALID_STATE;}
    memcpy(current_key,s_state.chat_page_key,sizeof(current_key));direction=s_state.chat_page_direction;
    request=s_state.chat_page_request;epoch=s_owner_epoch;xSemaphoreGive(s_state_lock);
    chat_page_frame_t *frame=calloc(1,sizeof(*frame));
    feishu_page_cursor_t *destination=calloc(1,sizeof(*destination));
    esp_err_t err=frame && destination ? ESP_OK:ESP_ERR_NO_MEM;
    size_t count=0;int64_t pin=0;
    if(err==ESP_OK && current_key[0]) {
        err=feishu_cache_read(current_key,frame,sizeof(*frame),1,&count,&pin);
        feishu_cache_unpin(&pin);
        if(err==ESP_OK && (count!=1 || !feishu_page_valid(&frame->page,FEISHU_MAX_CHATS)))err=ESP_ERR_INVALID_RESPONSE;
    }
    if(err==ESP_OK) {
        if(direction>0 && !frame->page.more)err=ESP_ERR_NOT_FOUND;
        else {
            const char *token=direction>0 ? frame->page.next:frame->page.previous_known ? frame->page.previous:"";
            strcpy(destination->token,token);
            destination->number=direction>0 ? frame->page.number+1:frame->page.previous_known ? frame->page.number-1:0;
            if(direction>0) {destination->previous_known=true;strcpy(destination->previous,frame->page.token);}
        }
    }
    char key[64]={0};bool hit=false;
    if(err==ESP_OK) {
        feishu_page_key("chats","",destination->token,key);
        err=feishu_cache_read(key,frame,sizeof(*frame),1,&count,&pin);
        hit=err==ESP_OK && count==1 && feishu_page_valid(&frame->page,FEISHU_MAX_CHATS) && !strcmp(frame->page.token,destination->token);
        if(!hit)err=ESP_ERR_NOT_FOUND;
        feishu_cache_unpin(&pin);
        if(!hit && !allow_network && online()) {
            free(destination);free(frame);enqueue(WORK_FETCH_CHAT_PAGE);return ESP_OK;
        }
        if(!hit && allow_network && online()) {
            memset(frame,0,sizeof(*frame));
            err=feishu_api_chat_page(&s_session,destination->token,frame->chats,FEISHU_MAX_CHATS,&frame->page);
            if(err==ESP_OK) {
                frame->page.number=destination->number;frame->page.previous_known=destination->previous_known;
                strcpy(frame->page.previous,destination->previous);
                err=feishu_cache_write_if_scope(scope,key,frame,sizeof(*frame),1);
            }
        }
    }
    bool applied=false;
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    bool active=epoch==s_owner_epoch && request==s_state.chat_page_request && s_state.nav.page==FEISHU_PAGE_CHATS;
    xSemaphoreGive(s_state_lock);
    if(active && err==ESP_OK)applied=apply_conversations(frame->chats,frame->page.count,&frame->page,request,epoch);
    if(applied)feishu_cache_protect(key,&s_chat_pin);
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    if(active && request==s_state.chat_page_request && s_state.nav.page==FEISHU_PAGE_CHATS) {
        s_state.loading=false;
        feishu_utf8_copy(s_state.status,sizeof(s_state.status),applied ? (hit ? "缓存列表":""):
            "此页暂不可读，原列表保留，可返回或重试");
    }
    xSemaphoreGive(s_state_lock);
    free(destination);free(frame);return err;
}
static esp_err_t load_message_page(bool allow_network)
{
    char current_key[64],chat[FEISHU_CHAT_ID_MAX];int direction;size_t chat_index;
    uint32_t request,epoch,scope=feishu_cache_scope_version();
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    chat_index=s_state.nav.chat_index;
    if(s_state.nav.page!=FEISHU_PAGE_MESSAGES || !s_state.loading || !s_state.message_page_loading || chat_index>=s_state.nav.chat_count) {
        xSemaphoreGive(s_state_lock);return ESP_ERR_INVALID_STATE;
    }
    feishu_utf8_copy(chat,sizeof(chat),s_state.chats[chat_index].chat_id);
    memcpy(current_key,s_state.message_page_key,sizeof(current_key));direction=s_state.message_page_direction;
    request=s_state.message_page_request;epoch=s_owner_epoch;xSemaphoreGive(s_state_lock);
    message_page_frame_t *frame=calloc(1,sizeof(*frame));
    feishu_page_cursor_t *destination=calloc(1,sizeof(*destination));
    esp_err_t err=frame && destination ? ESP_OK:ESP_ERR_NO_MEM;
    size_t count=0;int64_t pin=0;
    if(err==ESP_OK) {
        err=feishu_cache_read(current_key,frame,sizeof(*frame),1,&count,&pin);
        feishu_cache_unpin(&pin);
        if(err==ESP_OK && (count!=1 || !feishu_page_valid(&frame->page,FEISHU_MAX_MESSAGES) ||
           !memchr(frame->chat,0,sizeof(frame->chat)) || strcmp(frame->chat,chat)))err=ESP_ERR_INVALID_RESPONSE;
    }
    if(err==ESP_OK) {
        if(direction>0 && !frame->page.more)err=ESP_ERR_NOT_FOUND;
        else {
            const char *token=direction>0 ? frame->page.next:frame->page.previous_known ? frame->page.previous:"";
            strcpy(destination->token,token);
            destination->number=direction>0 ? frame->page.number+1:frame->page.previous_known ? frame->page.number-1:0;
            if(direction>0) {destination->previous_known=true;strcpy(destination->previous,frame->page.token);}
        }
    }
    char key[64]={0};bool hit=false;
    if(err==ESP_OK) {
        feishu_page_key("messages",chat,destination->token,key);
        err=feishu_cache_read(key,frame,sizeof(*frame),1,&count,&pin);
        hit=err==ESP_OK && count==1 && feishu_page_valid(&frame->page,FEISHU_MAX_MESSAGES) &&
            !strcmp(frame->page.token,destination->token) && memchr(frame->chat,0,sizeof(frame->chat)) && !strcmp(frame->chat,chat);
        if(!hit)err=ESP_ERR_NOT_FOUND;
        feishu_cache_unpin(&pin);
        if(!hit && !allow_network && online()) {free(destination);free(frame);enqueue(WORK_FETCH_MESSAGE_PAGE);return ESP_OK;}
        if(!hit && allow_network && online()) {
            memset(frame,0,sizeof(*frame));
            err=feishu_api_message_page(&s_session,chat,destination->token,frame->messages,FEISHU_MAX_MESSAGES,&frame->page);
            if(err==ESP_OK) {
                frame->page.number=destination->number;frame->page.previous_known=destination->previous_known;
                strcpy(frame->page.previous,destination->previous);feishu_utf8_copy(frame->chat,sizeof(frame->chat),chat);
                err=feishu_cache_write_if_scope(scope,key,frame,sizeof(*frame),1);
            }
        }
    }
    bool applied=false;
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    bool active=epoch==s_owner_epoch && request==s_state.message_page_request && s_state.nav.page==FEISHU_PAGE_MESSAGES &&
        chat_index==s_state.nav.chat_index && chat_index<s_state.nav.chat_count && !strcmp(chat,s_state.chats[chat_index].chat_id);
    if(active) {
        s_state.loading=s_state.message_page_loading=false;
        if(err==ESP_OK) {
            memcpy(s_state.messages,frame->messages,sizeof(s_state.messages));s_state.nav.message_count=frame->page.count;
            s_state.nav.message_index=direction>0 && frame->page.count ? frame->page.count-1:0;s_state.nav.composer_selected=false;
            feishu_utf8_copy(s_state.loaded_chat,sizeof(s_state.loaded_chat),chat);
            feishu_utf8_copy(s_state.message_page_key,sizeof(s_state.message_page_key),key);
            s_state.message_page_number=frame->page.number;s_state.message_page_more=frame->page.more;applied=true;
        }
        feishu_utf8_copy(s_state.status,sizeof(s_state.status),applied ? (hit ? "缓存历史":""):
            "历史暂不可读，原消息保留，可返回或重试");
    }
    xSemaphoreGive(s_state_lock);
    if(applied)feishu_cache_protect(key,&s_message_pin);
    free(destination);free(frame);return err;
}
static esp_err_t load_conversation_list(void)
{
    chat_page_frame_t *frame=calloc(1,sizeof(*frame));
    feishu_chat_t *chats=frame ? frame->chats:NULL;
    size_t count = 0;
    if (chats == NULL) return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_state_lock,portMAX_DELAY);uint32_t request=s_state.chat_page_request,epoch=s_owner_epoch;xSemaphoreGive(s_state_lock);
    esp_err_t err=feishu_api_chat_page(&s_session,"",chats,FEISHU_MAX_CHATS,&frame->page);
    count=frame->page.count;
    if (err != ESP_OK) {
        free(frame);
        return err;
    }
    bool applied=apply_conversations(chats,count,&frame->page,request,epoch);
    char page_key[64];feishu_page_key("chats","","",page_key);
    if(feishu_cache_write(page_key,frame,sizeof(*frame),1)==ESP_OK && applied)feishu_cache_protect(page_key,&s_chat_pin);
    free(frame);
    return ESP_OK;
}

static esp_err_t initialize_feishu(void)
{
    esp_err_t err;
    bool cached=false;
    feishu_credentials_t *credentials = calloc(1, sizeof(*credentials));
    if (credentials == NULL) return ESP_ERR_NO_MEM;
    err = feishu_store_load_credentials(credentials);
    if(err!=ESP_OK) goto done;
    feishu_utf8_copy(s_cache_app,sizeof(s_cache_app),credentials->app_id);
    feishu_store_load_cache_owner(s_cache_owner,sizeof(s_cache_owner));
    feishu_cache_set_scope(s_cache_app,s_cache_owner);
    if(s_cache_owner[0]) {
        int64_t begin=esp_timer_get_time();unsigned unreadable=0;
        for(unsigned i=0;i<FEISHU_OUTBOX_SLOTS;++i) {
            feishu_outbox_t pending;esp_err_t read=feishu_cache_pending_read(i,&pending);
            if(read!=ESP_OK && read!=ESP_ERR_NOT_FOUND)++unreadable;
        }
        ESP_LOGI(TAG,"draft index empty=%u unreadable=%u elapsed_ms=%u",(unsigned)feishu_cache_pending_empty(),unreadable,(unsigned)((esp_timer_get_time()-begin)/1000));
    }
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    s_state.ready=true; /* Bound users retain Settings even without a cache. */
    bool need_cache=s_state.nav.page==FEISHU_PAGE_CHATS && !s_state.nav.chat_count && !s_state.chat_page_key[0];
    cached=s_state.nav.chat_count>0 || s_state.chat_page_key[0];
    if(need_cache)s_state.loading=false;
    xSemaphoreGive(s_state_lock);
    chat_page_frame_t *frame=need_cache ? calloc(1,sizeof(*frame)):NULL;
    if(frame) {
        size_t count=0;char page_key[64];feishu_page_key("chats","","",page_key);
        esp_err_t cached_read=feishu_cache_read(page_key,frame,sizeof(*frame),1,&count,&s_chat_pin);
        bool framed=cached_read==ESP_OK && count==1 && feishu_page_valid(&frame->page,FEISHU_MAX_CHATS) && !frame->page.token[0];
        if(framed)count=frame->page.count;
        else cached_read=feishu_cache_read("chats",frame->chats,sizeof(frame->chats[0]),FEISHU_MAX_CHATS,&count,&s_chat_pin);
        ESP_LOGI(TAG,"startup conversation cache rc=%d count=%u",(int)cached_read,(unsigned)count);
        if(cached_read==ESP_OK) {
            xSemaphoreTake(s_state_lock,portMAX_DELAY);
            if(!s_state.nav.chat_count && !s_state.chat_page_key[0] && s_state.nav.page==FEISHU_PAGE_CHATS) {
                memcpy(s_state.chats,frame->chats,sizeof(s_state.chats));
                s_state.nav.chat_count=count;s_state.nav.chat_index=0;
                if(framed) {
                    feishu_utf8_copy(s_state.chat_page_key,sizeof(s_state.chat_page_key),page_key);
                    s_state.chat_page_number=0;s_state.chat_page_more=frame->page.more;
                }
            }
            xSemaphoreGive(s_state_lock);cached=true;
        }
        free(frame);
    }
    s_authenticated=false;
    set_connection_status(cached ? "缓存已就绪，正在连接..." : "正在连接网络...");
    err=feishu_network_start(20000);
    if(err!=ESP_OK) goto done;
    set_connection_status("正在连接飞书...");
    err=feishu_api_authenticate(&s_session,credentials);
    if(err!=ESP_OK) goto done;
    s_authenticated=true;
    if(strcmp(s_cache_owner,s_session.self_open_id)) {
        xSemaphoreTake(s_state_lock,portMAX_DELAY);
        ++s_owner_epoch;++s_state.chat_page_request;
        s_state.chat_page_key[0]=0;s_state.chat_page_number=0;s_state.chat_page_more=false;
        ++s_state.message_page_request;s_state.message_page_key[0]=0;s_state.message_page_number=0;
        s_state.message_page_more=s_state.message_page_loading=false;
        s_state.nav.draft_manager=false;s_state.nav.draft_index=s_state.nav.draft_count=0;
        s_state.pending_slot=-1;memset(&s_state.pending,0,sizeof(s_state.pending));
        s_state.reply[0]=s_state.detail_text[0]=s_state.reply_uuid[0]=0;
        s_state.loaded_chat[0]=s_state.last_messages_chat[0]=0;
        if(s_cache_owner[0]) {
            memset(s_state.chats,0,sizeof(s_state.chats));
            memset(s_state.messages,0,sizeof(s_state.messages));
            s_state.nav.chat_count=s_state.nav.message_count=0;
            s_state.nav.chat_index=s_state.nav.message_index=0;
            s_state.nav.page=FEISHU_PAGE_CHATS;s_state.loading=false;
        }
        xSemaphoreGive(s_state_lock);
    }
    feishu_utf8_copy(s_cache_owner,sizeof(s_cache_owner),s_session.self_open_id);
    feishu_cache_set_scope(s_cache_app,s_cache_owner);
    if(s_cache_owner[0]) {
        int64_t begin=esp_timer_get_time();unsigned unreadable=0;
        for(unsigned i=0;i<FEISHU_OUTBOX_SLOTS;++i) {
            feishu_outbox_t pending;esp_err_t read=feishu_cache_pending_read(i,&pending);
            if(read!=ESP_OK && read!=ESP_ERR_NOT_FOUND)++unreadable;
        }
        ESP_LOGI(TAG,"draft index empty=%u unreadable=%u elapsed_ms=%u",(unsigned)feishu_cache_pending_empty(),unreadable,(unsigned)((esp_timer_get_time()-begin)/1000));
    }
    feishu_store_save_cache_owner(s_cache_owner);
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    bool keep_page=s_state.nav.page==FEISHU_PAGE_CHATS && s_state.chat_page_number>0;
    xSemaphoreGive(s_state_lock);
    if(!keep_page) {set_connection_status("正在加载会话...");err=load_conversation_list();}
done:
    memset(credentials, 0, sizeof(*credentials));
    free(credentials);
    return err;
}

static void recover_revoked_auth(esp_err_t err)
{
    if(err==FEISHU_ERR_APP_REJECTED) {
        set_status("应用凭据失效，请在设置重新授权",false);return;
    }
    if (err != FEISHU_ERR_AUTH_REVOKED) return;
    // Keep the owner app and Wi-Fi; onboarding can request a new QR.
    esp_err_t clear_err = feishu_store_request_auth(FEISHU_AUTH_REAUTHORIZE);
    if(clear_err==ESP_OK)clear_err=feishu_store_clear_user_tokens();
    if (clear_err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    }
    ESP_LOGW(TAG, "could not clear revoked tokens: %s",
             esp_err_to_name(clear_err));
}

typedef struct {uint64_t offset;int32_t scroll;uint32_t version;} read_position_t;

static void bookmark_locked(void)
{
    if(s_state.nav.message_index>=s_state.nav.message_count)return;
    feishu_utf8_copy(s_state.bookmark_id,sizeof(s_state.bookmark_id),
        s_state.messages[s_state.nav.message_index].message_id);
    s_state.bookmark_page=s_state.detail_page;
    s_state.bookmark_scroll=s_state.detail_scroll;
    s_state.bookmark_epoch=s_owner_epoch;
    s_state.bookmark_dirty=true;
}

static void save_read_position(void)
{
    char id[FEISHU_MESSAGE_ID_MAX],name[96];
    uint32_t epoch,scope;read_position_t value;
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    if(!s_state.bookmark_dirty || s_state.bookmark_epoch!=s_owner_epoch) {
        if(s_state.bookmark_epoch!=s_owner_epoch)s_state.bookmark_dirty=false;
        xSemaphoreGive(s_state_lock);return;
    }
    feishu_utf8_copy(id,sizeof(id),s_state.bookmark_id);
    epoch=s_state.bookmark_epoch;scope=s_state.bookmark_scope;
    value=(read_position_t){.offset=(uint64_t)s_state.bookmark_page*FEISHU_TEXT_PAGE_STEP,
                            .scroll=s_state.bookmark_scroll,.version=1};
    xSemaphoreGive(s_state_lock);
    snprintf(name,sizeof(name),"read/%s",id);
    if(feishu_cache_write_if_scope(scope,name,&value,sizeof(value),1)!=ESP_OK)return;
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    if(epoch==s_owner_epoch && !strcmp(id,s_state.bookmark_id) &&
       value.offset==(uint64_t)s_state.bookmark_page*FEISHU_TEXT_PAGE_STEP && value.scroll==s_state.bookmark_scroll)
        s_state.bookmark_dirty=false;
    xSemaphoreGive(s_state_lock);
}

static void load_text_page(void)
{
    feishu_message_t message;
    uint32_t epoch,page,requested_page;int32_t scroll;bool restore,save_previous;
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    if(s_state.nav.page!=FEISHU_PAGE_MESSAGE_DETAIL || s_state.nav.message_index>=s_state.nav.message_count) {
        xSemaphoreGive(s_state_lock);return;
    }
    message=s_state.messages[s_state.nav.message_index];epoch=s_owner_epoch;
    page=s_state.detail_page;scroll=s_state.detail_scroll;restore=s_state.detail_restore;
    requested_page=page;
    save_previous=s_state.bookmark_dirty && strcmp(s_state.bookmark_id,message.message_id);
    xSemaphoreGive(s_state_lock);
    if(save_previous)save_read_position();
    uint64_t total=0;
    esp_err_t err=feishu_cache_text_pin(message.message_id,message.text_object_id,message.text_mapped,&s_text_pin,&total);
    if(err==ESP_OK && total!=message.text_length)err=ESP_ERR_INVALID_RESPONSE;
    bool full=err==ESP_OK;
    if(!full)feishu_cache_unpin(&s_text_pin);
    if(!full && !message.text_truncated) {total=strlen(message.text);err=ESP_OK;}
    if(total>64u*1024u*1024u)err=ESP_ERR_INVALID_SIZE;
    if(err!=ESP_OK) {
        feishu_cache_unpin(&s_text_pin);
        xSemaphoreTake(s_state_lock,portMAX_DELAY);
        bool active=epoch==s_owner_epoch && s_state.nav.page==FEISHU_PAGE_MESSAGE_DETAIL &&
            s_state.nav.message_index<s_state.nav.message_count &&
            !strcmp(message.message_id,s_state.messages[s_state.nav.message_index].message_id);
        bool retry=active && online() && !s_state.detail_refetch_attempted;
        if(active) {
            s_state.detail_loaded=false;s_state.detail_pages=1;s_state.detail_page=0;
            s_state.detail_scroll=s_state.detail_scroll_max=0;
            s_state.detail_refetch_attempted=true;s_state.loading=retry;
            feishu_utf8_copy(s_state.status,sizeof(s_state.status),"正文暂不可读，已保留预览");
        }
        xSemaphoreGive(s_state_lock);
        if(retry) {
            work_command_t command=WORK_FETCH_TEXT;
            if(xQueueSend(s_work_queue,&command,0)!=pdTRUE) {
                xSemaphoreTake(s_state_lock,portMAX_DELAY);s_state.loading=false;xSemaphoreGive(s_state_lock);
            }
        }
        return;
    }
    uint32_t pages=total ? (uint32_t)((total+FEISHU_TEXT_PAGE_STEP-1)/FEISHU_TEXT_PAGE_STEP) : 1;
    if(restore) {
        read_position_t saved;size_t count=0;char name[96];snprintf(name,sizeof(name),"read/%s",message.message_id);
        if(feishu_cache_read(name,&saved,sizeof(saved),1,&count,NULL)==ESP_OK && count==1 && saved.version==1) {
            uint64_t n=saved.offset/FEISHU_TEXT_PAGE_STEP;
            page=n>=pages ? pages-1 : (uint32_t)n;scroll=saved.scroll<0 ? 0 : saved.scroll;
        }
    }
    if(page>=pages)page=pages-1;
    char output[FEISHU_TEXT_PAGE_CAP];size_t length=0;
    if(full) {
        uint64_t start=(uint64_t)page*FEISHU_TEXT_PAGE_STEP;
        uint64_t offset=start>3 ? start-3 : 0;
        uint8_t raw[FEISHU_TEXT_PAGE_CAP];size_t got=0;
        err=feishu_cache_text_read(message.message_id,message.text_object_id,message.text_mapped,offset,raw,sizeof(raw),&got);
        if(err==ESP_OK && !feishu_text_page(raw,got,offset,total,page,output,&length))err=ESP_ERR_INVALID_RESPONSE;
    } else {
        length=feishu_utf8_copy(output,sizeof(output),message.text);
    }
    if(err!=ESP_OK)feishu_cache_unpin(&s_text_pin);
    uint32_t scope=feishu_cache_scope_version();
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    if(epoch==s_owner_epoch && s_state.nav.page==FEISHU_PAGE_MESSAGE_DETAIL &&
       s_state.nav.message_index<s_state.nav.message_count &&
       !strcmp(message.message_id,s_state.messages[s_state.nav.message_index].message_id) &&
       (restore || s_state.detail_page==requested_page)) {
        s_state.loading=false;s_state.detail_restore=false;s_state.detail_loaded=err==ESP_OK;
        if(err==ESP_OK) {
            memcpy(s_state.detail_text,output,length+1);
            s_state.detail_page=page;s_state.detail_pages=pages;s_state.detail_scroll=scroll;
            s_state.status[0]=0;s_state.bookmark_scope=scope;bookmark_locked();
        } else {
            s_state.detail_page=0;s_state.detail_pages=1;s_state.detail_scroll=s_state.detail_scroll_max=0;
            feishu_utf8_copy(s_state.status,sizeof(s_state.status),"正文暂不可读，已保留预览");
        }
    }
    xSemaphoreGive(s_state_lock);
}

static void load_draft_page(void)
{
    feishu_outbox_t p;uint32_t page,epoch;
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    if(s_state.nav.page==FEISHU_PAGE_REVIEW && s_state.pending_slot==RAM_REPLY_SLOT) {
        feishu_utf8_copy(s_state.detail_text,sizeof(s_state.detail_text),s_state.reply);
        s_state.detail_page=0;s_state.detail_pages=1;s_state.detail_loaded=true;s_state.loading=false;
        xSemaphoreGive(s_state_lock);return;
    }
    if(s_state.nav.page!=FEISHU_PAGE_REVIEW || s_state.pending_slot<0) {xSemaphoreGive(s_state_lock);return;}
    p=s_state.pending;page=s_state.detail_page;epoch=s_owner_epoch;
    xSemaphoreGive(s_state_lock);
    uint64_t total=0;char output[FEISHU_TEXT_PAGE_CAP];size_t length=0;
    esp_err_t err=feishu_cache_text_pin(p.body_name,p.object_id,false,&s_text_pin,&total);
    if(err==ESP_OK && total!=p.length)err=ESP_ERR_INVALID_RESPONSE;
    uint32_t pages=(uint32_t)((p.length+FEISHU_TEXT_PAGE_STEP-1)/FEISHU_TEXT_PAGE_STEP);
    if(!pages)pages=1;
    if(page>=pages)page=pages-1;
    if(err==ESP_OK) {
        uint64_t start=(uint64_t)page*FEISHU_TEXT_PAGE_STEP,offset=start>3 ? start-3:0;
        uint8_t bytes[FEISHU_TEXT_PAGE_CAP];size_t got=0;
        err=feishu_cache_text_read(p.body_name,p.object_id,false,offset,bytes,sizeof(bytes),&got);
        if(err==ESP_OK && !feishu_text_page(bytes,got,offset,p.length,page,output,&length))err=ESP_ERR_INVALID_RESPONSE;
    }
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    if(epoch==s_owner_epoch && s_state.nav.page==FEISHU_PAGE_REVIEW &&
       s_state.pending.object_id==p.object_id && s_state.detail_page==page) {
        s_state.loading=false;s_state.detail_loaded=err==ESP_OK;s_state.detail_pages=pages;
        if(err==ESP_OK) {
            memcpy(s_state.detail_text,output,length+1);
            if(!strcmp(s_state.status,"正在读取正文..."))s_state.status[0]=0;
        }
        else feishu_utf8_copy(s_state.status,sizeof(s_state.status),"草稿正文暂不可读，可取消后重录");
    }
    xSemaphoreGive(s_state_lock);
}
/* Caller holds the UI-state lock. No flash access from button callbacks. */
static void apply_pending_draft(int slot,const feishu_outbox_t *p)
{
    s_state.pending_slot=slot;s_state.pending_retry_warning=false;s_state.pending=*p;
    s_state.nav.reply_to_message=p->message[0]!=0;s_state.nav.page=FEISHU_PAGE_REVIEW;
    s_state.detail_page=0;s_state.detail_scroll=s_state.detail_scroll_max=0;s_state.detail_loaded=false;
    s_state.detail_pages=(uint32_t)((p->length+FEISHU_TEXT_PAGE_STEP-1)/FEISHU_TEXT_PAGE_STEP);
    s_state.reply[0]=0;s_state.loading=true;
    feishu_utf8_copy(s_state.status,sizeof(s_state.status),p->state==FEISHU_OUTBOX_READY ?
        "已恢复待确认草稿":"上次发送结果未确认，重试可能重复");
}
static void list_pending_drafts(void)
{
    uint32_t epoch;
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    if(s_state.nav.page!=FEISHU_PAGE_DRAFTS) {xSemaphoreGive(s_state_lock);return;}
    epoch=s_owner_epoch;xSemaphoreGive(s_state_lock);
    uint8_t slots[FEISHU_OUTBOX_SLOTS];char labels[FEISHU_OUTBOX_SLOTS][96];
    size_t count=0,errors=0;
    for(unsigned i=0;i<FEISHU_OUTBOX_SLOTS;++i) {
        feishu_outbox_t p={0};esp_err_t err=feishu_cache_pending_read(i,&p);
        if(err!=ESP_OK) {if(err!=ESP_ERR_NOT_FOUND)++errors;continue;}
        if(p.state==FEISHU_OUTBOX_EMPTY)continue;
        slots[count]=(uint8_t)i;
        char name[37];feishu_utf8_copy(name,sizeof(name),p.chat);
        xSemaphoreTake(s_state_lock,portMAX_DELAY);
        for(size_t j=0;j<s_state.nav.chat_count;++j)if(!strcmp(p.chat,s_state.chats[j].chat_id)) {
            feishu_utf8_copy(name,sizeof(name),s_state.chats[j].name);break;
        }
        xSemaphoreGive(s_state_lock);
        snprintf(labels[count],sizeof(labels[count]),"%s\n%s %llu字节",name,
            p.state==FEISHU_OUTBOX_READY ? "待确认":"结果未确认",(unsigned long long)p.length);
        ++count;
    }
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    if(epoch==s_owner_epoch && s_state.nav.page==FEISHU_PAGE_DRAFTS) {
        memcpy(s_state.draft_slots,slots,count);memcpy(s_state.draft_labels,labels,count*sizeof(labels[0]));
        s_state.nav.draft_count=count;if(s_state.nav.draft_index>=count)s_state.nav.draft_index=count ? count-1:0;
        s_state.loading=false;
        feishu_utf8_copy(s_state.status,sizeof(s_state.status),errors ? "部分草稿暂不可读，请稍后重试":"");
    }
    xSemaphoreGive(s_state_lock);
}
static void open_pending_draft(void)
{
    unsigned slot;uint32_t epoch;size_t index;
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    index=s_state.nav.draft_index;
    if(s_state.nav.page!=FEISHU_PAGE_DRAFTS || index>=s_state.nav.draft_count) {xSemaphoreGive(s_state_lock);return;}
    slot=s_state.draft_slots[index];epoch=s_owner_epoch;xSemaphoreGive(s_state_lock);
    feishu_outbox_t p={0};esp_err_t err=feishu_cache_pending_read(slot,&p);
    bool active=false;
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    if(epoch==s_owner_epoch && s_state.nav.page==FEISHU_PAGE_DRAFTS && s_state.nav.draft_index==index) {
        s_state.loading=false;
        if(err==ESP_OK && p.state!=FEISHU_OUTBOX_EMPTY) {apply_pending_draft((int)slot,&p);active=true;}
        else feishu_utf8_copy(s_state.status,sizeof(s_state.status),"草稿暂不可读，返回后可重试");
    }
    xSemaphoreGive(s_state_lock);
    if(active)load_draft_page();
}
static void load_pending_draft(void)
{
    int64_t started=esp_timer_get_time();
    char chat[FEISHU_CHAT_ID_MAX];uint32_t epoch;
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    if(s_state.nav.page!=FEISHU_PAGE_REPLY_READY || s_state.nav.chat_index>=s_state.nav.chat_count) {xSemaphoreGive(s_state_lock);return;}
    feishu_utf8_copy(chat,sizeof(chat),s_state.chats[s_state.nav.chat_index].chat_id);epoch=s_owner_epoch;
    xSemaphoreGive(s_state_lock);
    feishu_outbox_t p={0};int slot=-1;unsigned errors=0;
    for(unsigned i=0;i<FEISHU_OUTBOX_SLOTS;++i) {
        esp_err_t err=feishu_cache_pending_read(i,&p);
        if(err==ESP_OK && !strcmp(chat,p.chat)) {slot=(int)i;break;}
        if(err!=ESP_OK && err!=ESP_ERR_NOT_FOUND)++errors;
    }
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    bool active=epoch==s_owner_epoch && s_state.nav.page==FEISHU_PAGE_REPLY_READY &&
        s_state.nav.chat_index<s_state.nav.chat_count && !strcmp(chat,s_state.chats[s_state.nav.chat_index].chat_id);
    if(active) {
        s_state.pending_slot=slot;s_state.pending_retry_warning=false;s_state.loading=false;
        if(slot>=0) {
            apply_pending_draft(slot,&p);
        } else if(errors) {
            s_state.nav.page=FEISHU_PAGE_STATUS;s_state.status_error=true;
            feishu_utf8_copy(s_state.status,sizeof(s_state.status),"草稿暂不可读，返回后可重试");
        }
    }
    xSemaphoreGive(s_state_lock);
    ESP_LOGI(TAG,"draft lookup found=%u elapsed_ms=%u",(unsigned)(slot>=0),(unsigned)((esp_timer_get_time()-started)/1000));
    if(active && slot>=0)load_draft_page();
}

enum { IMAGE_OBSOLETE,IMAGE_APPLIED,IMAGE_FETCH,IMAGE_AGAIN };
static unsigned apply_image(const char *message,uint32_t epoch,uint32_t request,
                              feishu_prepared_image_t *prepared,esp_err_t err,bool final)
{
    int64_t release=0;unsigned action=IMAGE_OBSOLETE;
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    bool visible=s_state.nav.page==FEISHU_PAGE_IMAGE || s_state.nav.page==FEISHU_PAGE_IMAGE_ACTIONS;
    bool active=visible && epoch==s_owner_epoch && request==s_state.image_request &&
        s_state.nav.message_index<s_state.nav.message_count &&
        !strcmp(message,s_state.messages[s_state.nav.message_index].message_id);
    if(active && err!=ESP_OK && !final)action=IMAGE_FETCH;
    else if(active) {
        bool quick=err==ESP_OK && prepared->approximate && !final;
        action=quick ? IMAGE_FETCH:IMAGE_APPLIED;s_state.image_busy=quick;s_state.loading=false;
        if(err==ESP_OK) {
            ESP_LOGI(TAG,"preview ready: z%u x%u y%u %ux%u request %u",(unsigned)prepared->info.zoom,
                (unsigned)prepared->info.left,(unsigned)prepared->info.top,(unsigned)prepared->info.width,
                (unsigned)prepared->info.height,(unsigned)request);
            release=s_image_pin;s_image_pin=prepared->pin;prepared->pin=0;
            s_state.image=*prepared;s_state.image_ready=true;++s_state.image_version;
            s_state.image_view.zoom=prepared->info.zoom;
            s_state.image_view.center_x=(int32_t)(prepared->info.left+prepared->info.crop_width/2);
            s_state.image_view.center_y=(int32_t)(prepared->info.top+prepared->info.crop_height/2);
            feishu_utf8_copy(s_state.status,sizeof(s_state.status),prepared->cached ? "缓存预览":"");
        } else {
            feishu_utf8_copy(s_state.status,sizeof(s_state.status),
                err==ESP_ERR_NOT_FOUND || (!online() && err==ESP_ERR_INVALID_STATE) ? "此区域未缓存，请先联网":
                err==ESP_ERR_NOT_SUPPORTED ? "编码不支持，请用普通 JPG":
                err==ESP_ERR_INVALID_SIZE ? "图片或缓存过大，原预览保留":
                err==ESP_ERR_TIMEOUT ? "预览处理超时，可重试":
                err==ESP_ERR_NO_MEM ? "内存不足，原预览保留":
                err==FEISHU_HTTP_ERR_CANCELLED ? "已停止更新预览":"图片暂不可读，原预览保留");
            if(s_state.image_ready && s_state.image.approximate)
                feishu_utf8_copy(s_state.status,sizeof(s_state.status),"细节暂不可用，低清预览保留");
        }
    } else if(visible && s_state.image_busy && request!=s_state.image_request)action=IMAGE_AGAIN;
    xSemaphoreGive(s_state_lock);
    feishu_cache_unpin(&release);feishu_cache_unpin(&prepared->pin);return action;
}
static void load_cached_image(void)
{
    feishu_message_t message={0};feishu_image_view_t view;uint32_t epoch,request;
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    bool visible=(s_state.nav.page==FEISHU_PAGE_IMAGE || s_state.nav.page==FEISHU_PAGE_IMAGE_ACTIONS) &&
        s_state.nav.message_index<s_state.nav.message_count;
    if(visible)message=s_state.messages[s_state.nav.message_index];
    view=s_state.image_view;epoch=s_owner_epoch;request=s_state.image_request;
    xSemaphoreGive(s_state_lock);
    if(!visible)return;
    feishu_prepared_image_t prepared={0};
    esp_err_t err=feishu_image_cached(message.message_id,message.resource_key,&view,&prepared);
    if(err!=ESP_OK) {
        esp_err_t quick=feishu_image_quick_view(message.message_id,message.resource_key,&view,&prepared);
        if(quick==ESP_OK)err=ESP_OK;
    }
    unsigned action=apply_image(message.message_id,epoch,request,&prepared,err,false);
    if(action==IMAGE_FETCH)enqueue(WORK_FETCH_IMAGE);
    else if(action==IMAGE_AGAIN)enqueue(WORK_LOAD_IMAGE);
}

/* Disk hits stay responsive while the network worker waits for Wi-Fi/TLS.
 * The cache adapter serializes its sole SQLite connection across both tasks. */
static void cache_worker_task(void *unused)
{
    (void)unused;
    work_command_t command;
    TickType_t last_position_save=0;
    for(;;) {
        xSemaphoreTake(s_state_lock,portMAX_DELAY);
        bool closing=s_state.closing;
        bool detail=s_state.nav.page==FEISHU_PAGE_MESSAGE_DETAIL || s_state.nav.page==FEISHU_PAGE_REVIEW ||
            s_state.nav.page==FEISHU_PAGE_DRAFT_ACTIONS;
        bool image=(s_state.nav.page==FEISHU_PAGE_IMAGE || s_state.nav.page==FEISHU_PAGE_IMAGE_ACTIONS) && s_state.image_ready;
        int64_t release_image=0;
        if(!image) {release_image=s_image_pin;s_image_pin=0;}
        size_t index=s_state.nav.chat_index;
        xSemaphoreGive(s_state_lock);
        if(closing) {feishu_cache_unpin(&release_image);save_read_position();break;}
        if(!detail)feishu_cache_unpin(&s_text_pin);
        feishu_cache_unpin(&release_image);
        if(xQueueReceive(s_cache_queue,&command,pdMS_TO_TICKS(100))!=pdTRUE) {
            if(xTaskGetTickCount()-last_position_save>=pdMS_TO_TICKS(2000)) {
                save_read_position();last_position_save=xTaskGetTickCount();
            }
            continue;
        }
        if(command==WORK_EXIT) break;
        if(command==WORK_LOAD_TEXT_PAGE) {load_text_page();continue;}
        if(command==WORK_LOAD_DRAFT) {load_pending_draft();continue;}
        if(command==WORK_LOAD_DRAFT_PAGE) {load_draft_page();continue;}
        if(command==WORK_LIST_DRAFTS) {list_pending_drafts();continue;}
        if(command==WORK_OPEN_DRAFT) {open_pending_draft();continue;}
        if(command==WORK_CHAT_PAGE) {load_chat_page(false);continue;}
        if(command==WORK_MESSAGE_PAGE) {load_message_page(false);continue;}
        if(command==WORK_LOAD_IMAGE) {
            xSemaphoreTake(s_state_lock,portMAX_DELAY);s_state.image_cache_queued=false;xSemaphoreGive(s_state_lock);
            load_cached_image();continue;
        }
        /* Capture selection after receiving the request. */
        xSemaphoreTake(s_state_lock,portMAX_DELAY);
        index=s_state.nav.chat_index;
        xSemaphoreGive(s_state_lock);
        esp_err_t err=load_messages_for_chat(index,true,true);
        xSemaphoreTake(s_state_lock,portMAX_DELAY);
        bool visible=s_state.nav.page==FEISHU_PAGE_MESSAGES;
        xSemaphoreGive(s_state_lock);
        if(!visible) continue;
        if(err!=ESP_OK) {
            command=WORK_LOAD_MESSAGES;
            if(xQueueSend(s_work_queue,&command,0)==pdTRUE) continue;
        }
        xSemaphoreTake(s_state_lock,portMAX_DELAY);
        s_state.loading=false;
        feishu_utf8_copy(s_state.status,sizeof(s_state.status),err==ESP_OK ?
            (online() ? "缓存，后台更新中" : "离线缓存") : "操作繁忙，请稍后重试");
        xSemaphoreGive(s_state_lock);
    }
    s_cache_worker=NULL;
    xSemaphoreGive(s_cache_done);
    vTaskDelete(NULL);
}

static void start_cache_worker(void)
{
    if(s_cache_worker || feishu_cache_init()!=ESP_OK) return;
    if(!s_cache_queue) s_cache_queue=xQueueCreate(4,sizeof(work_command_t));
    if(!s_cache_done) s_cache_done=xSemaphoreCreateBinary();
    if(s_cache_queue && s_cache_done)
        xTaskCreate(cache_worker_task,"feishu_cache",8192,NULL,3,&s_cache_worker);
}

static void fetch_current_text(void)
{
    feishu_message_t next;
    char id[FEISHU_MESSAGE_ID_MAX];uint32_t epoch;
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    if(s_state.nav.page!=FEISHU_PAGE_MESSAGE_DETAIL || s_state.nav.message_index>=s_state.nav.message_count) {
        xSemaphoreGive(s_state_lock);return;
    }
    feishu_utf8_copy(id,sizeof(id),s_state.messages[s_state.nav.message_index].message_id);
    epoch=s_owner_epoch;
    xSemaphoreGive(s_state_lock);
    esp_err_t err=online() ? feishu_api_get_message(&s_session,id,&next) : ESP_ERR_INVALID_STATE;
    bool active=false;
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    active=epoch==s_owner_epoch && s_state.nav.page==FEISHU_PAGE_MESSAGE_DETAIL &&
        s_state.nav.message_index<s_state.nav.message_count &&
        !strcmp(id,s_state.messages[s_state.nav.message_index].message_id);
    if(active && err==ESP_OK) {
        feishu_message_t *previous=&s_state.messages[s_state.nav.message_index];
        if(!strcmp(next.sender_name,"未知用户"))feishu_utf8_copy(next.sender_name,sizeof(next.sender_name),previous->sender_name);
        *previous=next;
    } else if(active) {
        s_state.loading=false;
        feishu_utf8_copy(s_state.status,sizeof(s_state.status),"暂时无法读取正文，已保留预览");
    }
    xSemaphoreGive(s_state_lock);
    if(active && err==ESP_OK) {
        if(next.text_object_id>0) {
            uint64_t size=0;
            feishu_cache_text_pin(id,next.text_object_id,next.text_mapped,&s_text_pin,&size);
        }
        feishu_message_t *snapshot=calloc(FEISHU_MAX_MESSAGES,sizeof(*snapshot));
        if(snapshot) {
            char name[FEISHU_CHAT_ID_MAX+16]={0};size_t count=0;
            xSemaphoreTake(s_state_lock,portMAX_DELAY);
            if(epoch==s_owner_epoch && s_state.nav.chat_index<s_state.nav.chat_count) {
                memcpy(snapshot,s_state.messages,sizeof(s_state.messages));count=s_state.nav.message_count;
                snprintf(name,sizeof(name),"messages/%s",s_state.chats[s_state.nav.chat_index].chat_id);
            }
            xSemaphoreGive(s_state_lock);
            if(name[0] && feishu_cache_write(name,snapshot,sizeof(*snapshot),count)==ESP_OK)
                feishu_cache_protect(name,&s_message_pin);
            free(snapshot);
        }
        work_command_t command=WORK_LOAD_TEXT_PAGE;
        QueueHandle_t queue=s_cache_worker ? s_cache_queue : s_work_queue;
        if(xQueueSend(queue,&command,0)!=pdTRUE) {
            xSemaphoreTake(s_state_lock,portMAX_DELAY);s_state.loading=false;xSemaphoreGive(s_state_lock);
        }
    }
    recover_revoked_auth(err);
}

static void worker_task(void *argument)
{
    (void)argument;
    work_command_t command;
    TickType_t last_chat_refresh = 0;

    for (;;) {
        if (xQueueReceive(s_work_queue, &command,
                          pdMS_TO_TICKS(8000)) != pdTRUE) {
            feishu_page_t page;
            size_t chat_index;
            bool ready;
            bool initialized;
            xSemaphoreTake(s_state_lock, portMAX_DELAY);
            int battery_percent = bsp_battery_soc();
            if (battery_percent >= 0) s_state.battery_percent = battery_percent;
            initialized = s_state.ready;
            ready = initialized && !s_state.loading;
            page = s_state.nav.page;
            chat_index = s_state.nav.chat_index;
            xSemaphoreGive(s_state_lock);
            if (ready && online() && page == FEISHU_PAGE_MESSAGES) {
                esp_err_t refresh_err = load_messages_for_chat(chat_index, true, false);
                recover_revoked_auth(refresh_err);
                if (refresh_err != ESP_OK) {
                    ESP_LOGW(TAG, "background message refresh failed");
                }
            } else if (ready && online() && page == FEISHU_PAGE_CHATS &&
                       xTaskGetTickCount() - last_chat_refresh >=
                           pdMS_TO_TICKS(8000)) {
                esp_err_t refresh_err = refresh_conversations();
                recover_revoked_auth(refresh_err);
                if (refresh_err != ESP_OK) {
                    ESP_LOGW(TAG, "background conversation refresh failed");
                }
                last_chat_refresh = xTaskGetTickCount();
            }
            // Failed initialization previously left every key disabled forever.
            // Retry only initialization, never replay a queued send/record action.
            if (initialized && online()) continue;
            xSemaphoreTake(s_state_lock, portMAX_DELAY);
            s_state.loading = !initialized;
            xSemaphoreGive(s_state_lock);
            command = WORK_INIT;
        }
        esp_err_t err = ESP_OK;
        xSemaphoreTake(s_state_lock,portMAX_DELAY);
        bool closing=s_state.closing;
        xSemaphoreGive(s_state_lock);
        if (command == WORK_EXIT || closing) break;
        if(!online() && (command==WORK_RECORD || command==WORK_SEND)) {
            xSemaphoreTake(s_state_lock,portMAX_DELAY);
            s_state.loading=false;
            s_state.nav.page=FEISHU_PAGE_STATUS;
            feishu_utf8_copy(s_state.status,sizeof(s_state.status),"离线缓存可读，请联网后重试此操作");
            xSemaphoreGive(s_state_lock);
            continue;
        }
        if (command == WORK_INIT) {
            start_cache_worker();
            xSemaphoreTake(s_state_lock,portMAX_DELAY);uint32_t init_request=s_state.chat_page_request;xSemaphoreGive(s_state_lock);
            err = initialize_feishu();
            if (err != ESP_OK) ESP_LOGW(TAG, "initialization failed: %s", esp_err_to_name(err));
            xSemaphoreTake(s_state_lock, portMAX_DELAY);
            bool cached_ready=s_state.ready;
            bool home=s_state.nav.page==FEISHU_PAGE_CHATS && s_state.chat_page_number==0 &&
                (!s_state.loading || init_request==s_state.chat_page_request);
            s_state.ready = cached_ready || err == ESP_OK;
            if(home) {
            s_state.loading=false;
            feishu_utf8_copy(s_state.status, sizeof(s_state.status),
                             err == ESP_OK ? "" :
                             (cached_ready && s_state.nav.chat_count>0 ? "显示已有内容，稍后重试更新" :
                             err == ESP_ERR_INVALID_STATE || err == ESP_ERR_NVS_NOT_FOUND ?
                              "请先完成配网和绑定" : "暂时无法连接飞书，稍后重试"));
            }
            xSemaphoreGive(s_state_lock);
            if (err == ESP_OK && home) {
#ifdef FEISHU_VOICE_AUTOTEST
                /* Run after normal initialization/UI allocation, before slow
                 * optional summary enrichment can consume the probe window. */
                static bool tested;
                if(!tested) {
                    tested=true;
#ifdef FEISHU_DRAFT_AUTOTEST
                    if(feishu_draft_autotest_resume())continue;
#ifdef FEISHU_DRAFT_VOICE_SUITE
                    feishu_asr_autotest(&s_session);
#else
                    if(!feishu_asr_autotest_provision()) {
                        ESP_LOGI(TAG,"DRAFT_PHASE phase=1 pass=0 provision=0");continue;
                    }
#endif
#elif defined(FEISHU_BUSINESS_AUTOTEST)
#ifdef FEISHU_SEND_PERF_AUTOTEST
                    feishu_business_autotest(&s_session);continue;
#else
                    if(feishu_asr_autotest_provision())feishu_business_autotest(&s_session);
                    else ESP_LOGI(TAG,"BUSINESS_DONE pass=0 voice_provision_failed=1");
#endif
#else
                    feishu_asr_autotest(&s_session);
#endif
                    xSemaphoreTake(s_state_lock,portMAX_DELAY);
                    s_voice_ui_probe=s_state.nav.chat_count>0;
                    if(s_voice_ui_probe) {
                        s_state.nav.chat_index=0;s_state.nav.reply_to_message=false;s_state.nav.draft_manager=false;
                        s_state.nav.page=FEISHU_PAGE_RECORDING;s_state.loading=true;
                        s_state.record_pending=true;s_state.record_replaces_pending=false;
                        s_state.stop_recording=s_state.cancel_recording=false;
                        feishu_asr_autotest_prepare(&s_state.stop_recording,&s_state.cancel_recording);
                    }
                    xSemaphoreGive(s_state_lock);
                    if(s_voice_ui_probe){enqueue(WORK_RECORD);continue;}
                }
#endif
                esp_err_t refresh_err = refresh_conversations();
                recover_revoked_auth(refresh_err);
                if (refresh_err != ESP_OK) {
                    ESP_LOGW(TAG, "initial conversation enrichment failed");
                }
            }
        } else if(command==WORK_LOAD_TEXT_PAGE) {
            load_text_page();
        } else if(command==WORK_LOAD_DRAFT) {
            load_pending_draft();
        } else if(command==WORK_LOAD_DRAFT_PAGE) {
            load_draft_page();
        } else if(command==WORK_LIST_DRAFTS) {
            list_pending_drafts();
        } else if(command==WORK_OPEN_DRAFT) {
            open_pending_draft();
        } else if(command==WORK_CHAT_PAGE || command==WORK_FETCH_CHAT_PAGE) {
            err=load_chat_page(true);
        } else if(command==WORK_MESSAGE_PAGE || command==WORK_FETCH_MESSAGE_PAGE) {
            err=load_message_page(true);
        } else if(command==WORK_FETCH_TEXT) {
            fetch_current_text();
        } else if (command == WORK_REFRESH_MESSAGES) {
            xSemaphoreTake(s_state_lock,portMAX_DELAY);
            bool visible=s_state.nav.page==FEISHU_PAGE_MESSAGES;
            size_t index=s_state.nav.chat_index;
            xSemaphoreGive(s_state_lock);
            if(visible && online() && uxQueueMessagesWaiting(s_work_queue)==0) {
                err=load_messages_for_chat(index,true,false);
                if(err!=ESP_OK) {
                    xSemaphoreTake(s_state_lock,portMAX_DELAY);
                    feishu_utf8_copy(s_state.status,sizeof(s_state.status),"缓存可读，暂时无法更新");
                    xSemaphoreGive(s_state_lock);
                }
            }
        } else if (command == WORK_LOAD_MESSAGES) {
            size_t chat_index;
            xSemaphoreTake(s_state_lock, portMAX_DELAY);
            chat_index = s_state.nav.chat_index;
            xSemaphoreGive(s_state_lock);
            err = load_messages_for_chat(chat_index, true, true);
            if(err!=ESP_OK)err=load_messages_for_chat(chat_index,true,false);
            if (err != ESP_OK) ESP_LOGW(TAG, "message load failed: %s", esp_err_to_name(err));
            xSemaphoreTake(s_state_lock, portMAX_DELAY);
            if(s_state.nav.page==FEISHU_PAGE_MESSAGES && s_state.nav.chat_index==chat_index) {
            s_state.loading = false;
            if (err != ESP_OK) {
                s_state.nav.page = FEISHU_PAGE_STATUS;
                feishu_utf8_copy(s_state.status, sizeof(s_state.status),
                                 "无法加载消息，请稍后重试");
            } else {
                s_state.status[0] = '\0';
            }
            }
            xSemaphoreGive(s_state_lock);
        } else if (command == WORK_LOAD_IMAGE || command==WORK_FETCH_IMAGE) {
            feishu_message_t message={0};feishu_image_view_t view;
            uint32_t request,epoch;
            xSemaphoreTake(s_state_lock,portMAX_DELAY);
            if(command==WORK_FETCH_IMAGE)s_state.image_fetch_queued=false;
            else s_state.image_cache_queued=false;
            bool wanted=(s_state.nav.page==FEISHU_PAGE_IMAGE || s_state.nav.page==FEISHU_PAGE_IMAGE_ACTIONS) &&
                s_state.nav.message_index<s_state.nav.message_count;
            if(wanted)message=s_state.messages[s_state.nav.message_index];
            view=s_state.image_view;request=s_state.image_request;epoch=s_owner_epoch;
            xSemaphoreGive(s_state_lock);
            feishu_prepared_image_t prepared={0};
            if(wanted)err=feishu_api_prepare_image(&s_session,&message,&view,online(),&prepared);
            else err=FEISHU_HTTP_ERR_CANCELLED;
            unsigned next=apply_image(message.message_id,epoch,request,&prepared,err,true);
            if(next==IMAGE_AGAIN)enqueue(WORK_LOAD_IMAGE);
        } else if (command == WORK_RECORD) {
            char recognition[FEISHU_REPLY_MAX]={0},chat[64]={0},message[64]={0},uuid[41],name[64];
            int slot=-1;bool cancelled;
            uint32_t epoch;
            xSemaphoreTake(s_state_lock,portMAX_DELAY);
            cancelled=s_state.cancel_recording || (s_state.nav.page!=FEISHU_PAGE_RECORDING && s_state.nav.page!=FEISHU_PAGE_TRANSCRIBING);
            epoch=s_owner_epoch;
            if(s_state.record_replaces_pending && s_state.pending_slot>=0) {
                slot=s_state.pending_slot;
                feishu_utf8_copy(chat,sizeof(chat),s_state.pending.chat);
                feishu_utf8_copy(message,sizeof(message),s_state.pending.message);
            } else if(s_state.nav.chat_index<s_state.nav.chat_count) {
                feishu_utf8_copy(chat,sizeof(chat),s_state.chats[s_state.nav.chat_index].chat_id);
                if(s_state.nav.reply_to_message && s_state.nav.message_index<s_state.nav.message_count)
                    feishu_utf8_copy(message,sizeof(message),s_state.messages[s_state.nav.message_index].message_id);
            }
            xSemaphoreGive(s_state_lock);
            if(cancelled) {
                xSemaphoreTake(s_state_lock,portMAX_DELAY);
                s_state.loading=false;s_state.record_pending=false;s_state.recovering_operation=false;
                xSemaphoreGive(s_state_lock);continue;
            }
            if(slot<0 && slot!=RAM_REPLY_SLOT)for(unsigned i=0;i<FEISHU_OUTBOX_SLOTS;++i) {
                feishu_outbox_t existing;
                esp_err_t found=feishu_cache_pending_read(i,&existing);
                if(found==ESP_ERR_NOT_FOUND) {slot=(int)i;break;}
                if(found!=ESP_OK) {err=found;break;}
            }
            if((slot<0 && slot!=RAM_REPLY_SLOT) || !chat[0])err=ESP_ERR_INVALID_STATE;
            feishu_data_writer_t draft={0};
#ifdef FEISHU_DRAFT_AUTOTEST
            bool monitor_heap=false;
            if(s_voice_ui_probe && err==ESP_OK && !cancelled) {
                ESP_LOGI(TAG,"DRAFT_MEMORY boot_minimum=%u",(unsigned)esp_get_minimum_free_heap_size());
                monitor_heap=heap_caps_monitor_local_minimum_free_size_start()==ESP_OK;
            }
#endif
            make_reply_uuid(uuid);snprintf(name,sizeof(name),"draft_%s",uuid);
            xSemaphoreTake(s_state_lock,portMAX_DELAY);
            cancelled=s_state.cancel_recording;
            xSemaphoreGive(s_state_lock);
            if(err==ESP_OK && !cancelled)err=feishu_asr_record(&s_session,&s_state.stop_recording,&s_state.cancel_recording,
                &s_state.elapsed_ms,recognition,sizeof(recognition),slot==RAM_REPLY_SLOT ? NULL:&draft,&s_state.record_pending);
            xSemaphoreTake(s_state_lock,portMAX_DELAY);
            cancelled=s_state.cancel_recording || epoch!=s_owner_epoch;
            xSemaphoreGive(s_state_lock);
            if(err!=ESP_OK)ESP_LOGW(TAG,"record prepare/capture failed: %s slot=%d",esp_err_to_name(err),slot);
            feishu_outbox_t pending={0};
            if(!cancelled && err==ESP_OK && slot==RAM_REPLY_SLOT && recognition[0]) {
                pending.version=1;pending.state=FEISHU_OUTBOX_READY;pending.length=strlen(recognition);
                feishu_utf8_copy(pending.chat,sizeof(pending.chat),chat);
                feishu_utf8_copy(pending.message,sizeof(pending.message),message);
                feishu_utf8_copy(pending.uuid,sizeof(pending.uuid),uuid);
            } else if(!cancelled && err==ESP_OK && draft.id && draft.raw && recognition[0]) {
                if(!feishu_outbox_create(&pending,draft.id,draft.raw,name,chat,message,uuid))err=ESP_ERR_INVALID_RESPONSE;
                if(err==ESP_OK)err=feishu_cache_pending_write((unsigned)slot,&pending,&draft);
            } else if(!cancelled && err==ESP_OK)err=ESP_ERR_NOT_FOUND;
            feishu_cache_text_abort(&draft);
#ifdef FEISHU_DRAFT_AUTOTEST
            if(monitor_heap) {
                ESP_LOGI(TAG,"DRAFT_MEMORY record_minimum=%u heap=%u",(unsigned)esp_get_minimum_free_heap_size(),(unsigned)esp_get_free_heap_size());
                heap_caps_monitor_local_minimum_free_size_stop();
            }
#endif
            xSemaphoreTake(s_state_lock,portMAX_DELAY);
            if(epoch!=s_owner_epoch) {
                s_state.loading=s_state.record_pending=s_state.recovering_operation=false;
                xSemaphoreGive(s_state_lock);memset(recognition,0,sizeof(recognition));memset(uuid,0,sizeof(uuid));continue;
            }
            s_state.loading=false;s_state.record_pending=false;
            s_state.status_error=!cancelled && err!=ESP_OK;
            if(cancelled) {
                s_state.cancel_recording=false;s_state.nav.page=s_state.nav.draft_manager ? FEISHU_PAGE_DRAFTS:FEISHU_PAGE_MESSAGES;
            } else if(err==ESP_OK) {
                s_state.pending=pending;s_state.pending_slot=slot;s_state.pending_retry_warning=false;
                feishu_utf8_copy(s_state.reply,sizeof(s_state.reply),recognition);
                feishu_utf8_copy(s_state.reply_uuid,sizeof(s_state.reply_uuid),uuid);
                s_state.nav.page=FEISHU_PAGE_REVIEW;s_state.detail_page=0;
                s_state.detail_pages=(uint32_t)((pending.length+FEISHU_TEXT_PAGE_STEP-1)/FEISHU_TEXT_PAGE_STEP);
                s_state.detail_loaded=false;s_state.detail_scroll=s_state.detail_scroll_max=0;
                if(slot==RAM_REPLY_SLOT) {
                    feishu_utf8_copy(s_state.detail_text,sizeof(s_state.detail_text),recognition);
                    s_state.detail_pages=1;s_state.detail_loaded=true;
                    feishu_utf8_copy(s_state.status,sizeof(s_state.status),"暂存内存，断电不保留");
                } else feishu_utf8_copy(s_state.status,sizeof(s_state.status),"草稿已保存，长按确定可返回");
            } else {
                s_state.nav.page=s_state.nav.draft_manager ? FEISHU_PAGE_DRAFTS:FEISHU_PAGE_MESSAGES;
                feishu_utf8_copy(s_state.status,sizeof(s_state.status),
                    slot<0 && slot!=RAM_REPLY_SLOT ? "待发送草稿已满或暂不可读，请先处理已有草稿":
                    err==FEISHU_ERR_ACCESS_REJECTED ? "缺少语音权限，请重新授权":
                    err==FEISHU_ERR_ASR_LIMITED ? "语音服务受限，当前无法识别":
                    err==FEISHU_ERR_VOICE_LOGIN ? "请先授权语音登录，会话功能不受影响":
                    err==FEISHU_ERR_ASR_TOO_LONG ? "内容超过内存预览上限，请分段录音":
                    err==ESP_ERR_TIMEOUT ? "网络暂时跟不上录音，请稍后重试":
                    err==ESP_ERR_NO_MEM ? "录音资源暂不足，返回列表后重试":
                    err==ESP_ERR_NOT_FOUND ? "没有识别到语音，请重试":"识别或保存失败，原有草稿保留");
            }
            xSemaphoreGive(s_state_lock);
#ifdef FEISHU_VOICE_AUTOTEST
            if(s_voice_ui_probe) {
                feishu_asr_autotest_finish();s_voice_ui_probe=false;
                if(err==ESP_OK)load_draft_page();
                xSemaphoreTake(s_state_lock,portMAX_DELAY);
                bool pass=err==ESP_OK && s_state.pending_slot>=0 && s_state.reply[0] &&
                    feishu_outbox_valid(&s_state.pending) && s_state.nav.page==FEISHU_PAGE_REVIEW &&
                    s_state.detail_loaded && !strcmp(s_state.detail_text,s_state.reply);
                ESP_LOGI(TAG,"VOICE_UI_DRAFT pass=%u err=%d bytes=%u slot=%d disk_writer=1",
                    (unsigned)pass,err,(unsigned)strlen(s_state.reply),s_state.pending_slot);
                s_state.detail_scroll_max=0;
                s_state.nav.page=FEISHU_PAGE_DRAFT_ACTIONS;s_state.nav.settings_index=0;
                xSemaphoreGive(s_state_lock);
                s_voice_force_offline=true;handle_navigation(FEISHU_NAV_OK);s_voice_force_offline=false;
                xSemaphoreTake(s_state_lock,portMAX_DELAY);
                bool retained=s_state.nav.page==FEISHU_PAGE_REVIEW && s_state.pending_slot>=0 &&
                    s_state.reply[0] && !s_state.sending && !s_state.loading;
                ESP_LOGI(TAG,"VOICE_UI_OFFLINE pass=%u retained=%u sends=0",(unsigned)retained,(unsigned)strlen(s_state.reply));
                s_state.status_error=false;feishu_utf8_copy(s_state.status,sizeof(s_state.status),"草稿已保存，长按确定可返回");
                xSemaphoreGive(s_state_lock);
#ifdef FEISHU_DRAFT_AUTOTEST
                feishu_draft_autotest_saved(slot,&pending,recognition,pass && retained);
#endif
            }
#endif
            if(slot!=RAM_REPLY_SLOT) {
                if(err==ESP_OK && !cancelled)enqueue(WORK_LOAD_DRAFT_PAGE);
                else list_pending_drafts();
            }
            memset(recognition,0,sizeof(recognition));memset(uuid,0,sizeof(uuid));
        } else if(command==WORK_DISCARD_DRAFT) {
            feishu_outbox_t pending;int slot;
            xSemaphoreTake(s_state_lock,portMAX_DELAY);
            pending=s_state.pending;slot=s_state.pending_slot;
            xSemaphoreGive(s_state_lock);
            if(slot==RAM_REPLY_SLOT)err=ESP_OK;
            else if(slot<0)err=ESP_ERR_NOT_FOUND;
            else {pending.state=FEISHU_OUTBOX_EMPTY;err=feishu_cache_pending_write((unsigned)slot,&pending,NULL);}
            xSemaphoreTake(s_state_lock,portMAX_DELAY);
            s_state.loading=false;
            if(err==ESP_OK) {
                s_state.pending_slot=-1;memset(&s_state.pending,0,sizeof(s_state.pending));
                s_state.reply[0]=s_state.reply_uuid[0]=0;s_state.nav.page=s_state.nav.draft_manager ? FEISHU_PAGE_DRAFTS:FEISHU_PAGE_MESSAGES;
                feishu_utf8_copy(s_state.status,sizeof(s_state.status),"草稿已取消");
            } else {
                s_state.nav.page=FEISHU_PAGE_REVIEW;
                feishu_utf8_copy(s_state.status,sizeof(s_state.status),"取消未完成，草稿仍保留");
            }
            xSemaphoreGive(s_state_lock);
            if(err==ESP_OK && slot!=RAM_REPLY_SLOT)list_pending_drafts();
        } else if (command == WORK_SEND) {
            feishu_outbox_t pending;int slot;bool retry;uint32_t send_epoch;
            xSemaphoreTake(s_state_lock,portMAX_DELAY);
            bool wanted=s_state.sending;
            pending=s_state.pending;slot=s_state.pending_slot;retry=s_state.pending_retry_warning;send_epoch=s_owner_epoch;
            if(!wanted && s_state.recovering_operation)s_state.loading=s_state.recovering_operation=false;
            xSemaphoreGive(s_state_lock);
            if(!wanted)continue;
            bool memory=slot==RAM_REPLY_SLOT;
            if(memory) {
                char text[FEISHU_REPLY_MAX];bool paused;
                xSemaphoreTake(s_state_lock,portMAX_DELAY);
                feishu_utf8_copy(text,sizeof(text),s_state.reply);paused=s_state.cancel_send;
                xSemaphoreGive(s_state_lock);
                if(paused)err=FEISHU_HTTP_ERR_CANCELLED;
                else if(!text[0] || strlen(text)!=pending.length || !pending.chat[0] || !pending.uuid[0])err=ESP_ERR_INVALID_STATE;
                else {
                    pending.state=FEISHU_OUTBOX_SENDING;
                    err=pending.message[0] ? feishu_api_reply_text(&s_session,pending.message,text,pending.uuid):
                        feishu_api_send_text(&s_session,pending.chat,text,pending.uuid);
                    pending.state=err==ESP_OK ? FEISHU_OUTBOX_EMPTY:FEISHU_OUTBOX_UNCERTAIN;
                }
                memset(text,0,sizeof(text));
            } else if(slot<0 || !feishu_outbox_valid(&pending))err=ESP_ERR_INVALID_STATE;
            while(!memory && err==ESP_OK && pending.state!=FEISHU_OUTBOX_EMPTY) {
                xSemaphoreTake(s_state_lock,portMAX_DELAY);
                bool paused=s_state.cancel_send;
                xSemaphoreGive(s_state_lock);
                if(paused) {err=ESP_ERR_INVALID_STATE;break;}
                if(!feishu_outbox_attempt(&pending,retry)) {err=ESP_ERR_INVALID_STATE;break;}
                /* Persist intent before any POST. A reboot leaves an uncertain
                 * operation requiring explicit user review, never auto-replay. */
                err=feishu_cache_pending_write((unsigned)slot,&pending,NULL);
                if(err!=ESP_OK)break;
                uint64_t sent=0;bool rejected=false;
                err=feishu_api_send_pending_part(&s_session,&pending,&sent,&rejected);
                if(err==ESP_OK) {
                    if(!feishu_outbox_ack(&pending,sent)) {err=ESP_ERR_INVALID_RESPONSE;break;}
                    err=feishu_cache_pending_write((unsigned)slot,&pending,NULL);
                } else {
                    feishu_outbox_failed(&pending,rejected);
                    (void)feishu_cache_pending_write((unsigned)slot,&pending,NULL);
                }
                xSemaphoreTake(s_state_lock,portMAX_DELAY);
                bool closing_now=s_state.closing;
                paused=s_state.cancel_send;
                xSemaphoreGive(s_state_lock);
                if((closing_now || paused) && err==ESP_OK && pending.state!=FEISHU_OUTBOX_EMPTY)err=ESP_ERR_INVALID_STATE;
                retry=false;
                if(err==ESP_OK && pending.state!=FEISHU_OUTBOX_EMPTY)vTaskDelay(pdMS_TO_TICKS(250));
            }
            /* Reload durable truth if publication after a successful POST
             * failed; an in-RAM ACK must not authorize a later automatic retry. */
            feishu_outbox_t durable;
            if(slot>=0 && feishu_cache_pending_read((unsigned)slot,&durable)==ESP_OK)pending=durable;
            xSemaphoreTake(s_state_lock,portMAX_DELAY);
            if(send_epoch!=s_owner_epoch) {
                s_state.loading=s_state.sending=s_state.recovering_operation=false;xSemaphoreGive(s_state_lock);continue;
            }
            bool replied=s_state.pending.message[0]!=0;
            s_state.loading=false;s_state.sending=false;s_state.pending_retry_warning=false;
            if(err==ESP_OK) {
                s_state.nav.page=s_state.nav.draft_manager ? FEISHU_PAGE_DRAFTS:FEISHU_PAGE_MESSAGES;s_state.pending_slot=-1;
                memset(&s_state.pending,0,sizeof(s_state.pending));
                s_state.reply[0]=s_state.reply_uuid[0]=0;
                feishu_utf8_copy(s_state.status,sizeof(s_state.status),replied ? "回复已发送":"消息已发送");
            } else {
                s_state.pending=pending;s_state.nav.page=FEISHU_PAGE_REVIEW;
                feishu_utf8_copy(s_state.status,sizeof(s_state.status),pending.state==FEISHU_OUTBOX_READY ?
                    (s_state.cancel_send ? "已暂停，已发送进度及剩余文字保留":"发送未完成，已发送进度及剩余文字保留"):
                    "发送结果未确认，请先查看飞书，重试可能重复");
            }
            xSemaphoreGive(s_state_lock);
            if(err==ESP_OK && !memory)list_pending_drafts();
        } else if (command == WORK_RESET_WIFI) {
            err=feishu_store_require_wifi_setup(true);
            if(err!=ESP_OK) {
                set_status("无法保存配网请求，请稍后重试",false);
                continue;
            }
            feishu_network_forget_wifi();
            ESP_LOGI(TAG, "Wi-Fi reset requested: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(300));
            esp_restart();
        } else if (command == WORK_UNBIND || command==WORK_REAUTHORIZE) {
            err = feishu_store_request_auth(command==WORK_UNBIND ? FEISHU_AUTH_CHANGE_APP:FEISHU_AUTH_REAUTHORIZE);
            if(err!=ESP_OK) {
                set_status("无法保存授权请求，原配置保留",false);
                continue;
            }
            ESP_LOGI(TAG, "Feishu authorization requested: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(300));
            esp_restart();
        }
        xSemaphoreTake(s_state_lock,portMAX_DELAY);
        s_state.status_error=err!=ESP_OK && err!=FEISHU_HTTP_ERR_CANCELLED;
        xSemaphoreGive(s_state_lock);
        recover_revoked_auth(err);
    }
    if(s_cache_worker) xSemaphoreTake(s_cache_done,portMAX_DELAY);
    feishu_cache_unpin(&s_chat_pin);
    feishu_cache_unpin(&s_message_pin);
    feishu_cache_unpin(&s_text_pin);
    feishu_cache_unpin(&s_image_pin);
    feishu_cache_close();
    s_worker = NULL;
    xSemaphoreGive(s_worker_done);
    vTaskDelete(NULL);
}

static void append_line(char *buffer, size_t capacity, const char *format, ...)
{
    size_t length = strlen(buffer);
    if (length >= capacity - 1) return;
    va_list args;
    va_start(args, format);
    vsnprintf(buffer + length, capacity - length, format, args);
    va_end(args);
}

static size_t visible_start(size_t selected, size_t count)
{
    if (count <= FEISHU_VISIBLE_ROWS) return 0;
    size_t start = selected >= FEISHU_VISIBLE_ROWS - 1 ?
                   selected - (FEISHU_VISIBLE_ROWS - 1) : 0;
    if (start + FEISHU_VISIBLE_ROWS > count) start = count - FEISHU_VISIBLE_ROWS;
    return start;
}

static size_t message_visible_start(size_t selected, size_t count)
{
    if (count <= FEISHU_MESSAGE_ROWS) return 0;
    size_t start = selected >= FEISHU_MESSAGE_ROWS - 1 ?
                   selected - (FEISHU_MESSAGE_ROWS - 1) : 0;
    if (start + FEISHU_MESSAGE_ROWS > count) start = count - FEISHU_MESSAGE_ROWS;
    return start;
}

static void show_list_rows(bool visible)
{
    if (visible) lv_obj_add_flag(s_content, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(s_content, LV_OBJ_FLAG_HIDDEN);
    for (size_t i = 0; i < FEISHU_VISIBLE_ROWS; ++i) {
        if (visible) lv_obj_remove_flag(s_rows[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_rows[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void style_list_row(size_t slot, bool selected)
{
    // A restrained navy surface keeps each row readable; focus is expressed
    // only by the high-contrast yellow key-navigation outline.
    lv_obj_set_style_bg_opa(s_rows[slot],
                            selected ? LV_OPA_COVER : LV_OPA_50, 0);
    lv_obj_set_style_bg_color(s_rows[slot],
        lv_color_hex(selected ? 0x142B43 : 0x102238), 0);
    lv_obj_set_style_border_width(s_rows[slot], selected ? 2 : 0, 0);
    lv_obj_set_style_border_color(s_rows[slot], lv_color_hex(UI_YELLOW), 0);
    lv_obj_set_style_radius(s_rows[slot], 10, 0);
    lv_obj_set_style_text_color(s_row_text[slot], lv_color_hex(UI_PAPER), 0);
    lv_obj_set_style_bg_color(s_row_mark[slot], lv_color_hex(UI_RED), 0);
}

static void render_chat_rows(void)
{
    static const uint32_t avatar_colors[] = {
        0x1689E8, 0x7557D9, 0x20A779, 0xD18A18,
    };
    set_label_text(s_title, "飞书 | 会话");
    lv_label_set_text_fmt(s_page_info, "%u/%u",
        s_state.nav.chat_count == 0 ? 0U : (unsigned)s_state.nav.chat_index + 1,
        (unsigned)s_state.nav.chat_count);
    size_t start = visible_start(s_state.nav.chat_index, s_state.nav.chat_count);
    for (size_t slot = 0; slot < FEISHU_VISIBLE_ROWS; ++slot) {
        lv_obj_set_pos(s_rows[slot], 10, 38 + (int)slot * 58);
        lv_obj_set_size(s_rows[slot], 220, 54);
        size_t index = start + slot;
        if (index >= s_state.nav.chat_count) {
            lv_obj_add_flag(s_rows[slot], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(s_rows[slot], LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_row_avatar[slot], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_row_avatar[slot], 5, 7);
        lv_obj_set_size(s_row_avatar[slot], 40, 40);
        lv_obj_set_style_bg_color(s_row_avatar[slot],
            lv_color_hex(avatar_colors[index % 4]), 0);
        char avatar[5];
        feishu_avatar_text(avatar, s_state.chats[index].name,
                         s_state.chats[index].p2p ? "聊" : "群");
        set_label_text(s_row_avatar_text[slot], avatar);
        lv_obj_center(s_row_avatar_text[slot]);
        lv_label_set_long_mode(s_row_text[slot],LV_LABEL_LONG_DOT);
        lv_obj_align(s_row_text[slot],LV_ALIGN_TOP_LEFT,52,17);
        lv_obj_set_size(s_row_text[slot], 142, 20);
        lv_obj_set_style_bg_opa(s_row_text[slot], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_row_text[slot], 0, 0);
        lv_obj_set_style_pad_all(s_row_text[slot], 0, 0);
        lv_obj_set_style_text_align(s_row_text[slot], LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_add_flag(s_row_subtext[slot], LV_OBJ_FLAG_HIDDEN);
        char name[61];
        feishu_utf8_copy(name, sizeof(name), s_state.chats[index].name);
        set_label_text(s_row_text[slot], name);
        set_label_text(s_row_subtext[slot], "");
        set_label_text(s_row_mark[slot], "");
        lv_obj_set_style_bg_opa(s_row_mark[slot],
            s_state.chats[index].unread_count ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        style_list_row(slot, index == s_state.nav.chat_index);
    }
}

static void render_message_rows(void)
{
    char chat_name[55];
    feishu_utf8_copy(chat_name, sizeof(chat_name),
                     s_state.chats[s_state.nav.chat_index].name);
    lv_label_set_text_fmt(s_title, "< %s", chat_name);
    size_t start = message_visible_start(s_state.nav.message_index,
                                         s_state.nav.message_count);
    for (size_t slot = 0; slot < FEISHU_MESSAGE_ROWS; ++slot) {
        lv_obj_set_pos(s_rows[slot], 10, 38 + (int)slot * 58);
        lv_obj_set_size(s_rows[slot], 220, 54);
        size_t index = start + slot;
        if (index >= s_state.nav.message_count) {
            lv_obj_add_flag(s_rows[slot], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(s_rows[slot], LV_OBJ_FLAG_HIDDEN);
        char message[91];
        feishu_text_preview(message,sizeof(message),s_state.messages[index].text,
            s_state.messages[index].text_truncated,22,2);
        lv_label_set_long_mode(s_row_text[slot],LV_LABEL_LONG_CLIP);
        set_label_text(s_row_text[slot], message);
        lv_obj_add_flag(s_row_subtext[slot], LV_OBJ_FLAG_HIDDEN);
        bool mine = s_state.messages[index].mine;
        if (mine) lv_obj_add_flag(s_row_avatar[slot], LV_OBJ_FLAG_HIDDEN);
        else {
            lv_obj_remove_flag(s_row_avatar[slot], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(s_row_avatar[slot], 4, 8);
            lv_obj_set_size(s_row_avatar[slot], 30, 30);
            lv_obj_set_style_bg_color(s_row_avatar[slot],
                                      lv_color_hex(UI_SKY_DARK), 0);
            char avatar[5];
            feishu_avatar_text(avatar, s_state.messages[index].sender_name, "他");
            set_label_text(s_row_avatar_text[slot], avatar);
        lv_obj_center(s_row_avatar_text[slot]);
        }
        lv_obj_align(s_row_text[slot],LV_ALIGN_TOP_LEFT,mine ? 44 : 40,3);
        lv_obj_set_size(s_row_text[slot], mine ? 168 : 172, 48);
        lv_obj_set_style_bg_opa(s_row_text[slot], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s_row_text[slot],
            lv_color_hex(mine ? 0x2A8B55 : 0x24384E), 0);
        lv_obj_set_style_text_color(s_row_text[slot],
            lv_color_hex(UI_PAPER), 0);
        lv_obj_set_style_text_align(s_row_text[slot],
            mine ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_pad_all(s_row_text[slot], 4, 0);
        lv_obj_set_style_text_line_space(s_row_text[slot],1,0);
        lv_obj_set_style_radius(s_row_text[slot], 8, 0);
        lv_obj_set_style_border_width(s_row_text[slot],
                                      !s_state.nav.composer_selected && index == s_state.nav.message_index ? 2 : 0, 0);
        lv_obj_set_style_border_color(s_row_text[slot],
                                      lv_color_hex(UI_YELLOW), 0);
        set_label_text(s_row_mark[slot], "");
        lv_obj_set_style_bg_opa(s_row_mark[slot], LV_OPA_TRANSP, 0);
        // The bubble itself already has the yellow focus outline. Keep the
        // surrounding message row transparent to avoid a double card.
        lv_obj_set_style_bg_opa(s_rows[slot], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_rows[slot], 0, 0);
        // style_list_row sets the default text color; restore bubble contrast.
        lv_obj_set_style_text_color(s_row_text[slot], lv_color_hex(UI_PAPER), 0);
    }
    size_t slot = FEISHU_MESSAGE_ROWS;
    lv_obj_remove_flag(s_rows[slot], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_row_avatar[slot], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_row_subtext[slot], LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_rows[slot], 18, 222);
    lv_obj_set_size(s_rows[slot], 204, 42);
    lv_obj_set_style_bg_opa(s_rows[slot], LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_rows[slot], lv_color_hex(0x102B44), 0);
    lv_obj_set_style_border_width(s_rows[slot],
                                  s_state.nav.composer_selected ? 2 : 1, 0);
    lv_obj_set_style_border_color(s_rows[slot], lv_color_hex(
        s_state.nav.composer_selected ? UI_YELLOW : 0x34506A), 0);
    lv_obj_set_style_radius(s_rows[slot], 12, 0);
    lv_obj_set_width(s_row_text[slot],188);
    lv_obj_set_height(s_row_text[slot],LV_SIZE_CONTENT);
    lv_obj_align(s_row_text[slot],LV_ALIGN_CENTER,0,0);
    lv_obj_set_style_bg_opa(s_row_text[slot], LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_row_text[slot], 0, 0);
    lv_obj_set_style_pad_all(s_row_text[slot], 0, 0);
    lv_obj_set_style_text_align(s_row_text[slot], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_row_text[slot], lv_color_hex(UI_PAPER), 0);
    lv_label_set_long_mode(s_row_text[slot],LV_LABEL_LONG_CLIP);
    set_label_text(s_row_text[slot], "发消息");
    set_label_text(s_row_mark[slot], "");
    lv_obj_set_style_bg_opa(s_row_mark[slot], LV_OPA_TRANSP, 0);
}

static void render(lv_timer_t *timer)
{
    (void)timer;
    char body[1200] = { 0 };
    char hint[96] = { 0 };
    bool detail_view=false;
    uint32_t detail_page=0;int64_t detail_id=0;int32_t detail_scroll=0;
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    uint32_t recovered=feishu_cache_recovery_generation();
    if(recovered && recovered!=s_state.recovery_generation) {
        bool busy=s_state.recovering_operation || s_state.sending || s_state.record_pending ||
            s_state.nav.page==FEISHU_PAGE_RECORDING || s_state.nav.page==FEISHU_PAGE_TRANSCRIBING;
        s_state.recovery_generation=recovered;++s_owner_epoch;++s_state.chat_page_request;++s_state.message_page_request;
        s_state.chat_page_key[0]=s_state.message_page_key[0]=s_state.loaded_chat[0]=s_state.last_messages_chat[0]=0;
        s_state.chat_page_number=s_state.message_page_number=0;s_state.chat_page_more=s_state.message_page_more=false;
        s_state.message_page_loading=false;s_state.nav.page=FEISHU_PAGE_CHATS;s_state.nav.message_count=0;
        s_state.nav.draft_manager=false;s_state.nav.draft_count=0;s_state.pending_slot=-1;
        memset(&s_state.pending,0,sizeof(s_state.pending));s_state.reply[0]=s_state.reply_uuid[0]=s_state.detail_text[0]=0;
        s_state.bookmark_dirty=false;s_state.image_ready=s_state.image_busy=false;++s_state.image_request;
        s_state.stop_recording=s_state.cancel_recording=s_state.cancel_send=true;
        s_state.sending=s_state.record_pending=false;s_state.recovering_operation=busy;s_state.loading=busy;
        feishu_utf8_copy(s_state.status,sizeof(s_state.status),busy ? "存储异常已隔离，正在停止当前操作...":"存储异常已隔离，旧草稿需恢复后查看");
        feishu_api_cancel_prefetch();
    }
    // Style setters invalidate LVGL objects even when their values are equal.
    // Avoid repainting an unchanged page every 100 ms on the single-core C3.
    // This signature is only a paint optimization, not a storage/scope check.
    static uint64_t painted;
    static uint32_t painted_generation;
    static bool paint_valid;
    bool stable=s_state.nav.page!=FEISHU_PAGE_RECORDING &&
        s_state.nav.page!=FEISHU_PAGE_TRANSCRIBING && !s_state.sending && !s_state.record_pending;
    uint64_t signature=UINT64_C(14695981039346656037);
    if(stable) {
        const unsigned char *bytes=(const unsigned char *)&s_state;
        for(size_t i=0;i<sizeof(s_state);++i)signature=(signature^bytes[i])*UINT64_C(1099511628211);
        if(paint_valid && painted_generation==s_screen_generation && painted==signature) {
            xSemaphoreGive(s_state_lock);return;
        }
    }
    paint_valid=stable;painted=signature;painted_generation=s_screen_generation;
    if(!(s_state.ready && !s_state.loading && s_state.nav.page==FEISHU_PAGE_IMAGE && s_state.image_ready))
        lv_obj_add_flag(s_image,LV_OBJ_FLAG_HIDDEN);

    if (!s_state.ready) {
        show_list_rows(false);
        set_label_text(s_title, "飞书");
        snprintf(body, sizeof(body), "\n\n%s", s_state.status);
        snprintf(hint, sizeof(hint), "%s", s_state.loading ? "请稍候" : "长按确定：返回");
    } else if (s_state.loading) {
        show_list_rows(false);
        set_label_text(s_title, "飞书");
        snprintf(body, sizeof(body), "\n\n%s", s_state.status);
        snprintf(hint, sizeof(hint), "请稍候");
    } else {
        switch (s_state.nav.page) {
        case FEISHU_PAGE_CHATS:
            show_list_rows(true);
            render_chat_rows();
            char notice[25];
            feishu_utf8_copy(notice,sizeof(notice),s_state.status[0] ? s_state.status:s_state.recovery_generation ? "存储隔离待恢复":"上下选 确定进");
            snprintf(hint,sizeof(hint),"%lu页%s %s\n长按上下翻页/确定设置",
                (unsigned long)s_state.chat_page_number+1,s_state.chat_page_more ? "+":"",notice);
            break;
        case FEISHU_PAGE_MESSAGES:
            show_list_rows(true);
            render_message_rows();
            if(s_state.message_page_more || s_state.message_page_number) {
                char notice[25];feishu_utf8_copy(notice,sizeof(notice),s_state.status[0] ? s_state.status:"上下选 确定进");
                snprintf(hint,sizeof(hint),"%lu页 %s\n长按上看更早/下看较新",(unsigned long)s_state.message_page_number+1,notice);
            } else if (s_state.status[0] != '\0') {
                char notice[49];
                feishu_utf8_copy(notice, sizeof(notice), s_state.status);
                snprintf(hint, sizeof(hint), "%s\n上下选择  确定进入",
                         notice);
            } else {
                snprintf(hint, sizeof(hint), "上下选择  确定进入  长按返回");
            }
            break;
        case FEISHU_PAGE_MESSAGE_DETAIL:
            show_list_rows(false);
            set_label_text(s_title, "消息详情");
            if (s_state.nav.message_index < s_state.nav.message_count) {
                const feishu_message_t *message =
                    &s_state.messages[s_state.nav.message_index];
                char sender[49];
                char detail[401];
                feishu_utf8_copy(sender, sizeof(sender),
                    message->mine ? "我" : message->sender_name);
                feishu_utf8_copy(detail,sizeof(detail),s_state.detail_loaded ? s_state.detail_text : message->text);
                snprintf(body, sizeof(body), "\n%s\n\n%s", sender, detail);
                if(message->type==FEISHU_MESSAGE_TEXT && message->text_truncated && !s_state.detail_loaded)
                    snprintf(hint,sizeof(hint),"仅显示预览  确定重试\n长按确定返回");
                else if(message->type==FEISHU_MESSAGE_TEXT && s_state.detail_pages>1)
                    snprintf(hint,sizeof(hint),"%lu/%lu页  上下翻页  长按快翻\n确定回复  长按确定返回",
                        (unsigned long)s_state.detail_page+1,(unsigned long)s_state.detail_pages);
                else snprintf(hint, sizeof(hint), message->type == FEISHU_MESSAGE_IMAGE ?
                         "确定查看图片  长按返回" : "上下滚动  确定回复  长按返回");
                detail_view=true;detail_page=s_state.detail_page;detail_id=message->text_object_id;
                detail_scroll=s_state.detail_scroll;
            }
            break;
        case FEISHU_PAGE_REPLY_READY:
            show_list_rows(false);
            set_label_text(s_title,
                s_state.nav.reply_to_message ? "回复消息" : "发送消息");
            if (s_state.nav.reply_to_message &&
                s_state.nav.message_index < s_state.nav.message_count) {
            char sender[49];
            char target[241];
            feishu_utf8_copy(sender, sizeof(sender),
                s_state.messages[s_state.nav.message_index].mine ? "我" :
                s_state.messages[s_state.nav.message_index].sender_name);
            feishu_utf8_copy(target, sizeof(target),
                             s_state.messages[s_state.nav.message_index].text);
            snprintf(body, sizeof(body),
                     "\n准备回复\n\n%s：\n%s\n\n按确定后开始录音",
                     sender, target);
            } else {
                snprintf(body, sizeof(body),
                         "\n向当前会话发送消息\n\n按确定后开始录音\n识别文字后可确认发送");
            }
            snprintf(hint, sizeof(hint), "确定开始录音  长按返回");
            break;
        case FEISHU_PAGE_IMAGE:
            show_list_rows(false);
            set_label_text(s_title, "图片消息");
            if (s_state.image_ready) {
                lv_obj_add_flag(s_content, LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(s_image, LV_OBJ_FLAG_HIDDEN);
                if (s_displayed_image_version != s_state.image_version) {
                    char path[48];
                    if(feishu_image_fs_select(&s_state.image,path)==ESP_OK)lv_image_set_src(s_image,path);
                    else {
                        s_state.image_ready=false;lv_obj_add_flag(s_image,LV_OBJ_FLAG_HIDDEN);
                        lv_obj_remove_flag(s_content,LV_OBJ_FLAG_HIDDEN);
                        feishu_utf8_copy(s_state.status,sizeof(s_state.status),"预览暂不可显示，请重试");
                        snprintf(body,sizeof(body),"%s",s_state.status);
                    }
                    s_displayed_image_version=s_state.image_version;
                }
                if(s_state.image.approximate)snprintf(hint,sizeof(hint),"低清 %ux %s\n%s",
                    1u<<(unsigned)s_state.image.info.zoom,s_state.image_horizontal ? "上左 下右":"上移 下移",
                    s_state.image_busy ? "正在补充细节 长按返回":"确定操作 长按上下缩放");
                else if(s_state.image_busy)snprintf(hint,sizeof(hint),"正在更新预览...\n长按确定可停止");
                else if(s_state.status[0] && strcmp(s_state.status,"缓存预览")) {
                    char image_notice[64];feishu_utf8_copy(image_notice,sizeof(image_notice),s_state.status);
                    snprintf(hint,sizeof(hint),"%s\n确定操作 长按返回",image_notice);
                }
                else if(s_state.image.info.zoom)snprintf(hint,sizeof(hint),"局部 %ux  %s\n确定操作 长按上下缩放",
                    1u<<(unsigned)s_state.image.info.zoom,s_state.image_horizontal ? "上左 下右":"上移 下移");
                else snprintf(hint,sizeof(hint),"完整预览 长按上键放大\n确定操作 长按确定返回");
            } else {
                snprintf(body, sizeof(body), "\n\n%s", s_state.status);
                snprintf(hint,sizeof(hint),"确定操作  长按返回");
            }
            break;
        case FEISHU_PAGE_IMAGE_ACTIONS: {
            show_list_rows(false);set_label_text(s_title,"图片操作");
            static const char *labels[]={"回复此图","放大","缩小","切换移动方向","重试预览","返回图片"};
            append_line(body,sizeof(body),"图片操作\n\n");
            for(size_t i=0;i<6;++i)append_line(body,sizeof(body),"%s %s\n",s_state.nav.settings_index==i ? ">":" ",labels[i]);
            snprintf(hint,sizeof(hint),"上下选择  确定  长按返回");
            break;
        }
        case FEISHU_PAGE_RECORDING:
            show_list_rows(false);
            set_label_text(s_title,
                s_state.nav.reply_to_message ? "回复消息" : "发送消息");
            if (s_state.nav.reply_to_message &&
                s_state.nav.message_index < s_state.nav.message_count) {
            char target[121];
            feishu_utf8_copy(target, sizeof(target),
                             s_state.messages[s_state.nav.message_index].text);
              snprintf(body, sizeof(body), "\n%s\n\n回复：%s\n\n%u.%u 秒\n\n%s",
                       s_state.record_pending ? "正在准备录音":"正在录音",target,
                       s_state.elapsed_ms / 1000, (s_state.elapsed_ms % 1000) / 100,
                       s_state.record_pending ? "请稍候":"请说话...");
            } else {
                  snprintf(body, sizeof(body), "\n%s\n\n%u.%u 秒\n\n%s",
                           s_state.record_pending ? "正在准备录音":"正在录音",
                         s_state.elapsed_ms / 1000,
                           (s_state.elapsed_ms % 1000) / 100,s_state.record_pending ? "请稍候":"请说话...");
            }
            snprintf(hint, sizeof(hint), "确定停止  长按取消");
            break;
        case FEISHU_PAGE_TRANSCRIBING:
            show_list_rows(false);
            snprintf(body, sizeof(body), "\n\n正在识别语音...\n\n请稍候");
            snprintf(hint, sizeof(hint), "长按取消");
            break;
        case FEISHU_PAGE_REVIEW:
            show_list_rows(false);
            set_label_text(s_title,s_state.pending_slot==RAM_REPLY_SLOT ? "确认消息":"审阅草稿");
            char destination[96]={0};
            if(s_state.nav.draft_manager)snprintf(destination,sizeof(destination),"目标：%s\n",s_state.pending.chat);
            snprintf(body, sizeof(body), "%s%s\n\n%s%s%s",destination,
                     s_state.nav.reply_to_message ? "确认回复" : "确认发送",
                     s_state.status[0] ? s_state.status : "",
                       s_state.status[0] ? "\n\n" : "",s_state.detail_loaded ? s_state.detail_text:s_state.reply);
            if(s_state.detail_pages>1)snprintf(hint,sizeof(hint),"%lu/%lu页  上下翻页\n确定操作  长按确定保留返回",
                (unsigned long)s_state.detail_page+1,(unsigned long)s_state.detail_pages);
            else if(s_state.pending_slot==RAM_REPLY_SLOT)snprintf(hint,sizeof(hint),
                s_state.detail_scroll_max>0 ? "上下滚动 确定操作\n长按确定取消返回":"确定发送 上重录 下取消\n长按确定取消返回");
            else snprintf(hint,sizeof(hint),"确定发送 上重录 下取消\n长按确定保留返回");
            detail_view=true;detail_page=s_state.detail_page;detail_id=s_state.pending.object_id;
            detail_scroll=s_state.detail_scroll;
            break;
        case FEISHU_PAGE_DRAFT_ACTIONS:
            show_list_rows(false);
            set_label_text(s_title,"草稿操作");
            snprintf(body,sizeof(body),"确认处理草稿\n\n%s%s%s  %s\n\n%s  重录\n\n%s  取消草稿",
                s_state.pending_retry_warning ? "上次结果未确认，重试可能重复\n请先查看飞书\n\n":"",
                s_state.pending.offset ? "已发送部分内容，保留剩余进度\n\n":"",
                s_state.nav.settings_index==0 ? ">":" ",s_state.pending_retry_warning ? "仍要重试":"发送全文（必要时分段）",
                s_state.nav.settings_index==1 ? ">":" ",s_state.nav.settings_index==2 ? ">":" ");
            snprintf(hint,sizeof(hint),"上下选择  确定  长按返回审阅");
            break;
        case FEISHU_PAGE_STATUS:
            show_list_rows(false);
            snprintf(body, sizeof(body), "\n\n%s", s_state.status);
            snprintf(hint, sizeof(hint), "长按确定：返回消息");
            break;
        case FEISHU_PAGE_DRAFTS:
            show_list_rows(false);set_label_text(s_title,"全部待发送草稿");
            if(s_state.recovery_generation)append_line(body,sizeof(body),"故障数据已保留\n旧草稿需恢复后查看\n\n");
            if(!s_state.nav.draft_count)append_line(body,sizeof(body),"\n没有待发送草稿\n");
            size_t draft_start=s_state.nav.draft_index>1 ? s_state.nav.draft_index-1:0;
            if(s_state.nav.draft_count)append_line(body,sizeof(body),"%u/%u\n\n",
                (unsigned)s_state.nav.draft_index+1,(unsigned)s_state.nav.draft_count);
            for(size_t i=draft_start;i<s_state.nav.draft_count && i<draft_start+2;++i)append_line(body,sizeof(body),"%s %s\n\n",
                i==s_state.nav.draft_index ? ">":" ",s_state.draft_labels[i]);
            if(s_state.status[0])append_line(body,sizeof(body),"%s",s_state.status);
            snprintf(hint,sizeof(hint),"上下选择 确定审阅\n长按确定返回会话");
            break;
        case FEISHU_PAGE_SETTINGS:
            show_list_rows(false);
            append_line(body, sizeof(body), "设置\n\n");
            if(s_state.recovery_generation)append_line(body,sizeof(body),"故障数据已保留，旧草稿待恢复\n\n");
            append_line(body, sizeof(body), "%s 重新配置 Wi-Fi\n\n",
                        s_state.nav.settings_index == 0 ? ">" : " ");
            append_line(body, sizeof(body), "%s 更换应用/账号\n\n",
                        s_state.nav.settings_index == 1 ? ">" : " ");
            append_line(body, sizeof(body), "%s 全部待发送草稿\n\n",
                        s_state.nav.settings_index == 2 ? ">" : " ");
            append_line(body, sizeof(body), "%s 重新授权当前应用\n\n",
                        s_state.nav.settings_index == 3 ? ">" : " ");
            append_line(body,sizeof(body),"%s 返回会话\n",s_state.nav.settings_index==4 ? ">":" ");
            snprintf(hint, sizeof(hint), "上下选择  确定  长按返回");
            break;
        case FEISHU_PAGE_CONFIRM:
            show_list_rows(false);
            snprintf(body, sizeof(body), "\n确认操作\n\n%s\n\n此操作完成后设备会重启",
                     s_state.nav.confirm_action == 1 ?
                     "重新配置 Wi-Fi？\n飞书绑定会保留" :
                     s_state.nav.confirm_action==2 ? "更换应用或账号？\n完成前保留原绑定及草稿" :
                     "重新授权当前应用？\n应用和 Wi-Fi 会保留");
            snprintf(hint, sizeof(hint), "确定继续  上/下取消");
            break;
        default:
            break;
        }
    }
    char footer_notice[96],footer_hint[96];
    feishu_utf8_copy(footer_notice,sizeof(footer_notice),s_state.status);
    bool notice_error=s_state.status_error;
    if(!footer_notice[0])feishu_utf8_copy(footer_notice,sizeof(footer_notice),
        s_state.nav.page==FEISHU_PAGE_MESSAGES ? "消息已加载":s_state.nav.page==FEISHU_PAGE_CHATS ? "会话已加载":"就绪");
    switch(s_state.nav.page) {
    case FEISHU_PAGE_CHATS:
        if(s_state.chat_page_more)snprintf(footer_hint,sizeof(footer_hint),"%lu/?页  长按上下翻页  长按确定设置",(unsigned long)s_state.chat_page_number+1);
        else snprintf(footer_hint,sizeof(footer_hint),"%lu/%lu页  长按上下翻页  长按确定设置",(unsigned long)s_state.chat_page_number+1,(unsigned long)s_state.chat_page_number+1);
        break;
    case FEISHU_PAGE_MESSAGES:
        if(s_state.message_page_more)snprintf(footer_hint,sizeof(footer_hint),"%lu/?页  长按上下翻页  长按确定返回",(unsigned long)s_state.message_page_number+1);
        else snprintf(footer_hint,sizeof(footer_hint),"%lu/%lu页  长按上下翻页  长按确定返回",(unsigned long)s_state.message_page_number+1,(unsigned long)s_state.message_page_number+1);
        break;
    case FEISHU_PAGE_MESSAGE_DETAIL:case FEISHU_PAGE_REVIEW:
        snprintf(footer_hint,sizeof(footer_hint),"%lu/%lu页  长按上下翻页  长按确定返回",(unsigned long)s_state.detail_page+1,(unsigned long)s_state.detail_pages);break;
    case FEISHU_PAGE_REPLY_READY:snprintf(footer_hint,sizeof(footer_hint),"确定开始录音 / 长按返回");break;
    case FEISHU_PAGE_RECORDING:snprintf(footer_hint,sizeof(footer_hint),"确定结束录音 / 长按取消");break;
    default:feishu_text_preview(footer_hint,sizeof(footer_hint),hint,false,32,1);break;
    }
    if(s_state.nav.page==FEISHU_PAGE_RECORDING && !s_state.status[0])feishu_utf8_copy(footer_notice,sizeof(footer_notice),"录音中");
    char battery[8];
    if (s_state.battery_percent >= 0) {
        unsigned percent = (unsigned)(s_state.battery_percent > 100 ?
                                      100 : s_state.battery_percent);
        snprintf(battery, sizeof(battery), "%u%%", percent);
    } else {
        snprintf(battery, sizeof(battery), "--%%");
    }
    set_label_text(s_page_info, battery);
    xSemaphoreGive(s_state_lock);
    if (!lv_obj_has_flag(s_content, LV_OBJ_FLAG_HIDDEN)) {
        set_label_text(s_content, body);
        lv_obj_update_layout(s_content);
        int32_t maximum=lv_obj_get_height(s_content)-FEISHU_CONTENT_HEIGHT;
        if(maximum<0)maximum=0;
        if(!detail_view)detail_scroll=0;
        if(detail_scroll<0)detail_scroll=0;
        if(detail_scroll>maximum)detail_scroll=maximum;
        lv_obj_set_y(s_content,-detail_scroll);
        if(detail_view) {
            xSemaphoreTake(s_state_lock,portMAX_DELAY);
            bool reading=s_state.nav.page==FEISHU_PAGE_MESSAGE_DETAIL && s_state.nav.message_index<s_state.nav.message_count &&
                s_state.messages[s_state.nav.message_index].text_object_id==detail_id;
            bool reviewing=s_state.nav.page==FEISHU_PAGE_REVIEW && s_state.pending.object_id==detail_id;
            if((reading || reviewing) && s_state.detail_page==detail_page) {
                s_state.detail_scroll_max=maximum;
                if(s_state.detail_scroll!=detail_scroll) {s_state.detail_scroll=detail_scroll;if(reading)bookmark_locked();}
            }
            xSemaphoreGive(s_state_lock);
        }
    }
    char notice_line[96];
    feishu_text_preview(notice_line,sizeof(notice_line),footer_notice,false,32,1);
    set_label_text(s_notice,notice_line);
    lv_obj_set_style_text_color(s_notice,lv_color_hex(notice_error ? 0xFF9B91:0xE1EDF4),0);
    set_label_text(s_hint,footer_hint);
}

static void enqueue(work_command_t command)
{
    bool image=command==WORK_LOAD_IMAGE || command==WORK_FETCH_IMAGE;
    if(image && s_state_lock) {
        xSemaphoreTake(s_state_lock,portMAX_DELAY);
        bool *queued=command==WORK_LOAD_IMAGE ? &s_state.image_cache_queued:&s_state.image_fetch_queued;
        if(*queued) {xSemaphoreGive(s_state_lock);return;}
        *queued=true;xSemaphoreGive(s_state_lock);
    }
    QueueHandle_t queue=(command==WORK_LOAD_MESSAGES || command==WORK_LOAD_TEXT_PAGE || command==WORK_LOAD_DRAFT ||
        command==WORK_LOAD_DRAFT_PAGE || command==WORK_LIST_DRAFTS || command==WORK_OPEN_DRAFT || command==WORK_CHAT_PAGE || command==WORK_MESSAGE_PAGE || command==WORK_LOAD_IMAGE) && s_cache_worker ? s_cache_queue : s_work_queue;
    if(queue && xQueueSend(queue,&command,0)==pdTRUE) return;
    if(s_state_lock) {
        xSemaphoreTake(s_state_lock,portMAX_DELAY);
        s_state.loading=false;
        if(command==WORK_SEND)s_state.sending=false;
        if(command==WORK_RECORD) {
            s_state.record_pending=false;s_state.stop_recording=s_state.cancel_recording=true;
            s_state.nav.page=s_state.record_replaces_pending && s_state.pending_slot>=0 ? FEISHU_PAGE_REVIEW:FEISHU_PAGE_REPLY_READY;
        }
        if(image) {
            s_state.image_busy=false;
            if(command==WORK_LOAD_IMAGE)s_state.image_cache_queued=false;else s_state.image_fetch_queued=false;
        }
        feishu_utf8_copy(s_state.status,sizeof(s_state.status),"操作繁忙，请稍后重试");
        xSemaphoreGive(s_state_lock);
    }
}

static bool move_detail(int direction,bool fast)
{
    bool load=false;
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    bool reviewing=s_state.nav.page==FEISHU_PAGE_REVIEW && (s_state.detail_pages>1 ||
        (s_state.pending_slot==RAM_REPLY_SLOT && s_state.detail_scroll_max>0));
    if(s_state.nav.page!=FEISHU_PAGE_MESSAGE_DETAIL && !reviewing) {xSemaphoreGive(s_state_lock);return false;}
    if(s_state.loading) {xSemaphoreGive(s_state_lock);return true;}
    uint32_t pages=s_state.detail_pages ? s_state.detail_pages : 1;
    if(!fast && direction>0 && s_state.detail_scroll<s_state.detail_scroll_max) {
        s_state.detail_scroll+=210;if(s_state.detail_scroll>s_state.detail_scroll_max)s_state.detail_scroll=s_state.detail_scroll_max;
    } else if(!fast && direction<0 && s_state.detail_scroll>0) {
        s_state.detail_scroll-=210;if(s_state.detail_scroll<0)s_state.detail_scroll=0;
    } else {
        uint32_t next=s_state.detail_page;
        unsigned step=fast ? 10 : 1;
        if(direction>0)next=pages-1-next<step ? pages-1 : next+step;
        else next=next<step ? 0 : next-step;
        if(next!=s_state.detail_page) {
            s_state.detail_page=next;s_state.detail_scroll=!fast && direction<0 ? INT32_MAX : 0;
            s_state.detail_scroll_max=0;s_state.detail_restore=false;s_state.detail_loaded=false;s_state.loading=true;
            feishu_utf8_copy(s_state.status,sizeof(s_state.status),"正在读取正文...");load=true;
        } else if(fast)s_state.detail_scroll=direction>0 ? s_state.detail_scroll_max : 0;
    }
    if(!load && !reviewing)bookmark_locked();
    xSemaphoreGive(s_state_lock);
    if(load)enqueue(reviewing ? WORK_LOAD_DRAFT_PAGE:WORK_LOAD_TEXT_PAGE);
    return true;
}

static bool change_image(int direction,int zoom,bool axis,bool retry)
{
    bool queue=false;
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    if(!s_state.ready || s_state.nav.page!=FEISHU_PAGE_IMAGE) {xSemaphoreGive(s_state_lock);return false;}
    if(axis) {s_state.image_horizontal=!s_state.image_horizontal;s_state.status[0]=0;xSemaphoreGive(s_state_lock);return true;}
    if(!retry && !s_state.image_ready) {xSemaphoreGive(s_state_lock);return true;}
    if(zoom) {
        uint32_t maximum=feishu_jpeg_max_zoom(s_state.image.info.original_width,s_state.image.info.original_height);
        uint32_t next=s_state.image_view.zoom;
        if(zoom>0 && next<maximum)++next;else if(zoom<0 && next)--next;
        if(next==s_state.image_view.zoom) {xSemaphoreGive(s_state_lock);return true;}
        s_state.image_view.zoom=next;
    }
    if(direction) {
        bool horizontal=s_state.image_horizontal;
        int32_t *center=horizontal ? &s_state.image_view.center_x:&s_state.image_view.center_y;
        uint32_t crop=horizontal ? s_state.image.info.crop_width:s_state.image.info.crop_height;
        uint32_t original=horizontal ? s_state.image.info.original_width:s_state.image.info.original_height;
        int32_t low=(int32_t)(crop/2),high=(int32_t)(original-(crop-crop/2));
        int64_t next=(int64_t)*center+direction*(int64_t)(crop>2 ? crop/2:1);
        if(next<low)next=low;
        if(next>high)next=high;
        if(next==*center) {xSemaphoreGive(s_state_lock);return true;}
        *center=(int32_t)next;
    }
    queue=true;s_state.image_busy=true;++s_state.image_request;
    ESP_LOGI(TAG,"preview request: z%u center %ld,%ld request %u",(unsigned)s_state.image_view.zoom,
        (long)s_state.image_view.center_x,(long)s_state.image_view.center_y,(unsigned)s_state.image_request);
    feishu_utf8_copy(s_state.status,sizeof(s_state.status),"正在更新预览...");
    xSemaphoreGive(s_state_lock);
    feishu_api_cancel_prefetch();if(queue)enqueue(WORK_LOAD_IMAGE);return true;
}

static void handle_navigation(feishu_nav_event_t event)
{
    if((event==FEISHU_NAV_UP || event==FEISHU_NAV_DOWN) && move_detail(event==FEISHU_NAV_UP ? -1 : 1,false))return;
    if((event==FEISHU_NAV_UP || event==FEISHU_NAV_DOWN) && change_image(event==FEISHU_NAV_UP ? -1:1,0,false,false))return;
    feishu_nav_action_t action;bool schedule_image=false;
    feishu_page_t previous_page;
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    if(s_state.ready && s_state.loading && s_state.nav.page==FEISHU_PAGE_REPLY_READY && event==FEISHU_NAV_BACK) {
        s_state.loading=false;s_state.nav.page=FEISHU_PAGE_MESSAGES;
        feishu_utf8_copy(s_state.status,sizeof(s_state.status),"已取消读取草稿");
        xSemaphoreGive(s_state_lock);return;
    }
    if (!s_state.ready || s_state.loading) {
        xSemaphoreGive(s_state_lock);
        return;
    }
    previous_page = s_state.nav.page;
    s_state.status_error=false;
    if(previous_page==FEISHU_PAGE_REVIEW && event==FEISHU_NAV_BACK) {
        bool manager=s_state.nav.draft_manager;
        s_state.nav.page=manager ? FEISHU_PAGE_DRAFTS:FEISHU_PAGE_MESSAGES;s_state.pending_retry_warning=false;
        if(s_state.pending_slot==RAM_REPLY_SLOT) {
            s_state.pending_slot=-1;memset(&s_state.pending,0,sizeof(s_state.pending));
            s_state.reply[0]=s_state.reply_uuid[0]=s_state.detail_text[0]=0;
            feishu_utf8_copy(s_state.status,sizeof(s_state.status),"已取消内存中的待发消息");
        } else feishu_utf8_copy(s_state.status,sizeof(s_state.status),"草稿已保留，进入回复可继续");
        xSemaphoreGive(s_state_lock);if(manager)enqueue(WORK_LIST_DRAFTS);return;
    }
    if(previous_page==FEISHU_PAGE_REVIEW && (s_state.detail_pages>1 ||
       (s_state.pending_slot==RAM_REPLY_SLOT && s_state.detail_scroll_max>0)) && event==FEISHU_NAV_OK) {
        s_state.nav.page=FEISHU_PAGE_DRAFT_ACTIONS;s_state.nav.settings_index=0;
        xSemaphoreGive(s_state_lock);return;
    }
    if(previous_page==FEISHU_PAGE_RECORDING && s_state.record_pending && event==FEISHU_NAV_OK) {
        s_state.stop_recording=true;s_state.cancel_recording=true;s_state.nav.page=s_state.nav.draft_manager ? FEISHU_PAGE_DRAFTS:FEISHU_PAGE_MESSAGES;
        feishu_utf8_copy(s_state.status,sizeof(s_state.status),"录音尚未开始，已取消");
        xSemaphoreGive(s_state_lock);return;
    }
    if(previous_page==FEISHU_PAGE_MESSAGE_DETAIL && event==FEISHU_NAV_OK &&
       s_state.nav.message_index<s_state.nav.message_count &&
       s_state.messages[s_state.nav.message_index].type==FEISHU_MESSAGE_TEXT &&
       s_state.messages[s_state.nav.message_index].text_truncated && !s_state.detail_loaded) {
        s_state.loading=true;s_state.detail_refetch_attempted=true;
        xSemaphoreGive(s_state_lock);
        enqueue(online() ? WORK_FETCH_TEXT : WORK_LOAD_TEXT_PAGE);
        return;
    }
    if (s_state.nav.page == FEISHU_PAGE_MESSAGE_DETAIL &&
        event == FEISHU_NAV_OK &&
        s_state.nav.message_index < s_state.nav.message_count &&
        s_state.messages[s_state.nav.message_index].type == FEISHU_MESSAGE_IMAGE) {
        s_state.nav.page = FEISHU_PAGE_IMAGE;
        s_state.loading = false;
        s_state.image_ready = false;
        s_state.image_horizontal=false;s_state.image_view=(feishu_image_view_t){0,-1,-1};
        feishu_utf8_copy(s_state.status, sizeof(s_state.status), "正在准备图片预览...");
        action = FEISHU_NAV_LOAD_IMAGE;
    } else {
        action = feishu_nav_handle(&s_state.nav, event);
    }
    if(!online() && (action==FEISHU_NAV_START_RECORDING ||
       action==FEISHU_NAV_RETRY_RECORDING || action==FEISHU_NAV_SEND_REPLY)) {
        s_state.loading=false;
        if(action==FEISHU_NAV_SEND_REPLY && (s_state.pending_slot==RAM_REPLY_SLOT ||
           (s_state.pending_slot>=0 && feishu_outbox_valid(&s_state.pending)))) {
            s_state.nav.page=FEISHU_PAGE_REVIEW;s_state.status_error=true;
            feishu_utf8_copy(s_state.status,sizeof(s_state.status),s_state.pending_slot==RAM_REPLY_SLOT ?
                "当前离线，内容留在内存，请联网后重试":"当前离线，草稿已保存，请联网后重试");
        } else {
            s_state.nav.page=FEISHU_PAGE_STATUS;
            feishu_utf8_copy(s_state.status,sizeof(s_state.status),"离线缓存可读，请联网后重试此操作");
        }
        action=FEISHU_NAV_NONE;
    }
    if(action==FEISHU_NAV_SEND_REPLY && s_state.pending_slot!=RAM_REPLY_SLOT && (s_state.pending_slot<0 ||
       !feishu_outbox_valid(&s_state.pending) || s_state.pending.state==FEISHU_OUTBOX_EMPTY)) {
        feishu_utf8_copy(s_state.status,sizeof(s_state.status),"请返回后重新读取草稿状态");action=FEISHU_NAV_NONE;
    }
    if(action==FEISHU_NAV_SEND_REPLY && s_state.pending.state!=FEISHU_OUTBOX_READY && !s_state.pending_retry_warning) {
        s_state.pending_retry_warning=true;
        feishu_utf8_copy(s_state.status,sizeof(s_state.status),"上次结果未确认，请先查看飞书。再次确定可能重复发送");
        xSemaphoreGive(s_state_lock);return;
    }
    if(previous_page==FEISHU_PAGE_DRAFT_ACTIONS && (event==FEISHU_NAV_UP || event==FEISHU_NAV_DOWN))s_state.pending_retry_warning=false;
    bool read_pending=previous_page!=FEISHU_PAGE_REPLY_READY && s_state.nav.page==FEISHU_PAGE_REPLY_READY;
    if(s_state.nav.draft_manager && s_state.nav.page==FEISHU_PAGE_MESSAGES &&
       (action==FEISHU_NAV_CANCEL_REPLY || previous_page==FEISHU_PAGE_STATUS))s_state.nav.page=FEISHU_PAGE_DRAFTS;
    if(action==FEISHU_NAV_LIST_DRAFTS || action==FEISHU_NAV_OPEN_DRAFT) {
        s_state.loading=true;feishu_utf8_copy(s_state.status,sizeof(s_state.status),"正在读取草稿...");
    }
    if(read_pending) {
        s_state.nav.draft_manager=false;
        /* Ordinary recording must not wait for a disposable database. Existing
         * durable drafts remain reachable through the separate draft manager. */
        read_pending=false;s_state.pending_slot=-1;s_state.pending_retry_warning=false;
        s_state.loading=false;s_state.status[0]=0;
    }
    bool entered_detail=previous_page!=FEISHU_PAGE_MESSAGE_DETAIL && s_state.nav.page==FEISHU_PAGE_MESSAGE_DETAIL;
    if(entered_detail) {
        s_state.detail_page=0;s_state.detail_pages=1;s_state.detail_scroll=s_state.detail_scroll_max=0;
        s_state.detail_loaded=false;s_state.detail_restore=true;s_state.detail_refetch_attempted=false;
    }
    bool read_detail=entered_detail &&
        s_state.nav.message_index<s_state.nav.message_count &&
        s_state.messages[s_state.nav.message_index].type==FEISHU_MESSAGE_TEXT;
    if(read_detail) {
        s_state.loading=true;feishu_utf8_copy(s_state.status,sizeof(s_state.status),"正在读取正文...");
    }
    if(action==FEISHU_NAV_LOAD_IMAGE) {
        schedule_image=true;s_state.image_busy=true;++s_state.image_request;
        s_state.loading=false;feishu_utf8_copy(s_state.status,sizeof(s_state.status),"正在更新预览...");
    }
    bool leaving_image=(previous_page==FEISHU_PAGE_IMAGE || previous_page==FEISHU_PAGE_IMAGE_ACTIONS) &&
        s_state.nav.page!=FEISHU_PAGE_IMAGE && s_state.nav.page!=FEISHU_PAGE_IMAGE_ACTIONS;
    if(leaving_image) {
        s_state.image_busy=false;++s_state.image_request;
    }
    if (action == FEISHU_NAV_LOAD_MESSAGES) {
        ++s_state.message_page_request;s_state.message_page_loading=false;
        s_state.loading = true;
        feishu_utf8_copy(s_state.status, sizeof(s_state.status), "正在加载消息...");
    } else if (action == FEISHU_NAV_START_RECORDING ||
               action == FEISHU_NAV_RETRY_RECORDING) {
        s_state.stop_recording = false;
        s_state.cancel_recording = false;
        s_state.record_pending=true;s_state.record_replaces_pending=action==FEISHU_NAV_RETRY_RECORDING;
        s_state.elapsed_ms = 0;
        s_state.status[0] = '\0';
        memset(s_state.reply_uuid, 0, sizeof(s_state.reply_uuid));
    } else if (action == FEISHU_NAV_STOP_RECORDING) {
        s_state.stop_recording = true;
    } else if (action == FEISHU_NAV_CANCEL_REPLY) {
        s_state.stop_recording = true;
        s_state.cancel_recording = true;
        if (previous_page == FEISHU_PAGE_RECORDING ||
            previous_page == FEISHU_PAGE_TRANSCRIBING) {
            s_state.loading = true;
            feishu_utf8_copy(s_state.status, sizeof(s_state.status),
                             "正在停止录音...");
        } else if(s_state.pending_slot>=0 && (previous_page==FEISHU_PAGE_REVIEW || previous_page==FEISHU_PAGE_DRAFT_ACTIONS)) {
            s_state.loading=true;feishu_utf8_copy(s_state.status,sizeof(s_state.status),"正在取消草稿...");
        }
        memset(s_state.reply, 0, sizeof(s_state.reply));
    } else if (action == FEISHU_NAV_SEND_REPLY) {
        s_state.cancel_send=false;s_state.sending=true;
        if (s_state.reply_uuid[0] == '\0') make_reply_uuid(s_state.reply_uuid);
        s_state.loading = true;
        feishu_utf8_copy(s_state.status, sizeof(s_state.status),
                         s_state.nav.reply_to_message ?
                         "正在发送回复..." : "正在发送消息...");
    }
    xSemaphoreGive(s_state_lock);
    if(read_pending || read_detail || action==FEISHU_NAV_START_RECORDING || action==FEISHU_NAV_RETRY_RECORDING ||
       action==FEISHU_NAV_LOAD_IMAGE || leaving_image)
        feishu_api_cancel_prefetch();
    if(read_detail)enqueue(WORK_LOAD_TEXT_PAGE);
    if(read_pending)enqueue(WORK_LOAD_DRAFT);
    if(action==FEISHU_NAV_LIST_DRAFTS || (previous_page==FEISHU_PAGE_STATUS && event==FEISHU_NAV_BACK))enqueue(WORK_LIST_DRAFTS);
    if(action==FEISHU_NAV_OPEN_DRAFT)enqueue(WORK_OPEN_DRAFT);
    if(action==FEISHU_NAV_CANCEL_REPLY && (previous_page==FEISHU_PAGE_REVIEW || previous_page==FEISHU_PAGE_DRAFT_ACTIONS))enqueue(WORK_DISCARD_DRAFT);

    if (action == FEISHU_NAV_LOAD_MESSAGES) enqueue(WORK_LOAD_MESSAGES);
    if (action == FEISHU_NAV_LOAD_IMAGE && schedule_image) enqueue(WORK_LOAD_IMAGE);
    if(action==FEISHU_NAV_IMAGE_ZOOM_IN || action==FEISHU_NAV_IMAGE_ZOOM_OUT || action==FEISHU_NAV_IMAGE_AXIS)
        change_image(0,action==FEISHU_NAV_IMAGE_ZOOM_IN ? 1:action==FEISHU_NAV_IMAGE_ZOOM_OUT ? -1:0,action==FEISHU_NAV_IMAGE_AXIS,false);
    if (action == FEISHU_NAV_START_RECORDING || action == FEISHU_NAV_RETRY_RECORDING) enqueue(WORK_RECORD);
    if (action == FEISHU_NAV_SEND_REPLY) enqueue(WORK_SEND);
    if (action == FEISHU_NAV_RESET_WIFI) enqueue(WORK_RESET_WIFI);
    if (action == FEISHU_NAV_UNBIND) enqueue(WORK_UNBIND);
    if (action == FEISHU_NAV_REAUTHORIZE) enqueue(WORK_REAUTHORIZE);
}

void demo_feishu_enter(void)
{
    ++s_screen_generation;
    memset(&s_state, 0, sizeof(s_state));
    s_state.pending_slot=-1;
    memset(&s_session, 0, sizeof(s_session));
    s_authenticated=false;
    s_displayed_image_version=UINT32_MAX;
    s_chat_pin=s_message_pin=s_text_pin=s_image_pin=0;
    s_cache_app[0]=s_cache_owner[0]='\0';
    s_owner_epoch=0;
    s_state.nav.page = FEISHU_PAGE_CHATS;
    s_state.battery_percent = -1;
    feishu_utf8_copy(s_state.status, sizeof(s_state.status), "正在启动...");
    s_state.loading = true;

    s_state_lock = xSemaphoreCreateMutex();
    s_worker_done = xSemaphoreCreateBinary();
    s_work_queue = xQueueCreate(4, sizeof(work_command_t));
    feishu_cache_init();

    s_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(UI_INK), 0);
    lv_obj_set_style_border_width(s_scr, 0, 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);
    s_title = lv_label_create(s_scr);
    set_label_text(s_title, "飞书");
    lv_obj_set_width(s_title, 168);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(s_title, &lv_font_ai_passport_14, 0);
    lv_obj_set_style_text_color(s_title, lv_color_hex(UI_PAPER), 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_LEFT, 12, 10);
    s_page_info = lv_label_create(s_scr);
    lv_obj_set_width(s_page_info, 42);
    lv_obj_set_style_text_font(s_page_info, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(s_page_info, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(s_page_info, lv_color_hex(UI_MUTED), 0);
    lv_obj_align(s_page_info, LV_ALIGN_TOP_RIGHT, -10, 10);
    s_header_rule = lv_obj_create(s_scr);
    lv_obj_remove_flag(s_header_rule, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_header_rule, 10, 34);
    lv_obj_set_size(s_header_rule, 220, 1);
    lv_obj_set_style_border_width(s_header_rule, 0, 0);
    lv_obj_set_style_pad_all(s_header_rule, 0, 0);
    lv_obj_set_style_bg_color(s_header_rule, lv_color_hex(0x54738C), 0);
    lv_obj_set_style_bg_opa(s_header_rule, LV_OPA_COVER, 0);
    s_viewport=lv_obj_create(s_scr);
    lv_obj_set_size(s_viewport,216,FEISHU_CONTENT_HEIGHT);lv_obj_set_pos(s_viewport,12,38);
    lv_obj_remove_flag(s_viewport,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_viewport,LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_style_bg_opa(s_viewport,LV_OPA_TRANSP,0);
    lv_obj_set_style_border_width(s_viewport,0,0);lv_obj_set_style_pad_all(s_viewport,0,0);
    lv_obj_set_style_radius(s_viewport,0,0);lv_obj_set_style_shadow_width(s_viewport,0,0);
    s_content = lv_label_create(s_viewport);
    lv_obj_set_size(s_content,216,LV_SIZE_CONTENT);
    lv_obj_set_pos(s_content,0,0);
    lv_obj_set_style_text_font(s_content, &lv_font_ai_passport_14, 0);
    lv_obj_set_style_text_color(s_content, lv_color_hex(UI_PAPER), 0);
    lv_label_set_long_mode(s_content, LV_LABEL_LONG_WRAP);
    s_image = lv_image_create(s_scr);
    lv_obj_set_pos(s_image, 12, 38);
    lv_obj_set_size(s_image, 216, 226);
    lv_image_set_inner_align(s_image,LV_IMAGE_ALIGN_CENTER);
    lv_obj_add_flag(s_image, LV_OBJ_FLAG_HIDDEN);
    for (size_t i = 0; i < FEISHU_VISIBLE_ROWS; ++i) {
        s_rows[i] = lv_obj_create(s_scr);
        lv_obj_remove_flag(s_rows[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(s_rows[i], 10, 38 + (int)i * 58);
        lv_obj_set_size(s_rows[i], 220, 54);
        lv_obj_set_style_pad_all(s_rows[i], 0, 0);
        lv_obj_set_style_radius(s_rows[i], 2, 0);
        s_row_avatar[i] = lv_obj_create(s_rows[i]);
        lv_obj_remove_flag(s_row_avatar[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(s_row_avatar[i], 5, 7);
        lv_obj_set_size(s_row_avatar[i], 40, 40);
        lv_obj_set_style_radius(s_row_avatar[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(s_row_avatar[i], 0, 0);
        lv_obj_set_style_pad_all(s_row_avatar[i], 0, 0);
        s_row_avatar_text[i] = lv_label_create(s_row_avatar[i]);
        lv_obj_set_style_text_font(s_row_avatar_text[i],
                                   &lv_font_ai_passport_14, 0);
        lv_obj_set_style_text_color(s_row_avatar_text[i],
                                    lv_color_hex(UI_PAPER), 0);
        lv_obj_center(s_row_avatar_text[i]);
        s_row_text[i] = lv_label_create(s_rows[i]);
        lv_obj_set_pos(s_row_text[i], 8, 5);
        lv_obj_set_width(s_row_text[i], 184);
        lv_label_set_long_mode(s_row_text[i], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(s_row_text[i], &lv_font_ai_passport_14, 0);
        lv_obj_set_style_text_line_space(s_row_text[i], 3, 0);
        s_row_subtext[i] = lv_label_create(s_rows[i]);
        lv_obj_set_style_text_font(s_row_subtext[i], &lv_font_ai_passport_14, 0);
        lv_obj_set_style_text_color(s_row_subtext[i], lv_color_hex(0x94AFC5), 0);
        lv_label_set_long_mode(s_row_subtext[i], LV_LABEL_LONG_DOT);
        lv_obj_add_flag(s_row_subtext[i], LV_OBJ_FLAG_HIDDEN);
        s_row_mark[i] = lv_label_create(s_rows[i]);
        lv_obj_set_size(s_row_mark[i], 9, 9);
        lv_obj_align(s_row_mark[i], LV_ALIGN_RIGHT_MID, -8, 0);
        lv_obj_set_style_radius(s_row_mark[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s_row_mark[i], lv_color_hex(UI_RED), 0);
        lv_obj_set_style_bg_opa(s_row_mark[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_font(s_row_mark[i], &lv_font_ai_passport_14, 0);
        lv_obj_set_style_text_align(s_row_mark[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_add_flag(s_rows[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_t *footer=lv_obj_create(s_scr);
    lv_obj_set_pos(footer,10,272);lv_obj_set_size(footer,220,48);
    lv_obj_remove_flag(footer,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(footer,lv_color_hex(0x24445B),0);
    lv_obj_set_style_bg_opa(footer,LV_OPA_TRANSP,0);
    lv_obj_set_style_border_color(footer,lv_color_hex(0x54738C),0);
    lv_obj_set_style_border_width(footer,1,0);
    lv_obj_set_style_border_side(footer,LV_BORDER_SIDE_TOP,0);
    lv_obj_set_style_radius(footer,0,0);lv_obj_set_style_pad_all(footer,0,0);
    s_notice=lv_label_create(footer);s_hint=lv_label_create(footer);
    lv_obj_t *labels[]={s_notice,s_hint};
    for(unsigned i=0;i<2;++i) {
        lv_obj_set_pos(labels[i],3,i ? 26:4);lv_obj_set_size(labels[i],212,18);
        lv_obj_set_style_text_font(labels[i],&lv_font_ai_passport_14,0);
        lv_obj_set_style_text_align(labels[i],LV_TEXT_ALIGN_CENTER,0);
        lv_obj_set_style_text_color(labels[i],lv_color_hex(0xB9CFE0),0);
        lv_label_set_long_mode(labels[i],LV_LABEL_LONG_CLIP);
    }
    lv_obj_set_style_text_letter_space(s_hint,-1,0);
    lv_screen_load(s_scr);

    if (s_state_lock == NULL || s_worker_done == NULL || s_work_queue == NULL) {
        s_state.loading = false;
        feishu_utf8_copy(s_state.status, sizeof(s_state.status), "Not enough memory");
        set_label_text(s_content, "\n\n内存不足，请重启设备");
        set_label_text(s_hint, "长按确定：返回");
        return;
    }

    s_timer = lv_timer_create(render, 100, NULL);
    // Message parsing and HTTPS each have non-trivial fixed stack peaks. Image
    // resource keys also enlarge the bounded message arrays kept on this task.
    // Message parsing needs more than the original 10 KiB stack, while a
    // 16 KiB stack leaves too little contiguous heap for Feishu's unusually
    // long OAuth Authorization header plus TLS buffers on an ESP32-C3.
    if (xTaskCreate(worker_task, "feishu", 12288, NULL, 4, &s_worker) != pdPASS) {
        s_state.loading = false;
        feishu_utf8_copy(s_state.status, sizeof(s_state.status), "内存不足，请重启设备");
        return;
    }
    enqueue(WORK_INIT);
}

void demo_feishu_exit(void)
{
    feishu_api_cancel_prefetch();
    if(s_state_lock) {
        xSemaphoreTake(s_state_lock,portMAX_DELAY);
        s_state.closing=true;
        s_state.stop_recording=true;
        s_state.cancel_recording=true;
        xSemaphoreGive(s_state_lock);
    }
    if (s_worker != NULL) {
        enqueue(WORK_EXIT);
        /* Network requests have bounded timeouts. Never free a live worker's
         * queues, mutexes or SQLite connection underneath an in-flight call. */
        xSemaphoreTake(s_worker_done,portMAX_DELAY);
    }
    feishu_network_stop();
    if (s_timer != NULL) { lv_timer_delete(s_timer); s_timer = NULL; }
    if (s_scr != NULL) { lv_obj_delete(s_scr); s_scr = NULL; }
    feishu_cache_unmount();
    if (s_work_queue != NULL) { vQueueDelete(s_work_queue); s_work_queue = NULL; }
    if (s_cache_queue != NULL) { vQueueDelete(s_cache_queue); s_cache_queue = NULL; }
    if (s_cache_done != NULL) { vSemaphoreDelete(s_cache_done); s_cache_done = NULL; }
    if (s_worker_done != NULL) { vSemaphoreDelete(s_worker_done); s_worker_done = NULL; }
    if (s_state_lock != NULL) { vSemaphoreDelete(s_state_lock); s_state_lock = NULL; }
    memset(&s_session, 0, sizeof(s_session));
}

void demo_feishu_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if(ev==BSP_BTN_LONG && (btn==BSP_BTN_UP || btn==BSP_BTN_DOWN)) {
        if(change_image(0,btn==BSP_BTN_UP ? 1:-1,false,false))return;
        xSemaphoreTake(s_state_lock,portMAX_DELAY);
        bool message_page=s_state.ready && !s_state.loading && s_state.nav.page==FEISHU_PAGE_MESSAGES &&
            (btn==BSP_BTN_UP ? s_state.message_page_more:s_state.message_page_number>0);
        if(message_page) {
            s_state.message_page_direction=btn==BSP_BTN_UP ? 1:-1;++s_state.message_page_request;
            s_state.loading=s_state.message_page_loading=true;
            feishu_utf8_copy(s_state.status,sizeof(s_state.status),"正在读取历史消息...");
        }
        xSemaphoreGive(s_state_lock);
        if(message_page) {feishu_api_cancel_prefetch();enqueue(WORK_MESSAGE_PAGE);return;}
        xSemaphoreTake(s_state_lock,portMAX_DELAY);
        bool page=s_state.ready && !s_state.loading && s_state.nav.page==FEISHU_PAGE_CHATS;
        if(page) {
            bool possible=btn==BSP_BTN_UP ? s_state.chat_page_number>0:s_state.chat_page_more;
            if(possible) {
                s_state.chat_page_direction=btn==BSP_BTN_UP ? -1:1;++s_state.chat_page_request;s_state.loading=true;
                feishu_utf8_copy(s_state.status,sizeof(s_state.status),"正在读取列表页...");
            } else page=false;
        }
        xSemaphoreGive(s_state_lock);
        if(page) {feishu_api_cancel_prefetch();enqueue(WORK_CHAT_PAGE);return;}
        move_detail(btn==BSP_BTN_UP ? -1 : 1,true);return;
    }
    if (ev != BSP_BTN_CLICK) return;
    if (btn == BSP_BTN_UP) handle_navigation(FEISHU_NAV_UP);
    if (btn == BSP_BTN_DOWN) handle_navigation(FEISHU_NAV_DOWN);
    if (btn == BSP_BTN_OK) handle_navigation(FEISHU_NAV_OK);
}

bool demo_feishu_back(void)
{
    bool consume = true;
    bool ready;
    bool loading;
    if (s_state_lock == NULL) return false;
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    ready = s_state.ready;
    loading = s_state.loading;
    bool cancel_read=false;
    if(ready && loading && s_state.recovering_operation) {
        feishu_utf8_copy(s_state.status,sizeof(s_state.status),"正在停止当前操作，故障数据已保留...");
    } else if(ready && loading && s_state.nav.page==FEISHU_PAGE_CHATS) {
        ++s_state.chat_page_request;s_state.loading=false;cancel_read=true;
        feishu_utf8_copy(s_state.status,sizeof(s_state.status),"已停止翻页，原列表保留");
    } else if(ready && loading && s_state.nav.page==FEISHU_PAGE_MESSAGES && s_state.message_page_loading) {
        ++s_state.message_page_request;s_state.loading=s_state.message_page_loading=false;cancel_read=true;
        feishu_utf8_copy(s_state.status,sizeof(s_state.status),"已停止翻页，原消息保留");
    } else if(ready && loading && (s_state.nav.page==FEISHU_PAGE_MESSAGES || s_state.nav.page==FEISHU_PAGE_MESSAGE_DETAIL)) {
        s_state.nav.page=s_state.nav.page==FEISHU_PAGE_MESSAGES ? FEISHU_PAGE_CHATS:FEISHU_PAGE_MESSAGES;
        s_state.loading=false;cancel_read=true;
        feishu_utf8_copy(s_state.status,sizeof(s_state.status),"已停止加载");
    } else if(ready && loading && s_state.nav.page==FEISHU_PAGE_REPLY_READY) {
        s_state.loading=false;s_state.nav.page=FEISHU_PAGE_MESSAGES;cancel_read=true;
        feishu_utf8_copy(s_state.status,sizeof(s_state.status),"已取消读取草稿");
    } else if(ready && loading && s_state.nav.page==FEISHU_PAGE_REVIEW && !s_state.sending) {
        s_state.nav.page=s_state.nav.draft_manager ? FEISHU_PAGE_DRAFTS:FEISHU_PAGE_MESSAGES;s_state.loading=false;cancel_read=true;
    } else if(ready && loading && s_state.sending) {
        s_state.cancel_send=true;
        feishu_utf8_copy(s_state.status,sizeof(s_state.status),"将在本段确认后暂停，剩余文字会保留");
    } else if (!ready) {
        consume = loading;
    } else if (loading) {
        consume = true;
    }
    xSemaphoreGive(s_state_lock);
    if(cancel_read) {feishu_api_cancel_prefetch();return true;}
    if (consume && ready && !loading) {
        handle_navigation(FEISHU_NAV_BACK);
    }
    return consume;
}

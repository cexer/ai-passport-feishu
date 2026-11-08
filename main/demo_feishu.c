// Direct-to-Feishu messenger demo. Network and ASR work never runs in the
// button/LVGL task; revision-checked voice or physical OK confirms sends.
#include "demo.h"
#include "feishu_boot.h"
#include "feishu_power.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "bsp_audio.h"
#include "feishu_api.h"
#include "feishu_asr.h"
#include "feishu_network.h"
#include "feishu_store.h"
#include "feishu_cache.h"
#include "feishu_text_view.h"
#include "feishu_image_fs.h"
#include "feishu_http.h"
#include "ui_pixel.h"
#include "feishu_theme.h"
#include "feishu_startup.h"
#include "feishu_ui_chrome.h"
#include "feishu_pairing_policy.h"
#include "feishu_xiao_console_qr.h"

#include "esp_log.h"
#include "esp_app_desc.h"
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
#include "feishu_assistant.h"
#include "feishu_assistant_policy.h"
#include "feishu_compose.h"
#include "cJSON.h"

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
static esp_err_t reserve_audio_resources(void);
static void release_audio_resources(void);
static void assistant_enter(void);
static bool s_focus_audio_reserved;

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
    WORK_POWER_APPLY,WORK_POWER_SAVE,WORK_FRONT_REFRESH,
    WORK_CHAT_PAGE,
    WORK_FETCH_CHAT_PAGE,
    WORK_MESSAGE_PAGE,
    WORK_FETCH_MESSAGE_PAGE,
    WORK_DISCARD_DRAFT,
    WORK_CLEAR_DRAFTS,
    WORK_LOAD_IMAGE,
    WORK_FETCH_IMAGE,
    WORK_AUDIO_FOCUS,
    WORK_RECORD,
    WORK_SEND,
    WORK_RESET_WIFI,
    WORK_UNBIND,
    WORK_REAUTHORIZE,
    WORK_CHECK_XIAOZHI,
    WORK_ASSISTANT,
    WORK_ASSISTANT_SAVE,
    WORK_FEEDBACK,WORK_SOUND_SAVE,
    WORK_NETWORK_LIST,
    WORK_NETWORK_SCAN,
    WORK_NETWORK_CONNECT,
    WORK_NETWORK_DISCONNECT,
    WORK_NETWORK_REMOVE,
    WORK_NETWORK_EDIT,
    WORK_NETWORK_PASSWORD_SAVE,
#ifdef FEISHU_NETWORK_UI_AUTOTEST
    WORK_NETWORK_AUTOTEST,
#endif
    WORK_EXIT,
} work_command_t;
static void enqueue(work_command_t command);
static void feedback_request(feishu_sound_t kind);
static void process_work_command(work_command_t command) __attribute__((noinline));
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
    bool startup_pending;
    bool closing;
    volatile bool cancel_recording;
    bool assistant_active,assistant_requested,assistant_settings_editing;
    feishu_assistant_phase_t assistant_phase;bool assistant_memory_retried;
    bool assistant_exit_requested,assistant_enabled,assistant_greeted;
    bool assistant_timed_out;uint32_t assistant_activity_ms;
    bool assistant_composing,compose_busy,compose_send_confirm;
    volatile bool assistant_speech_detected;
    bool reply_from_assistant;
    unsigned assistant_send_notice;
    bool assistant_boot_checked;
    feishu_assistant_settings_t assistant_settings;
    feishu_sound_settings_t sounds;
    char assistant_heard[192],assistant_reply[384];
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
    feishu_asr_pairing_t pairing;
    volatile bool cancel_pairing;
    bool pairing_busy,pairing_auto_blocked;
    uint32_t pairing_retry_at;
    feishu_page_t pairing_return_page;
    uint32_t pairing_request;
} feishu_ui_state_t;

static feishu_message_t *s_sent_echo;
static char s_sent_echo_chat[FEISHU_CHAT_ID_MAX];
static uint32_t s_sent_echo_epoch;
static TickType_t s_sent_echo_expires;
static lv_obj_t *s_scr;
static lv_obj_t *s_startup_cover,*s_startup_logo;
static uint32_t s_screen_generation;
static lv_obj_t *s_title;
static lv_obj_t *s_assistant_badge;
static lv_obj_t *s_page_info;
static lv_obj_t *s_content;
static lv_obj_t *s_viewport;
static lv_obj_t *s_image;
static lv_obj_t *s_pair_qr;
static lv_obj_t *s_pair_caption;
static lv_obj_t *s_hint;
static lv_obj_t *s_hint_groups[2];
static lv_obj_t *s_notice;
static lv_obj_t *s_notice_dots;
#define FEISHU_VISIBLE_ROWS FEISHU_CHAT_ROWS
#define FEISHU_CONTENT_TOP 46
#define FEISHU_CONTENT_HEIGHT 218
static lv_obj_t *s_rows[FEISHU_VISIBLE_ROWS];
static lv_obj_t *s_row_avatar[FEISHU_VISIBLE_ROWS];
static lv_obj_t *s_row_avatar_text[FEISHU_VISIBLE_ROWS];
static lv_obj_t *s_row_text[FEISHU_VISIBLE_ROWS];
static lv_obj_t *s_row_subtext[FEISHU_VISIBLE_ROWS];
static lv_obj_t *s_row_mark[FEISHU_VISIBLE_ROWS];
static lv_timer_t *s_timer;
static bool s_notice_listening;
#ifdef FEISHU_SCENARIO_AUTOTEST
static atomic_uint s_scenario_cancel_at,s_scenario_back_count;
#endif

#ifdef FEISHU_LVGL_RECOVERY_AUTOTEST
static atomic_uint s_glyph_inject,s_glyph_failures,s_ui_heartbeat;
bool feishu_lvgl_fail_next_glyph(void)
{
    if(!atomic_exchange(&s_glyph_inject,0))return false;
    atomic_fetch_add(&s_glyph_failures,1);return true;
}
#endif

#ifdef FEISHU_UI_AUTOTEST
static lv_timer_t *s_ui_smoke_timer;
#endif
static QueueHandle_t s_work_queue;
static SemaphoreHandle_t s_state_lock;
static SemaphoreHandle_t s_worker_done;
static TaskHandle_t s_worker;
static TaskHandle_t s_cache_worker;
static QueueHandle_t s_cache_queue;
static SemaphoreHandle_t s_cache_done;
static feishu_ui_state_t s_state;
/* Capture must not inherit the display task's slow paint mutex. */
static atomic_uint s_assistant_submit_seconds=2;
static atomic_bool s_power_queued,s_front_queued;
static feishu_api_session_t s_session;
static char s_cache_app[FEISHU_APP_ID_MAX];
static char s_cache_owner[FEISHU_CHAT_ID_MAX];
static int64_t s_chat_pin, s_message_pin,s_text_pin;
static int64_t s_image_pin;
static atomic_bool s_authenticated;
static atomic_bool s_audio_focus_queued;
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

/* Only the business worker starts/joins audio. UI cancellation is atomic. */
static void sync_audio_focus(void)
{
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    bool recording=s_state.nav.page==FEISHU_PAGE_RECORDING || s_state.nav.page==FEISHU_PAGE_TRANSCRIBING || s_state.assistant_active || s_state.assistant_requested;
    bool wanted=s_state.ready && !s_state.closing && !s_state.loading &&
        s_state.nav.page==FEISHU_PAGE_MESSAGES && s_state.nav.composer_selected && online();
    xSemaphoreGive(s_state_lock);
    if(recording)return;
    if(!wanted)feishu_asr_prepare_cancel();
    if(atomic_exchange(&s_audio_focus_queued,true))return;
    work_command_t command=WORK_AUDIO_FOCUS;
    if(!s_work_queue || xQueueSend(s_work_queue,&command,0)!=pdTRUE)
        atomic_store(&s_audio_focus_queued,false); /* optional, never block input */
}

static void apply_audio_focus(void)
{
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    bool recording=s_state.nav.page==FEISHU_PAGE_RECORDING || s_state.nav.page==FEISHU_PAGE_TRANSCRIBING || s_state.assistant_active || s_state.assistant_requested;
    bool wanted=s_state.ready && !s_state.closing && !s_state.loading &&
        s_state.nav.page==FEISHU_PAGE_MESSAGES && s_state.nav.composer_selected && online();
    xSemaphoreGive(s_state_lock);
    if(recording)return;
    if(wanted) {
        if(feishu_asr_prepare_expired()){
            (void)feishu_asr_prepare(true); /* Reap the retired owner; keep timeout latched. */
            if(s_focus_audio_reserved)release_audio_resources();
            return;
        }
        if(!s_focus_audio_reserved) {
            if(reserve_audio_resources()!=ESP_OK)return;
            s_focus_audio_reserved=true;
        }
        if(feishu_asr_prepare(true)!=ESP_OK) {
            (void)feishu_asr_prepare(false);
            release_audio_resources();
        }
    } else {
        (void)feishu_asr_prepare(false);
        if(s_focus_audio_reserved)release_audio_resources();
    }
}

#define FEISHU_MESSAGE_ROWS 5
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
    if(s_state.nav.page==FEISHU_PAGE_CHATS && !s_state.loading && !s_state.nav.chat_count)
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
    TickType_t load_started=xTaskGetTickCount();
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
        feishu_api_message_preview_page(&s_session,chat_id,"",messages,FEISHU_MAX_MESSAGES,&frame->page);
    bool saved=false;
    if(err==ESP_OK && !cached) {
        count=frame->page.count;feishu_utf8_copy(frame->chat,sizeof(frame->chat),chat_id);
        /* Publish the received page to the UI before cache/seen writes. */
        framed=true;
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
    if(!cached && !strcmp(s_state.loaded_chat,chat_id)){
        unsigned reused=0;
        for(size_t i=0;i<count;++i)for(size_t j=0;j<s_state.nav.message_count;++j)
            if(feishu_message_restore_body(&messages[i],&s_state.messages[j])){++reused;break;}
        if(reused)ESP_LOGI(TAG,"MESSAGE_BODY_CACHE reused=%u",reused);
    }
    memcpy(s_state.messages, messages, sizeof(s_state.messages));
    size_t display_count=count;
    if(s_sent_echo && (s_sent_echo_epoch!=s_owner_epoch || (int32_t)(xTaskGetTickCount()-s_sent_echo_expires)>=0)){
        free(s_sent_echo);s_sent_echo=NULL;
    }
    if(s_sent_echo && !strcmp(s_sent_echo_chat,chat_id)){
        bool present=feishu_message_merge_ack(s_state.messages,&display_count,FEISHU_MAX_MESSAGES,s_sent_echo);
        if(present && !cached){free(s_sent_echo);s_sent_echo=NULL;}
    }
    feishu_utf8_copy(s_state.loaded_chat,sizeof(s_state.loaded_chat),chat_id);
    s_state.nav.message_count = display_count;
    s_state.loading=false;
    s_state.status[0]=0;
    if(!ram_hit) {
        s_state.message_page_key[0]=0;s_state.message_page_number=0;s_state.message_page_more=false;
        if(framed) {
            feishu_utf8_copy(s_state.message_page_key,sizeof(s_state.message_page_key),page_key);
            s_state.message_page_number=frame->page.number;s_state.message_page_more=frame->page.more;
        }
    }
    s_state.nav.message_index = display_count == 0 ? 0 : display_count - 1;
    if (!followed_latest && selected_id[0] != '\0') {
        for (size_t i = 0; i < display_count; ++i) {
            if (strcmp(s_state.messages[i].message_id, selected_id) == 0) {
                s_state.nav.message_index = i;
                break;
            }
        }
    }
    s_state.chats[chat_index].latest_time_ms = latest;
    if (display_count > 0) {
        feishu_utf8_copy(s_state.chats[chat_index].summary,
                         sizeof(s_state.chats[chat_index].summary),
                         s_state.messages[display_count - 1].text);
    }
    if (mark_seen) s_state.chats[chat_index].unread_count = 0;
    }
    xSemaphoreGive(s_state_lock);
    if(applied)sync_audio_focus();
    if(applied && prefer_cache)feedback_request(FEISHU_SOUND_SUCCESS);
    ESP_LOGI(TAG,"MESSAGE_READY cache=%u ram=%u applied=%u rows=%u ms=%u",
        (unsigned)cached,(unsigned)ram_hit,(unsigned)applied,(unsigned)count,
        (unsigned)((xTaskGetTickCount()-load_started)*portTICK_PERIOD_MS));
    if(!cached) {
        saved=feishu_cache_write_if_scope(scope,page_key,frame,sizeof(*frame),1)==ESP_OK;
        xSemaphoreTake(s_state_lock,portMAX_DELAY);
        bool still_visible=applied && epoch==s_owner_epoch && request==s_state.message_page_request &&
            s_state.nav.page==FEISHU_PAGE_MESSAGES && s_state.nav.chat_index==chat_index;
        xSemaphoreGive(s_state_lock);
        if(still_visible && saved)feishu_cache_protect(page_key,&s_message_pin);
        if(saved) {
            xSemaphoreTake(s_state_lock,portMAX_DELAY);
            feishu_utf8_copy(s_state.last_messages_chat,sizeof(s_state.last_messages_chat),chat_id);
            s_state.last_messages_tick=xTaskGetTickCount();
            xSemaphoreGive(s_state_lock);
        }
    }
    if (applied && mark_seen && latest > 0) feishu_store_mark_seen(chat_id, latest);
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

/* Cheap active-page polling: one preview, zero cache writes if unchanged.
 * Publish a new row before reconciling the full page, so storage cannot delay it. */
static esp_err_t poll_visible_messages(void)
{
    char chat[FEISHU_CHAT_ID_MAX];uint32_t epoch,request;size_t index;
    static char reconciled_chat[FEISHU_CHAT_ID_MAX];static TickType_t reconciled_at;
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    bool eligible=s_state.ready && !s_state.loading && s_state.nav.page==FEISHU_PAGE_MESSAGES &&
        s_state.message_page_number==0 && !s_state.message_page_loading && s_state.nav.chat_index<s_state.nav.chat_count;
    if(!eligible){xSemaphoreGive(s_state_lock);return ESP_OK;}
    index=s_state.nav.chat_index;epoch=s_owner_epoch;request=s_state.message_page_request;
    feishu_utf8_copy(chat,sizeof(chat),s_state.chats[index].chat_id);
    if(!strcmp(s_state.last_messages_chat,chat) &&
       xTaskGetTickCount()-s_state.last_messages_tick<pdMS_TO_TICKS(30000)){
        feishu_utf8_copy(reconciled_chat,sizeof(reconciled_chat),chat);reconciled_at=s_state.last_messages_tick;
    }
    xSemaphoreGive(s_state_lock);
    feishu_message_t *head=calloc(1,sizeof(*head));if(!head)return ESP_ERR_NO_MEM;
    bool present=false;esp_err_t err=feishu_api_message_preview(&s_session,chat,head,&present);
    bool changed=false,current=false;
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    current=epoch==s_owner_epoch && request==s_state.message_page_request && s_state.nav.page==FEISHU_PAGE_MESSAGES &&
        s_state.nav.chat_index==index && !s_state.message_page_number && !strcmp(s_state.chats[index].chat_id,chat);
    if(current && err==ESP_OK && present){
        bool follow=s_state.nav.composer_selected || !s_state.nav.message_count || s_state.nav.message_index+1==s_state.nav.message_count;
        changed=feishu_message_receive(s_state.messages,&s_state.nav.message_count,FEISHU_MAX_MESSAGES,
                                      &s_state.nav.message_index,follow,head);
        if(changed){s_state.chats[index].latest_time_ms=head->create_time_ms;s_state.chats[index].unread_count=0;}
    }
    xSemaphoreGive(s_state_lock);free(head);
    if(err!=ESP_OK || !current)return err;
    TickType_t now=xTaskGetTickCount();
    bool due=strcmp(reconciled_chat,chat) || now-reconciled_at>=pdMS_TO_TICKS(30000);
    if(changed || due){
        if(changed)ESP_LOGI(TAG,"MESSAGE_PREVIEW applied=1 before_reconcile=1");
        feishu_utf8_copy(reconciled_chat,sizeof(reconciled_chat),chat);reconciled_at=now;
        return load_messages_for_chat(index,true,false);
    }
    return ESP_OK;
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
    bool light=true; /* Conversation rows need previews, not full history bodies. */
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
    if(light)messages=calloc(1,sizeof(*messages));
    else {message_frame=calloc(1,sizeof(*message_frame));messages=message_frame ? message_frame->messages:NULL;}
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
        if(light){
            bool present=false;memset(messages,0,sizeof(*messages));
            err=feishu_api_message_preview(&s_session,chat.chat_id,messages,&present);message_count=present ? 1:0;
        } else {
            memset(message_frame,0,sizeof(*message_frame));
            err=feishu_api_message_preview_page(&s_session,chat.chat_id,"",messages,FEISHU_MAX_MESSAGES,&message_frame->page);
            message_count=message_frame->page.count;
        }
        if (err == FEISHU_ERR_AUTH_REVOKED) goto done;
        if (err != ESP_OK) continue;
        chat.latest_time_ms = feishu_latest_visible_time(messages, message_count);
        uint64_t baseline = baselines[i];
        if (baseline == 0 && chat.latest_time_ms > 0) {
            // The first successful sync establishes a baseline. Historical
            // messages must not all appear as new on a newly bound device.
            feishu_store_mark_seen(chat.chat_id, chat.latest_time_ms);
            chat.unread_count = 0;
        } else if(light){
            if(chat.latest_time_ms<=baseline)chat.unread_count=0;
            else if(message_count && !messages[0].mine && !chat.unread_count)chat.unread_count=1;
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
        if(light)continue; /* Preview only: full history stays on demand. */
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
    if(light)free(messages);
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
        s_state.loading=false;s_state.status_error=!applied;
        feishu_utf8_copy(s_state.status,sizeof(s_state.status),applied ? (hit ? "缓存列表":""):
            (feishu_network_failure_hint() ? feishu_network_failure_hint():"此页暂不可读，原列表保留，可返回或重试"));
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
            err=feishu_api_message_preview_page(&s_session,chat,destination->token,frame->messages,FEISHU_MAX_MESSAGES,&frame->page);
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
        s_state.status_error=!applied;
        feishu_utf8_copy(s_state.status,sizeof(s_state.status),applied ? (hit ? "缓存历史":""):
            (feishu_network_failure_hint() ? feishu_network_failure_hint():"历史暂不可读，原消息保留，可返回或重试"));
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
            s_state.startup_pending=false;s_state.status[0]=0;
            if(!s_state.nav.chat_count && !s_state.chat_page_key[0] && s_state.nav.page==FEISHU_PAGE_CHATS) {
                memcpy(s_state.chats,frame->chats,sizeof(s_state.chats));
                s_state.nav.chat_count=count;s_state.nav.chat_index=0;
                if(framed) {
                    feishu_utf8_copy(s_state.chat_page_key,sizeof(s_state.chat_page_key),page_key);
                    s_state.chat_page_number=0;s_state.chat_page_more=frame->page.more;
                }
            }
            xSemaphoreGive(s_state_lock);cached=true;
            ESP_LOGI(TAG,"STARTUP_READY source=cache");
        }
        free(frame);
    }
    if(s_cache_owner[0]) {
        int64_t begin=esp_timer_get_time();unsigned unreadable=0;
        for(unsigned i=0;i<FEISHU_OUTBOX_SLOTS;++i) {
            feishu_outbox_t pending;esp_err_t read=feishu_cache_pending_read(i,&pending);
            if(read!=ESP_OK && read!=ESP_ERR_NOT_FOUND)++unreadable;
        }
        ESP_LOGI(TAG,"draft index empty=%u unreadable=%u elapsed_ms=%u",(unsigned)feishu_cache_pending_empty(),unreadable,(unsigned)((esp_timer_get_time()-begin)/1000));
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
        set_status("BOT凭据失效，请在设置重新授权",false);return;
    }
    if (err != FEISHU_ERR_AUTH_REVOKED) return;
    // Keep the owner app and Wi-Fi; onboarding can request a new QR.
    esp_err_t clear_err = feishu_store_request_auth(FEISHU_AUTH_REAUTHORIZE);
    if(clear_err==ESP_OK)clear_err=feishu_store_clear_user_tokens();
    if (clear_err == ESP_OK) {
        feishu_boot_authorization_entry();
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
    int64_t began=esp_timer_get_time();
    bool notify=false;
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
    if(!full && !message.text_truncated && !message.text_needs_mapping) {total=strlen(message.text);err=ESP_OK;}
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
        notify=true;s_state.status_error=err!=ESP_OK;
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
    ESP_LOGI(TAG,"TEXT_PAGE page=%u cached=%u result=%d ms=%u",(unsigned)page,(unsigned)full,err,(unsigned)((esp_timer_get_time()-began)/1000));
    if(notify)feedback_request(err==ESP_OK ? FEISHU_SOUND_SUCCESS:FEISHU_SOUND_FAILURE);
}

static void load_draft_page(void)
{
    feishu_outbox_t p;uint32_t page,epoch;
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    if(s_state.nav.page==FEISHU_PAGE_REVIEW && s_state.pending_slot==RAM_REPLY_SLOT) {
        feishu_mentions_display(s_state.reply,s_state.detail_text,sizeof(s_state.detail_text));
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
        s_state.loading=false;s_state.detail_loaded=err==ESP_OK;s_state.detail_pages=pages;s_state.status_error=err!=ESP_OK;
        if(err==ESP_OK) {
            feishu_mentions_display(output,s_state.detail_text,sizeof(s_state.detail_text));
            if(!strcmp(s_state.status,"正在读取正文..."))s_state.status[0]=0;
        }
        else feishu_utf8_copy(s_state.status,sizeof(s_state.status),"草稿正文暂不可读，可取消后重录");
    }
    xSemaphoreGive(s_state_lock);
}
/* Caller holds the UI-state lock. No flash access from button callbacks. */
static void apply_pending_draft(int slot,const feishu_outbox_t *p)
{
    s_state.compose_send_confirm=false;
    if(slot!=RAM_REPLY_SLOT && strcmp(s_state.pending.uuid,p->uuid))s_state.reply_from_assistant=false;
    s_state.pending_slot=slot;s_state.pending_retry_warning=false;s_state.pending=*p;
    s_state.nav.reply_to_message=p->message[0]!=0;s_state.nav.page=FEISHU_PAGE_REVIEW;
    s_state.detail_page=0;s_state.detail_scroll=s_state.detail_scroll_max=0;s_state.detail_loaded=false;
    s_state.detail_pages=(uint32_t)((p->length+FEISHU_TEXT_PAGE_STEP-1)/FEISHU_TEXT_PAGE_STEP);
    if(slot==RAM_REPLY_SLOT){
        s_state.detail_pages=1;s_state.detail_loaded=true;s_state.loading=false;
        feishu_mentions_display(s_state.reply,s_state.detail_text,sizeof(s_state.detail_text));
        feishu_utf8_copy(s_state.reply_uuid,sizeof(s_state.reply_uuid),p->uuid);
        feishu_utf8_copy(s_state.status,sizeof(s_state.status),"仅在内存，断电丢失；可确认发送");return;
    }
    s_state.reply[0]=0;s_state.loading=true;
    feishu_utf8_copy(s_state.status,sizeof(s_state.status),p->state==FEISHU_OUTBOX_READY ?
        "已恢复待确认草稿":"上次发送结果未确认，重试可能重复");
}
static void list_pending_drafts(void)
{
    uint32_t epoch;
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    if(s_state.nav.page!=FEISHU_PAGE_DRAFTS) {xSemaphoreGive(s_state_lock);return;}
    if(s_state.pending_slot==RAM_REPLY_SLOT && s_state.reply[0]){
        const char *name=s_state.pending.chat;
        for(size_t i=0;i<s_state.nav.chat_count;++i)if(!strcmp(name,s_state.chats[i].chat_id)){name=s_state.chats[i].name;break;}
        s_state.draft_slots[0]=FEISHU_OUTBOX_SLOTS;s_state.nav.draft_count=1;s_state.nav.draft_index=0;s_state.loading=false;
        snprintf(s_state.draft_labels[0],sizeof(s_state.draft_labels[0]),"%.63s\n未保存 · 断电丢失",name);
        xSemaphoreGive(s_state_lock);return;
    }
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
    slot=s_state.draft_slots[index];epoch=s_owner_epoch;
    if(slot==FEISHU_OUTBOX_SLOTS && s_state.pending_slot==RAM_REPLY_SLOT){
        apply_pending_draft(RAM_REPLY_SLOT,&s_state.pending);xSemaphoreGive(s_state_lock);return;
    }
    xSemaphoreGive(s_state_lock);
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
static void network_work(work_command_t command);
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
        bool draft_busy=detail || s_state.sending || s_state.assistant_active ||
            s_state.nav.page==FEISHU_PAGE_RECORDING || s_state.nav.page==FEISHU_PAGE_TRANSCRIBING;
        bool image=(s_state.nav.page==FEISHU_PAGE_IMAGE || s_state.nav.page==FEISHU_PAGE_IMAGE_ACTIONS) && s_state.image_ready;
        int64_t release_image=0;
        if(!image) {release_image=s_image_pin;s_image_pin=0;}
        size_t index=s_state.nav.chat_index;
        xSemaphoreGive(s_state_lock);
        if(closing) {feishu_cache_unpin(&release_image);save_read_position();break;}
        if(!detail)feishu_cache_unpin(&s_text_pin);
        if(!draft_busy && !feishu_cache_pending_empty())feishu_cache_drafts_idle();
        feishu_cache_unpin(&release_image);
        if(xQueueReceive(s_cache_queue,&command,pdMS_TO_TICKS(100))!=pdTRUE) {
            if(xTaskGetTickCount()-last_position_save>=pdMS_TO_TICKS(2000)) {
                save_read_position();last_position_save=xTaskGetTickCount();
            }
            continue;
        }
        if(command==WORK_EXIT) break;
        if(command==WORK_NETWORK_LIST){network_work(command);continue;}
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
        uint32_t request=s_state.message_page_request;
        xSemaphoreGive(s_state_lock);
        esp_err_t err=load_messages_for_chat(index,true,true);
        xSemaphoreTake(s_state_lock,portMAX_DELAY);
        bool visible=s_state.nav.page==FEISHU_PAGE_MESSAGES && s_state.nav.chat_index==index &&
            request==s_state.message_page_request;
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

static esp_err_t reserve_audio_resources(void)
{
    unsigned before=(unsigned)esp_get_free_heap_size();TickType_t began=xTaskGetTickCount();
    if(s_cache_worker) {
        work_command_t exit=WORK_EXIT;
        if(xQueueSendToFront(s_cache_queue,&exit,pdMS_TO_TICKS(1000))!=pdTRUE)return ESP_ERR_TIMEOUT;
        xSemaphoreTake(s_cache_done,portMAX_DELAY);
        work_command_t pending;
        while(xQueueReceive(s_cache_queue,&pending,0)==pdTRUE){
            if(pending!=WORK_EXIT && xQueueSend(s_work_queue,&pending,0)!=pdTRUE){
                xSemaphoreTake(s_state_lock,portMAX_DELAY);s_state.loading=false;
                feishu_utf8_copy(s_state.status,sizeof(s_state.status),"操作队列已满，请重试");xSemaphoreGive(s_state_lock);
            }
        }
        /* FreeRTOS reaps a self-deleted task from idle, before codec allocation. */
        vTaskDelay(pdMS_TO_TICKS(10));
        xSemaphoreTake(s_state_lock,portMAX_DELAY);s_state.image_cache_queued=false;xSemaphoreGive(s_state_lock);
    }
    ESP_LOGI(TAG,"VOICE_RESERVE cache_worker=%u before=%u after=%u ms=%u",(unsigned)(s_cache_worker!=NULL),before,
        (unsigned)esp_get_free_heap_size(),(unsigned)((xTaskGetTickCount()-began)*portTICK_PERIOD_MS));
    return ESP_OK;
}
static void release_audio_resources(void)
{
    s_focus_audio_reserved=false;
    xSemaphoreTake(s_state_lock,portMAX_DELAY);bool closing=s_state.closing;xSemaphoreGive(s_state_lock);
    if(!closing)start_cache_worker();
    ESP_LOGI(TAG,"VOICE_RELEASE cache_worker=%u heap=%u",(unsigned)(s_cache_worker!=NULL),(unsigned)esp_get_free_heap_size());
}

static void fetch_current_text(void)
{
    int64_t began=esp_timer_get_time();
    feishu_message_t next;
    char id[FEISHU_MESSAGE_ID_MAX];uint32_t epoch,scope=feishu_cache_scope_version();
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    if(s_state.nav.page!=FEISHU_PAGE_MESSAGE_DETAIL || s_state.nav.message_index>=s_state.nav.message_count) {
        xSemaphoreGive(s_state_lock);return;
    }
    feishu_utf8_copy(id,sizeof(id),s_state.messages[s_state.nav.message_index].message_id);
    epoch=s_owner_epoch;
    xSemaphoreGive(s_state_lock);
    esp_err_t err=online() ? feishu_api_get_message(&s_session,id,&next) : ESP_ERR_INVALID_STATE;
    ESP_LOGI(TAG,"TEXT_FETCH result=%d bytes=%llu ms=%u",err,err==ESP_OK ? (unsigned long long)next.text_length:0,(unsigned)((esp_timer_get_time()-began)/1000));
    bool active=false;
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    active=epoch==s_owner_epoch && s_state.nav.page==FEISHU_PAGE_MESSAGE_DETAIL &&
        s_state.nav.message_index<s_state.nav.message_count &&
        !strcmp(id,s_state.messages[s_state.nav.message_index].message_id);
    if(active)s_state.status_error=err!=ESP_OK;
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
        /* Display the committed body before optional list-snapshot persistence.
         * Flash metadata/GC must not keep an already-readable page hidden. */
        load_text_page();
        message_page_frame_t *snapshot=calloc(1,sizeof(*snapshot));
        if(snapshot) {
            char page_key[64]={0},chat[FEISHU_CHAT_ID_MAX]={0};size_t rows=0;
            uint32_t page_request=0;
            xSemaphoreTake(s_state_lock,portMAX_DELAY);
            if(epoch==s_owner_epoch && s_state.nav.chat_index<s_state.nav.chat_count){
                feishu_utf8_copy(page_key,sizeof(page_key),s_state.message_page_key);
                feishu_utf8_copy(chat,sizeof(chat),s_state.chats[s_state.nav.chat_index].chat_id);
                page_request=s_state.message_page_request;
            }
            xSemaphoreGive(s_state_lock);
            bool framed=page_key[0] && feishu_cache_read(page_key,snapshot,sizeof(*snapshot),1,&rows,NULL)==ESP_OK &&
                rows==1 && feishu_page_valid(&snapshot->page,FEISHU_MAX_MESSAGES) && !strcmp(snapshot->chat,chat);
            bool current=false;
            xSemaphoreTake(s_state_lock,portMAX_DELAY);
            current=framed && epoch==s_owner_epoch && page_request==s_state.message_page_request &&
                !strcmp(page_key,s_state.message_page_key) && !strcmp(chat,s_state.loaded_chat);
            if(current){memcpy(snapshot->messages,s_state.messages,sizeof(s_state.messages));snapshot->page.count=s_state.nav.message_count;}
            xSemaphoreGive(s_state_lock);
            if(current && feishu_cache_write_if_scope(scope,page_key,snapshot,sizeof(*snapshot),1)==ESP_OK)
                feishu_cache_protect(page_key,&s_message_pin);
            free(snapshot);
        }
    }
    recover_revoked_auth(err);
    if(active && err!=ESP_OK)feedback_request(FEISHU_SOUND_FAILURE);
}

static esp_err_t send_pending_reply(void)
{
    esp_err_t err=ESP_OK;
            feishu_outbox_t pending;int slot;bool retry;uint32_t send_epoch;
            xSemaphoreTake(s_state_lock,portMAX_DELAY);
            bool wanted=s_state.sending;bool back_to_assistant=s_state.reply_from_assistant;
            pending=s_state.pending;slot=s_state.pending_slot;retry=s_state.pending_retry_warning;send_epoch=s_owner_epoch;
            if(!wanted && s_state.recovering_operation)s_state.loading=s_state.recovering_operation=false;
            xSemaphoreGive(s_state_lock);
            if(!wanted)return ESP_OK;
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
                s_state.loading=s_state.sending=s_state.recovering_operation=false;xSemaphoreGive(s_state_lock);return FEISHU_HTTP_ERR_CANCELLED;
            }
            bool replied=s_state.pending.message[0]!=0;
            s_state.loading=false;s_state.sending=false;s_state.pending_retry_warning=false;
            if(err==ESP_OK) {
                if(s_session.last_sent_id[0] && s_session.last_sent_preview[0]){
                    if(!s_sent_echo)s_sent_echo=calloc(1,sizeof(*s_sent_echo));
                    if(s_sent_echo){
                        memset(s_sent_echo,0,sizeof(*s_sent_echo));s_sent_echo->mine=true;s_sent_echo->type=FEISHU_MESSAGE_TEXT;
                        feishu_utf8_copy(s_sent_echo->message_id,sizeof(s_sent_echo->message_id),s_session.last_sent_id);
                        feishu_utf8_copy(s_sent_echo->sender_id,sizeof(s_sent_echo->sender_id),s_session.self_open_id);
                        feishu_utf8_copy(s_sent_echo->sender_name,sizeof(s_sent_echo->sender_name),"我");
                        feishu_mentions_display(s_session.last_sent_preview,s_sent_echo->text,sizeof(s_sent_echo->text));
                        s_sent_echo->text_length=s_session.last_sent_length;s_sent_echo->text_truncated=s_session.last_sent_length>=sizeof(s_sent_echo->text);
                        feishu_utf8_copy(s_sent_echo_chat,sizeof(s_sent_echo_chat),pending.chat);
                        s_sent_echo_epoch=s_owner_epoch;s_sent_echo_expires=xTaskGetTickCount()+pdMS_TO_TICKS(30000);
                        if(s_state.nav.chat_index<s_state.nav.chat_count && !strcmp(s_state.chats[s_state.nav.chat_index].chat_id,pending.chat))
                            (void)feishu_message_merge_ack(s_state.messages,&s_state.nav.message_count,FEISHU_MAX_MESSAGES,s_sent_echo);
                    }
                }
                s_state.nav.page=s_state.nav.draft_manager ? FEISHU_PAGE_DRAFTS:FEISHU_PAGE_MESSAGES;s_state.pending_slot=-1;
                memset(&s_state.pending,0,sizeof(s_state.pending));
                s_state.reply[0]=s_state.reply_uuid[0]=0;
                feishu_utf8_copy(s_state.status,sizeof(s_state.status),replied ? "回复已发送":"消息已发送");
                if(!s_state.nav.draft_manager && s_state.nav.chat_index<s_state.nav.chat_count &&
                   !strcmp(s_state.chats[s_state.nav.chat_index].chat_id,pending.chat)) {
                    /* A successful send returns to the latest server page,
                     * never to the old RAM snapshot or an older page cursor. */
                    ++s_state.message_page_request;
                    s_state.message_page_number=0;s_state.message_page_more=false;
                    s_state.message_page_key[0]=0;s_state.last_messages_chat[0]=0;
                    s_state.nav.composer_selected=true;
                }
            } else {
                s_state.pending=pending;s_state.nav.page=FEISHU_PAGE_REVIEW;
                feishu_utf8_copy(s_state.status,sizeof(s_state.status),pending.state==FEISHU_OUTBOX_READY ?
                    (s_state.cancel_send ? "已暂停，已发送进度及剩余文字保留":"发送未完成，已发送进度及剩余文字保留"):
                    "结果未确认，重试可能重复");
            }
            if(back_to_assistant){
                s_state.assistant_send_notice=err==ESP_OK ? 1:pending.state==FEISHU_OUTBOX_READY ? 2:3;
            }
            if(err==ESP_OK)s_state.reply_from_assistant=false;
            xSemaphoreGive(s_state_lock);
            if(err==ESP_OK && memory)(void)feishu_cache_prune_drafts(-1,true);
            if(err==ESP_OK && !memory)list_pending_drafts();
            if(err==ESP_OK)enqueue(WORK_REFRESH_MESSAGES);
            /* The existing worker resumes the same activation after queued
             * sends/refreshes; do not restart its timer or greeting here. */

    return err;
}

#ifdef FEISHU_SEND_PERF_AUTOTEST

#include "business_autotest_config.h"
/* Verify the actual ASR writer -> durable draft -> rendered review -> send path.
 * Only a free slot and the explicitly configured diagnostic chat are used. */
static bool __attribute__((noinline)) voice_persist_send_autotest(void)
{
    volatile bool stop=false,cancel=false;volatile unsigned elapsed=0;
    char recognition[FEISHU_REPLY_MAX]={0},expected[FEISHU_REPLY_MAX],uuid[41],name[64];
    feishu_data_writer_t body={0};feishu_outbox_t pending={0},check={0};
    int slot=-1;esp_err_t err=ESP_OK;bool durable=false,review=false;
    for(unsigned i=0;i<FEISHU_OUTBOX_SLOTS;++i) {
        esp_err_t found=feishu_cache_pending_read(i,&check);
        if(found==ESP_ERR_NOT_FOUND){slot=(int)i;break;}
        if(found!=ESP_OK){err=found;goto finished;}
    }
    if(slot<0){err=ESP_ERR_INVALID_STATE;goto finished;}
    feishu_asr_autotest_prepare(&stop,&cancel);
    err=feishu_asr_record(&s_session,&stop,&cancel,&elapsed,recognition,sizeof(recognition),&body,NULL);
    feishu_asr_autotest_finish();
    if(err!=ESP_OK)goto finished;
    if(strcmp(recognition,BUSINESS_VOICE_EXPECTED)){err=ESP_ERR_INVALID_RESPONSE;goto finished;}
    /* ASR has already written the final short block. The text-range writer
     * rejects appending after that tail; test its actual output unchanged.
     * The xiaotest_ metadata namespace identifies this disposable test. */
    if(body.raw!=strlen(recognition)){err=ESP_ERR_INVALID_RESPONSE;goto finished;}
    snprintf(expected,sizeof(expected),"%s",recognition);
    make_reply_uuid(uuid);snprintf(name,sizeof(name),"xiaotest_%s",uuid);
    if(!feishu_outbox_create(&pending,body.id,body.raw,name,BUSINESS_CHAT,"",uuid)){err=ESP_ERR_INVALID_RESPONSE;goto finished;}
    err=feishu_cache_pending_write((unsigned)slot,&pending,&body);if(err!=ESP_OK)goto finished;durable=true;
    err=feishu_cache_pending_read((unsigned)slot,&check);if(err!=ESP_OK)goto finished;
    if(strcmp(check.uuid,uuid) || check.length!=strlen(expected)){err=ESP_ERR_INVALID_RESPONSE;goto finished;}
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    s_state.pending=check;s_state.pending_slot=slot;s_state.nav.page=FEISHU_PAGE_REVIEW;
    s_state.nav.draft_manager=false;s_state.nav.reply_to_message=false;s_state.detail_page=0;
    s_state.detail_loaded=false;s_state.loading=true;s_state.status[0]=0;
    xSemaphoreGive(s_state_lock);
    load_draft_page();vTaskDelay(pdMS_TO_TICKS(250));
    if(bsp_lvgl_lock(1000)) {
        review=!strcmp(lv_label_get_text(s_content),expected);
        bsp_lvgl_unlock();
    }
    if(!review){err=ESP_ERR_INVALID_RESPONSE;goto finished;}
    feishu_cache_unpin(&s_text_pin);
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    size_t test_chat=s_state.nav.chat_count;
    for(size_t i=0;i<s_state.nav.chat_count;++i)if(!strcmp(s_state.chats[i].chat_id,BUSINESS_CHAT)){test_chat=i;break;}
    if(test_chat<s_state.nav.chat_count) {
        s_state.nav.chat_index=test_chat;s_state.sending=true;s_state.cancel_send=false;
        s_state.nav.composer_selected=true;s_state.nav.draft_manager=false;
    }
    xSemaphoreGive(s_state_lock);
    if(test_chat>=s_state.nav.chat_count){err=ESP_ERR_NOT_FOUND;goto finished;}
    err=send_pending_reply();
    if(err==ESP_OK)err=load_messages_for_chat(test_chat,false,false);
    bool found=false;
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    for(size_t i=0;i<s_state.nav.message_count;++i)if(!strcmp(s_state.messages[i].text,expected))found=true;
    bool current=s_state.nav.page==FEISHU_PAGE_MESSAGES && !s_state.loading && s_state.nav.composer_selected;
    xSemaphoreGive(s_state_lock);
    ESP_LOGI(TAG,"POST_SEND_UI pass=%u newest_visible=%u current_page=%u",(unsigned)(found&&current),(unsigned)found,(unsigned)current);
    if(!found || !current)err=ESP_ERR_INVALID_RESPONSE;

finished:
    feishu_asr_autotest_finish();
    feishu_cache_text_abort(&body);feishu_cache_unpin(&s_text_pin);
    ESP_LOGI(TAG,"VOICE_PERSIST_SEND pass=%u err=%d durable=%u review_exact=%u slot=%d bytes=%u",
        (unsigned)(err==ESP_OK),(int)err,(unsigned)durable,(unsigned)review,slot,(unsigned)pending.length);
    return err==ESP_OK;
}
/* Device-only UI data-path exercise: two conversations, then disk hits after
 * switching away. No extra sends, no draft/config deletion, no private IDs. */
static void __attribute__((noinline)) latency_ui_autotest(void)
{
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    size_t count=s_state.nav.chat_count<2 ? s_state.nav.chat_count:2;
    xSemaphoreGive(s_state_lock);
    unsigned passed=0;
    for(unsigned round=0;round<2;++round)for(size_t i=0;i<count;++i) {
        xSemaphoreTake(s_state_lock,portMAX_DELAY);
        s_state.nav.page=FEISHU_PAGE_MESSAGES;s_state.nav.chat_index=i;
        ++s_state.message_page_request;s_state.loading=true;
        /* Require a durable snapshot rather than a RAM self-hit. */
        if(round)s_state.loaded_chat[0]=0;
        xSemaphoreGive(s_state_lock);
        TickType_t began=xTaskGetTickCount();
        esp_err_t result=load_messages_for_chat(i,false,round!=0);
        xSemaphoreTake(s_state_lock,portMAX_DELAY);
        bool shown=result==ESP_OK && !s_state.loading &&
            !strcmp(s_state.loaded_chat,s_state.chats[i].chat_id);
        xSemaphoreGive(s_state_lock);
        passed+=shown;
        ESP_LOGI(TAG,"LATENCY_UI round=%u chat=%u pass=%u ms=%u err=%d",round,(unsigned)i,
            (unsigned)shown,(unsigned)((xTaskGetTickCount()-began)*portTICK_PERIOD_MS),result);
    }
    bool voice=voice_persist_send_autotest();
    /* Same focus handler as the ordinary worker, with real HTTPS in parallel
     * with discarded microphone samples. Never sends additional messages. */
    unsigned focus_passed=0;unsigned focus_heap=esp_get_free_heap_size();
    for(unsigned cycle=0;cycle<3;++cycle) {
        xSemaphoreTake(s_state_lock,portMAX_DELAY);
        size_t index=s_state.nav.chat_index;
        s_state.nav.page=FEISHU_PAGE_MESSAGES;s_state.nav.composer_selected=true;s_state.loading=false;
        xSemaphoreGive(s_state_lock);
        apply_audio_focus();
        for(unsigned i=0;i<300 && !feishu_asr_prepare_ready();++i)vTaskDelay(pdMS_TO_TICKS(10));
        bool ready=feishu_asr_prepare_ready();
        esp_err_t refreshed=load_messages_for_chat(index,false,false);
        xSemaphoreTake(s_state_lock,portMAX_DELAY);
        s_state.nav.composer_selected=false;
        xSemaphoreGive(s_state_lock);
        sync_audio_focus();apply_audio_focus();vTaskDelay(pdMS_TO_TICKS(20));
        bool released=!feishu_asr_prepare_ready();
        bool pass=ready && refreshed==ESP_OK && released;
        focus_passed+=pass;
        ESP_LOGI(TAG,"FOCUS_PREWARM cycle=%u pass=%u ready=%u refresh=%d released=%u before=%u after=%u minimum=%u",
            cycle,(unsigned)pass,(unsigned)ready,refreshed,(unsigned)released,focus_heap,
            (unsigned)esp_get_free_heap_size(),(unsigned)esp_get_minimum_free_heap_size());
    }
    ESP_LOGI(TAG,"LATENCY_UI_DONE pass=%u checks=%u expected=%u",
        (unsigned)(count==2 && passed==4 && voice && focus_passed==3),passed,(unsigned)(count*2));
}
#endif

#ifdef FEISHU_MEMORY_DIAGNOSTIC
#include "../tools/memory_probe/memory_probe.inc"
#endif
#ifdef FEISHU_MCP_DIAGNOSTIC
static void mcp_device_probe(void)
{
    (void)feishu_asr_prepare(false);
    esp_err_t err=reserve_audio_resources();
    if(err==ESP_OK) {
        xSemaphoreTake(s_state_lock,portMAX_DELAY);
        s_state.nav.page=FEISHU_PAGE_STATUS;s_state.loading=false;
        feishu_utf8_copy(s_state.status,sizeof(s_state.status),"自动协议检查，保持静音");
        feishu_mcp_status_t snapshot={"status",(unsigned)s_state.nav.chat_count,
            (unsigned)s_state.nav.message_count,s_state.battery_percent,feishu_network_connected()};
        char chat[FEISHU_CHAT_ID_MAX];feishu_utf8_copy(chat,sizeof(chat),s_state.chats[0].chat_id);
        xSemaphoreGive(s_state_lock);
        err=feishu_asr_mcp_probe(&s_session,chat,&snapshot);
    }
    release_audio_resources();
    ESP_LOGI(TAG,"MCP_DEVICE_DONE pass=%u result=%d heap=%u minimum=%u",(unsigned)(err==ESP_OK),err,
        (unsigned)esp_get_free_heap_size(),(unsigned)esp_get_minimum_free_heap_size());
}
#endif

#include "feishu_network_ui.inc"
#include "feishu_power_ui.inc"
#ifdef FEISHU_NETWORK_UI_AUTOTEST
#include "../tools/ui_probe/network_ui.inc"
#endif

#include "feishu_feedback_ui.inc"
#include "feishu_assistant_ui.inc"
static esp_err_t __attribute__((noinline)) process_record_command(work_command_t command)
{
    (void)command;esp_err_t err=ESP_OK;
            char recognition[FEISHU_REPLY_MAX]={0},chat[64]={0},message[64]={0},uuid[41],name[64];
            int slot=-1;bool cancelled;
            uint32_t epoch;
            xSemaphoreTake(s_state_lock,portMAX_DELAY);
            cancelled=s_state.cancel_recording || (s_state.nav.page!=FEISHU_PAGE_RECORDING && s_state.nav.page!=FEISHU_PAGE_TRANSCRIBING);
            epoch=s_owner_epoch;
            if(s_state.record_replaces_pending && s_state.pending_slot!=-1) {
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
                xSemaphoreGive(s_state_lock);return err;
            }
            if(slot<0 && slot!=RAM_REPLY_SLOT){
                if(feishu_cache_draft_slot(&slot)!=ESP_OK)slot=RAM_REPLY_SLOT;
            }
            if(!chat[0])err=ESP_ERR_INVALID_STATE;
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
            bool asr_complete=err==ESP_OK;
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
            bool published=err==ESP_OK && slot>=0 && pending.object_id>0;
            if(!cancelled && asr_complete && recognition[0] && !published &&
               (!draft.raw || draft.raw==strlen(recognition))){
                memset(&pending,0,sizeof(pending));pending.version=1;pending.state=FEISHU_OUTBOX_READY;
                pending.length=strlen(recognition);feishu_utf8_copy(pending.chat,sizeof(pending.chat),chat);
                feishu_utf8_copy(pending.message,sizeof(pending.message),message);feishu_utf8_copy(pending.uuid,sizeof(pending.uuid),uuid);
                slot=RAM_REPLY_SLOT;err=ESP_OK;
            }
            if(!cancelled && err==ESP_OK && slot>=0)(void)feishu_cache_prune_drafts(slot,true);
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
                xSemaphoreGive(s_state_lock);memset(recognition,0,sizeof(recognition));memset(uuid,0,sizeof(uuid));return err;
            }
            s_state.loading=false;s_state.record_pending=false;
            s_state.status_error=!cancelled && err!=ESP_OK;
            if(cancelled) {
                s_state.cancel_recording=false;s_state.nav.page=s_state.nav.draft_manager ? FEISHU_PAGE_DRAFTS:FEISHU_PAGE_MESSAGES;
            } else if(err==ESP_OK) {
                s_state.pending=pending;s_state.pending_slot=slot;s_state.pending_retry_warning=false;
                if(!s_state.record_replaces_pending)s_state.reply_from_assistant=false;
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
            if(!cancelled && err==FEISHU_ERR_XIAO_PAIRING) {
                feishu_asr_pairing_snapshot(&s_state.pairing);
                s_state.pairing_return_page=s_state.record_replaces_pending ? FEISHU_PAGE_REVIEW:FEISHU_PAGE_MESSAGES;
                s_state.nav.page=FEISHU_PAGE_XIAOZHI;s_state.cancel_pairing=false;s_state.status_error=false;s_state.pairing_auto_blocked=false;s_state.pairing_retry_at=0;
                feishu_utf8_copy(s_state.status,sizeof(s_state.status),"请先在小智控制台完成配对");
                err=ESP_OK;
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

    return err;
}
static esp_err_t __attribute__((noinline)) process_image_command(work_command_t command)
{
    (void)command;esp_err_t err=ESP_OK;
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

    return err;
}
static esp_err_t __attribute__((noinline)) process_pairing_command(work_command_t command)
{
    (void)command;esp_err_t err=ESP_OK;
            xSemaphoreTake(s_state_lock,portMAX_DELAY);
            uint32_t request=s_state.pairing_request;
            bool active=s_state.nav.page==FEISHU_PAGE_XIAOZHI && !s_state.cancel_pairing;
            xSemaphoreGive(s_state_lock);
            if(!active)return err;
            feishu_asr_pairing_t info;
            err=feishu_network_connected() ? feishu_asr_pairing_check(&info,&s_state.cancel_pairing):ESP_ERR_INVALID_STATE;
            xSemaphoreTake(s_state_lock,portMAX_DELAY);
            bool wanted=s_state.nav.page==FEISHU_PAGE_XIAOZHI && !s_state.cancel_pairing && request==s_state.pairing_request;
            if(request==s_state.pairing_request)s_state.pairing_busy=false;
            if(wanted) {
                s_state.pairing_retry_at=(uint32_t)(esp_timer_get_time()/1000)+30000;
                s_state.pairing_auto_blocked=err==ESP_ERR_NOT_SUPPORTED;
                if(err==ESP_OK) {
                    s_state.pairing=info;
                    feishu_utf8_copy(s_state.status,sizeof(s_state.status),info.paired ? "配对已完成":info.code[0] ? "等待完成配对":"尚未获取设备码");
                } else {
                    s_state.pairing.paired=false;
                    feishu_utf8_copy(s_state.status,sizeof(s_state.status),err==ESP_ERR_NOT_SUPPORTED ? "当前固件未启用小智语音":"刷新失败，确定可重试");
                }
            } else err=FEISHU_HTTP_ERR_CANCELLED;
            xSemaphoreGive(s_state_lock);

    return err;
}
static void process_work_command_inner(work_command_t command)
{
        esp_err_t err = ESP_OK;
        xSemaphoreTake(s_state_lock,portMAX_DELAY);
        bool closing=s_state.closing;
        xSemaphoreGive(s_state_lock);
        if (command == WORK_EXIT || closing) return;
        if(command==WORK_POWER_APPLY){atomic_store(&s_power_queued,false);feishu_power_network_poll();return;}
        if(command==WORK_POWER_SAVE){
            feishu_power_settings_t p=feishu_power_get_settings();esp_err_t e=feishu_store_save_power(&p);
            xSemaphoreTake(s_state_lock,portMAX_DELAY);s_state.status_error=e!=ESP_OK;
            feishu_utf8_copy(s_state.status,sizeof(s_state.status),e==ESP_OK ? "省电设置已保存":"保存失败，当前设置仅本次生效");xSemaphoreGive(s_state_lock);return;
        }
        if(command==WORK_FRONT_REFRESH){
            atomic_store(&s_front_queued,false);xSemaphoreTake(s_state_lock,portMAX_DELAY);
            feishu_page_t page=s_state.nav.page;bool ready=s_state.ready && !s_state.loading;size_t index=s_state.nav.chat_index;
            bool first=s_state.chat_page_number==0;xSemaphoreGive(s_state_lock);
            if(ready && online()){
                if(page==FEISHU_PAGE_CHATS){if(first)(void)load_conversation_list();else (void)refresh_conversations();}
                else if(page==FEISHU_PAGE_MESSAGES)(void)load_messages_for_chat(index,false,false);
            }
            return;
        }
        if(command==WORK_FEEDBACK){atomic_store(&s_feedback_queued,false);feedback_play(s_state.assistant_active,false);return;}
        if(command==WORK_SOUND_SAVE){
            xSemaphoreTake(s_state_lock,portMAX_DELAY);feishu_sound_settings_t settings=s_state.sounds;xSemaphoreGive(s_state_lock);
            esp_err_t saved=feishu_store_save_sounds(&settings);
            xSemaphoreTake(s_state_lock,portMAX_DELAY);s_state.status_error=saved!=ESP_OK;
            feishu_utf8_copy(s_state.status,sizeof(s_state.status),saved==ESP_OK ? "设置已保存":"保存失败，请重试");xSemaphoreGive(s_state_lock);
            feedback_request(saved==ESP_OK ? FEISHU_SOUND_SUCCESS:FEISHU_SOUND_FAILURE);return;
        }
        if(command==WORK_ASSISTANT_SAVE){
            xSemaphoreTake(s_state_lock,portMAX_DELAY);
            feishu_assistant_settings_t settings=s_state.assistant_settings;
            xSemaphoreGive(s_state_lock);
            esp_err_t saved=feishu_store_save_assistant(&settings);
            xSemaphoreTake(s_state_lock,portMAX_DELAY);
            s_state.status_error=saved!=ESP_OK;
            feishu_utf8_copy(s_state.status,sizeof(s_state.status),saved==ESP_OK ? "设置已保存":"保存失败，请重试");
            xSemaphoreGive(s_state_lock);return;
        }
        if(!online() && (command==WORK_RECORD || command==WORK_SEND)) {
            xSemaphoreTake(s_state_lock,portMAX_DELAY);
            s_state.loading=false;
            bool back=command==WORK_SEND && s_state.reply_from_assistant;
            if(back){s_state.sending=false;s_state.assistant_send_notice=2;}
            s_state.nav.page=back ? FEISHU_PAGE_REVIEW:FEISHU_PAGE_STATUS;
            feishu_utf8_copy(s_state.status,sizeof(s_state.status),"离线缓存可读，请联网后重试此操作");
            xSemaphoreGive(s_state_lock);
            if(back)assistant_enter();
            return;
        }
        if(command==WORK_CHECK_XIAOZHI) {
            err=process_pairing_command(command);
        } else if (command == WORK_INIT) {
            start_cache_worker();
            feishu_power_settings_t power;esp_err_t power_err=feishu_store_load_power(&power);
            feishu_power_set_settings(&power);if(power_err!=ESP_OK)ESP_LOGW(TAG,"power settings fallback=%d",power_err);
            xSemaphoreTake(s_state_lock,portMAX_DELAY);uint32_t init_request=s_state.chat_page_request;xSemaphoreGive(s_state_lock);
            if(!s_state.assistant_boot_checked){
                feishu_assistant_settings_t settings;
                esp_err_t loaded=feishu_store_load_assistant(&settings);
                feishu_sound_settings_t sounds;esp_err_t sound_loaded=feishu_store_load_sounds(&sounds);
                xSemaphoreTake(s_state_lock,portMAX_DELAY);s_state.sounds=sounds;xSemaphoreGive(s_state_lock);
                if(sound_loaded!=ESP_OK)ESP_LOGW(TAG,"sound settings fallback=%d",sound_loaded);
                xSemaphoreTake(s_state_lock,portMAX_DELAY);s_state.assistant_settings=settings;xSemaphoreGive(s_state_lock);
                if(loaded!=ESP_OK)ESP_LOGW(TAG,"assistant settings load=%d; safe defaults",loaded);
            }
            err = initialize_feishu();
            if (err != ESP_OK) ESP_LOGW(TAG, "initialization failed: %s", esp_err_to_name(err));
#ifdef FEISHU_VOICE_AUTOTEST
            ESP_LOGI(TAG,"INIT_READY result=%d heap=%u largest=%u",err,(unsigned)esp_get_free_heap_size(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
#endif
            xSemaphoreTake(s_state_lock, portMAX_DELAY);
            bool cached_ready=s_state.ready;
            bool home=s_state.nav.page==FEISHU_PAGE_CHATS && s_state.chat_page_number==0 &&
                (!s_state.loading || init_request==s_state.chat_page_request);
            s_state.ready = cached_ready || err == ESP_OK;
            s_state.startup_pending=false;
            if(home) {
            s_state.loading=false;
            feishu_utf8_copy(s_state.status, sizeof(s_state.status),
                             err == ESP_OK ? "" : feishu_network_failure_hint() ? feishu_network_failure_hint():
                             (cached_ready && s_state.nav.chat_count>0 ? "显示已有内容，稍后重试更新" :
                             err == ESP_ERR_INVALID_STATE || err == ESP_ERR_NVS_NOT_FOUND ?
                              "请先完成配网和绑定" : "暂时无法连接飞书，稍后重试"));
            }
            xSemaphoreGive(s_state_lock);
            if(err==ESP_OK && home && !s_state.assistant_boot_checked){
                xSemaphoreTake(s_state_lock,portMAX_DELAY);
                s_state.assistant_boot_checked=true;
#ifndef FEISHU_ASSISTANT_AUTOTEST
                bool boot_assistant=s_state.assistant_settings.boot;
#endif
                xSemaphoreGive(s_state_lock);
#ifndef FEISHU_ASSISTANT_AUTOTEST
                if(boot_assistant){assistant_enter();return;}
#endif
            }
            if (err == ESP_OK && home) {
#ifdef FEISHU_ASSISTANT_AUTOTEST
                static bool assistant_tested;
                if(!assistant_tested) {
                    assistant_tested=true;
                    s_assistant_suite_pending=true;return;
                }
#endif
#ifdef FEISHU_MCP_DIAGNOSTIC
                static bool mcp_tested;
                if(!mcp_tested){mcp_tested=true;mcp_device_probe();return;}
#endif
#ifdef FEISHU_MEMORY_DIAGNOSTIC
                static bool memory_tested;
                if(!memory_tested && err==ESP_OK) {
                    memory_tested=true;memory_probe();return;
                }
#endif
#ifdef FEISHU_VOICE_AUTOTEST
                /* Run after normal initialization/UI allocation, before slow
                 * optional summary enrichment can consume the probe window. */
                static bool tested;
                if(!tested) {
                    tested=true;
#ifdef FEISHU_ASSISTANT_AUTOTEST
                    return; /* Dedicated read-only assistant test already ran. */
#endif
#ifdef FEISHU_DRAFT_AUTOTEST
                    if(feishu_draft_autotest_resume())return;
#ifdef FEISHU_DRAFT_VOICE_SUITE
                    feishu_asr_autotest(&s_session);
#else
                    if(!feishu_asr_autotest_provision()) {
                        ESP_LOGI(TAG,"DRAFT_PHASE phase=1 pass=0 provision=0");return;
                    }
#endif
#elif defined(FEISHU_BUSINESS_AUTOTEST)
#ifdef FEISHU_SEND_PERF_AUTOTEST
                    feishu_business_autotest(&s_session);latency_ui_autotest();return;
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
                    if(s_voice_ui_probe){enqueue(WORK_RECORD);return;}
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
        } else if(command==WORK_AUDIO_FOCUS) {
            atomic_store(&s_audio_focus_queued,false);
            apply_audio_focus();
        } else if (command == WORK_REFRESH_MESSAGES) {
            xSemaphoreTake(s_state_lock,portMAX_DELAY);
            bool visible=s_state.nav.page==FEISHU_PAGE_MESSAGES;
            size_t index=s_state.nav.chat_index;
            xSemaphoreGive(s_state_lock);
            if(visible && online()) {
                err=load_messages_for_chat(index,true,false);
                if(err!=ESP_OK) {
                    xSemaphoreTake(s_state_lock,portMAX_DELAY);
                    feishu_utf8_copy(s_state.status,sizeof(s_state.status),"缓存可读，暂时无法更新");
                    xSemaphoreGive(s_state_lock);
                }
            }
        } else if (command == WORK_LOAD_MESSAGES) {
            ESP_LOGI(TAG,"MESSAGE_LOAD_BEGIN");
            size_t chat_index;
            xSemaphoreTake(s_state_lock, portMAX_DELAY);
            chat_index = s_state.nav.chat_index;
            xSemaphoreGive(s_state_lock);
            err = load_messages_for_chat(chat_index, true, true);
            if(err!=ESP_OK && err!=FEISHU_HTTP_ERR_CANCELLED)err=load_messages_for_chat(chat_index,true,false);
            if (err != ESP_OK) ESP_LOGW(TAG, "message load failed: %s", esp_err_to_name(err));
            xSemaphoreTake(s_state_lock, portMAX_DELAY);
            if(s_state.nav.page==FEISHU_PAGE_MESSAGES && s_state.nav.chat_index==chat_index) {
            s_state.loading = false;
            if(err==FEISHU_HTTP_ERR_CANCELLED){
                feishu_utf8_copy(s_state.status,sizeof(s_state.status),"语音优先，稍后继续读取");
            } else if (err != ESP_OK) {
                bool same_chat=chat_index<s_state.nav.chat_count &&
                    !strcmp(s_state.loaded_chat,s_state.chats[chat_index].chat_id);
                feishu_nav_message_load_failed(&s_state.nav,same_chat);
                s_state.message_page_loading=false;
                if(!same_chat){
                    memset(s_state.messages,0,sizeof(s_state.messages));
                    s_state.loaded_chat[0]=s_state.message_page_key[0]=0;
                    s_state.message_page_number=0;s_state.message_page_more=false;
                }
                feishu_utf8_copy(s_state.status, sizeof(s_state.status),
                                 feishu_network_failure_hint() ? feishu_network_failure_hint():"无法加载消息，可返回重试");
            } else {
                s_state.status[0] = '\0';
            }
            }
            xSemaphoreGive(s_state_lock);
        } else if (command == WORK_LOAD_IMAGE || command==WORK_FETCH_IMAGE) {
            err=process_image_command(command);
        } else if (command == WORK_RECORD) {
            err=process_record_command(command);
        } else if(command==WORK_CLEAR_DRAFTS) {
            err=feishu_cache_prune_drafts(-1,true);
            xSemaphoreTake(s_state_lock,portMAX_DELAY);
            memset(&s_state.pending,0,sizeof(s_state.pending));s_state.pending_slot=-1;s_state.reply[0]=0;s_state.reply_from_assistant=false;
            s_state.nav.page=FEISHU_PAGE_DRAFTS;s_state.nav.draft_index=0;s_state.loading=false;
            xSemaphoreGive(s_state_lock);list_pending_drafts();
            set_status(err==ESP_OK ? "本地草稿已清空":"部分草稿未清理，请重试",false);
        } else if(command==WORK_DISCARD_DRAFT) {
            feishu_outbox_t pending;int slot;
            xSemaphoreTake(s_state_lock,portMAX_DELAY);
            pending=s_state.pending;slot=s_state.pending_slot;
            xSemaphoreGive(s_state_lock);
            esp_err_t older_cleanup=ESP_OK;
            if(slot==RAM_REPLY_SLOT){older_cleanup=feishu_cache_prune_drafts(-1,true);err=ESP_OK;}
            else if(slot<0)err=ESP_ERR_NOT_FOUND;
            else {pending.state=FEISHU_OUTBOX_EMPTY;err=feishu_cache_pending_write((unsigned)slot,&pending,NULL);}
            xSemaphoreTake(s_state_lock,portMAX_DELAY);
            s_state.loading=false;
            if(err==ESP_OK) {
                s_state.pending_slot=-1;memset(&s_state.pending,0,sizeof(s_state.pending));s_state.reply_from_assistant=false;
                s_state.reply[0]=s_state.reply_uuid[0]=0;s_state.nav.page=s_state.nav.draft_manager ? FEISHU_PAGE_DRAFTS:FEISHU_PAGE_MESSAGES;
                feishu_utf8_copy(s_state.status,sizeof(s_state.status),older_cleanup==ESP_OK ? "草稿已删除":"内存草稿已删，旧记录清理失败");
            } else {
                s_state.nav.page=FEISHU_PAGE_REVIEW;
                feishu_utf8_copy(s_state.status,sizeof(s_state.status),"删除未完成，草稿仍保留");
            }
            xSemaphoreGive(s_state_lock);
            if(err==ESP_OK && slot!=RAM_REPLY_SLOT)list_pending_drafts();
        } else if (command == WORK_SEND) {
            err=send_pending_reply();
        }
#ifdef FEISHU_NETWORK_UI_AUTOTEST
        else if(command==WORK_NETWORK_AUTOTEST) {network_ui_autotest();}
#endif
        else if(command==WORK_NETWORK_LIST || command==WORK_NETWORK_SCAN || command==WORK_NETWORK_CONNECT ||
                command==WORK_NETWORK_DISCONNECT || command==WORK_NETWORK_REMOVE || command==WORK_NETWORK_EDIT || command==WORK_NETWORK_PASSWORD_SAVE) {
            network_work(command);
        } else if (command == WORK_RESET_WIFI) {
            err=feishu_store_require_wifi_setup(true);
            if(err!=ESP_OK) {
                set_status("无法保存配网请求，请稍后重试",false);
                return;
            }
            ESP_LOGI(TAG, "Wi-Fi reset requested: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(300));
            esp_restart();
        } else if (command == WORK_UNBIND || command==WORK_REAUTHORIZE) {
            err = feishu_store_request_auth(command==WORK_UNBIND ? FEISHU_AUTH_CHANGE_APP:FEISHU_AUTH_REAUTHORIZE);
            if(err!=ESP_OK) {
                set_status("无法保存授权请求，原配置保留",false);
                return;
            }
            ESP_LOGI(TAG, "Feishu authorization requested: %s", esp_err_to_name(err));
            feishu_boot_authorization_entry();
            esp_restart();
        }
        xSemaphoreTake(s_state_lock,portMAX_DELAY);
        s_state.status_error=err!=ESP_OK && err!=FEISHU_HTTP_ERR_CANCELLED;
        xSemaphoreGive(s_state_lock);
        recover_revoked_auth(err);
        if(err!=FEISHU_HTTP_ERR_CANCELLED && (command==WORK_INIT || command==WORK_RECORD || command==WORK_LOAD_MESSAGES || command==WORK_SEND || command==WORK_CHAT_PAGE || command==WORK_MESSAGE_PAGE))
            feedback_request(err==ESP_OK ? FEISHU_SOUND_SUCCESS:FEISHU_SOUND_FAILURE);
}
static void process_work_command(work_command_t command)
{
 bool active=command!=WORK_POWER_APPLY;
 if(active){feishu_power_busy_begin();}
 feishu_power_network_poll();
 process_work_command_inner(command);
 if(active){feishu_power_busy_end();}
 feishu_power_network_poll();
}
static void worker_task(void *argument)
{
    (void)argument;
    work_command_t command;
    TickType_t last_chat_refresh = 0,last_battery=0;

    for (;;) {
        feishu_power_network_poll();
        if(!last_battery || xTaskGetTickCount()-last_battery>=pdMS_TO_TICKS(15000)) {
            int percent=bsp_battery_soc();
            if(percent<0 && bsp_battery_init()==ESP_OK)percent=bsp_battery_soc();
            xSemaphoreTake(s_state_lock,portMAX_DELAY);s_state.battery_percent=percent;xSemaphoreGive(s_state_lock);
            ESP_LOGI(TAG,"BATTERY_SAMPLE soc=%d mv=%d",percent,bsp_battery_mv());
            last_battery=xTaskGetTickCount();
        }
        work_command_t queued;unsigned waiting=uxQueueMessagesWaiting(s_work_queue);
        bool background_only=waiting==1 && xQueuePeek(s_work_queue,&queued,0)==pdTRUE && queued==WORK_REFRESH_MESSAGES;
        xSemaphoreTake(s_state_lock,portMAX_DELAY);
        bool resume=s_state.assistant_enabled && !s_state.assistant_active && !s_state.assistant_requested &&
            !s_state.closing && !s_state.loading && !s_state.sending && online() &&
            s_state.nav.page!=FEISHU_PAGE_RECORDING && s_state.nav.page!=FEISHU_PAGE_TRANSCRIBING &&
            (waiting==0 || background_only);
        if(resume)s_state.assistant_requested=true;
        xSemaphoreGive(s_state_lock);
        if(resume){command=WORK_ASSISTANT;if(background_only)ESP_LOGI(TAG,"ASSISTANT_RESUME before_refresh=1");}
        else if (xQueueReceive(s_work_queue, &command,
                          pdMS_TO_TICKS(feishu_power_background_allowed() ? 2000:15000)) != pdTRUE) {
            feishu_page_t page;
            bool ready;
            bool initialized;
            xSemaphoreTake(s_state_lock, portMAX_DELAY);
            initialized = s_state.ready;
            ready = initialized && !s_state.loading;
            page = s_state.nav.page;
            xSemaphoreGive(s_state_lock);
            if (ready && online() && feishu_power_background_allowed() && page == FEISHU_PAGE_MESSAGES) {
                esp_err_t refresh_err = poll_visible_messages();
                recover_revoked_auth(refresh_err);
                if (refresh_err != ESP_OK) {
                    ESP_LOGW(TAG, "background message refresh failed");
                }
            } else if (ready && online() && feishu_power_background_allowed() && page == FEISHU_PAGE_CHATS &&
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
            if(feishu_network_user_offline())continue;
            xSemaphoreTake(s_state_lock, portMAX_DELAY);
            s_state.loading = !initialized;
            xSemaphoreGive(s_state_lock);
            command = WORK_INIT;
        }
        xSemaphoreTake(s_state_lock,portMAX_DELAY);bool closing=s_state.closing;xSemaphoreGive(s_state_lock);
        if(command==WORK_EXIT || closing)break;
        if(command==WORK_ASSISTANT){assistant_worker();feedback_play(false,false);}
        else process_work_command(command);
#ifdef FEISHU_ASSISTANT_AUTOTEST
        if(s_assistant_suite_pending){s_assistant_suite_pending=false;assistant_suite();}
#endif
    }
    feishu_asr_prepare_cancel();
    (void)feishu_asr_prepare(false);
    s_focus_audio_reserved=false;
    if(s_cache_worker) xSemaphoreTake(s_cache_done,portMAX_DELAY);
    save_read_position();
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
                            LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_rows[slot],
        lv_color_hex(selected ? FEISHU_SURFACE_FOCUS : FEISHU_SURFACE), 0);
    lv_obj_set_style_border_width(s_rows[slot], selected ? 2 : 0, 0);
    lv_obj_set_style_border_color(s_rows[slot], lv_color_hex(FEISHU_FOCUS), 0);
    lv_obj_set_style_radius(s_rows[slot], 6, 0);
    lv_obj_set_style_text_color(s_row_text[slot], lv_color_hex(FEISHU_TEXT), 0);
    lv_obj_set_style_bg_color(s_row_mark[slot], lv_color_hex(FEISHU_ERROR), 0);
}

/* All business action menus reuse the settings row objects and focus style. */
static void render_choice_rows(const char *const *labels,size_t count,size_t selected,bool multiline)
{
    show_list_rows(true);
    size_t start=visible_start(selected,count);
    bool drafts=s_state.nav.page==FEISHU_PAGE_DRAFTS;
    if(drafts){start=selected>=3 ? selected-2:0;if(count>3 && start>count-3)start=count-3;if(count<=3)start=0;}
    for(size_t slot=0;slot<FEISHU_VISIBLE_ROWS;++slot) {
        size_t index=start+slot;
        if(index>=count || (drafts && slot>=3)){lv_obj_add_flag(s_rows[slot],LV_OBJ_FLAG_HIDDEN);continue;}
        lv_obj_remove_flag(s_rows[slot],LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_rows[slot],10,FEISHU_CONTENT_TOP+(int)slot*(multiline ? 52:34));
        lv_obj_set_size(s_rows[slot],220,multiline ? 46:29);
        style_list_row(slot,index==selected);lv_obj_set_style_radius(s_rows[slot],8,0);
        if(s_state.nav.page==FEISHU_PAGE_SETTINGS && index==FEISHU_SETTING_ASSISTANT){
            lv_obj_set_style_bg_color(s_rows[slot],lv_color_hex(0x164954),0);
            lv_obj_set_style_bg_opa(s_rows[slot],LV_OPA_COVER,0);
            lv_obj_set_style_text_color(s_row_text[slot],lv_color_hex(0x7EE2D2),0);
        }
        lv_obj_add_flag(s_row_avatar[slot],LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_row_subtext[slot],LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(s_row_text[slot],196,multiline ? 36:18);
        lv_obj_set_style_pad_all(s_row_text[slot],0,0);lv_obj_set_style_bg_opa(s_row_text[slot],LV_OPA_TRANSP,0);
        lv_obj_set_style_border_width(s_row_text[slot],0,0);lv_obj_set_style_text_align(s_row_text[slot],LV_TEXT_ALIGN_LEFT,0);
        lv_obj_set_style_text_line_space(s_row_text[slot],1,0);
        lv_label_set_long_mode(s_row_text[slot],multiline ? LV_LABEL_LONG_CLIP:LV_LABEL_LONG_DOT);
        const char *detail=multiline ? strchr(labels[index],'\n'):NULL;
        if(detail) {
            char title[96];feishu_utf8_copy(title,sizeof(title),labels[index]);
            char *newline=strchr(title,'\n');if(newline)*newline=0;
            lv_obj_set_height(s_row_text[slot],18);set_label_text(s_row_text[slot],title);
            lv_obj_align(s_row_text[slot],LV_ALIGN_TOP_LEFT,10,3);
            lv_obj_remove_flag(s_row_subtext[slot],LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_size(s_row_subtext[slot],196,18);
            lv_obj_set_style_text_color(s_row_subtext[slot],lv_color_hex(FEISHU_MUTED),0);
            set_label_text(s_row_subtext[slot],detail+1);
            lv_obj_align(s_row_subtext[slot],LV_ALIGN_TOP_LEFT,10,23);
        } else {set_label_text(s_row_text[slot],labels[index]);lv_obj_align(s_row_text[slot],LV_ALIGN_LEFT_MID,10,0);}
        set_label_text(s_row_mark[slot],"");lv_obj_set_style_bg_opa(s_row_mark[slot],LV_OPA_TRANSP,0);
    }
}

static void render_chat_rows(void)
{
    static const uint32_t avatar_colors[] = {
        0x1689E8, 0x7557D9, 0x20A779, 0xD18A18,
    };
    set_label_text(s_title, "飞书 | 会话");
    size_t start = visible_start(s_state.nav.chat_index, s_state.nav.chat_count);
    for (size_t slot = 0; slot < FEISHU_VISIBLE_ROWS; ++slot) {
        lv_obj_set_pos(s_rows[slot], 8, FEISHU_CONTENT_TOP + (int)slot * FEISHU_CHAT_STRIDE);
        lv_obj_set_size(s_rows[slot], 224, FEISHU_CHAT_HEIGHT);
        size_t index = start + slot;
        if (index >= s_state.nav.chat_count) {
            lv_obj_add_flag(s_rows[slot], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(s_rows[slot], LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_row_avatar[slot], LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(s_row_avatar[slot],LV_ALIGN_LEFT_MID,5,0);
        lv_obj_set_size(s_row_avatar[slot], 28, 28);
        lv_obj_set_style_bg_color(s_row_avatar[slot],
            lv_color_hex(avatar_colors[index % 4]), 0);
        char avatar[5];
        feishu_avatar_text(avatar, s_state.chats[index].name,
                         s_state.chats[index].p2p ? "聊" : "群");
        set_label_text(s_row_avatar_text[slot], avatar);
        lv_obj_center(s_row_avatar_text[slot]);
        lv_label_set_long_mode(s_row_text[slot],LV_LABEL_LONG_DOT);
        lv_obj_align(s_row_text[slot],LV_ALIGN_LEFT_MID,42,0);
        lv_obj_set_size(s_row_text[slot], 164, 20);
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
    uint16_t heights[FEISHU_MAX_MESSAGES];
    for(size_t i=0;i<s_state.nav.message_count;++i) {
        char preview[91];
        feishu_text_preview(preview,sizeof(preview),s_state.messages[i].text,
            s_state.messages[i].text_truncated,22,2);
        heights[i]=strchr(preview,'\n') ? 44:28;
    }
    size_t start=0;
    size_t visible=feishu_message_window(heights,s_state.nav.message_count,
        s_state.nav.composer_selected ? s_state.nav.message_count:s_state.nav.message_index,
        190,FEISHU_MESSAGE_ROWS,&start);
    int y=38;
    for (size_t slot = 0; slot < FEISHU_MESSAGE_ROWS; ++slot) {
        size_t index = start + slot;
        if (slot>=visible) {
            lv_obj_add_flag(s_rows[slot], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        unsigned height=heights[index];
        lv_obj_set_style_text_line_space(s_row_text[slot],3,0);
        lv_obj_set_pos(s_rows[slot],10,y);lv_obj_set_size(s_rows[slot],220,height);
        y+=(int)height+4;
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
            lv_obj_set_pos(s_row_avatar[slot], 4, ((int)height-26)/2);
            lv_obj_set_size(s_row_avatar[slot], 26, 26);
            lv_obj_set_style_bg_color(s_row_avatar[slot],
                                      lv_color_hex(FEISHU_SURFACE_FOCUS), 0);
            char avatar[5];
            feishu_avatar_text(avatar, s_state.messages[index].sender_name, "他");
            set_label_text(s_row_avatar_text[slot], avatar);
        lv_obj_center(s_row_avatar_text[slot]);
        }
        lv_obj_align(s_row_text[slot],LV_ALIGN_TOP_LEFT,mine ? 44 : 40,0);
        lv_obj_set_size(s_row_text[slot], mine ? 168 : 172, height);
        lv_obj_set_style_bg_opa(s_row_text[slot], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s_row_text[slot],
            lv_color_hex(mine ? 0x24504D : FEISHU_SURFACE), 0);
        lv_obj_set_style_text_color(s_row_text[slot],
            lv_color_hex(FEISHU_TEXT), 0);
        lv_obj_set_style_text_align(s_row_text[slot],
            mine ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_pad_all(s_row_text[slot], 2, 0);
        lv_obj_set_style_text_line_space(s_row_text[slot],1,0);
        lv_obj_set_style_radius(s_row_text[slot], 8, 0);
        lv_obj_set_style_border_width(s_row_text[slot],
                                      !s_state.nav.composer_selected && index == s_state.nav.message_index ? 2 : 0, 0);
        lv_obj_set_style_border_color(s_row_text[slot],
                                      lv_color_hex(FEISHU_FOCUS), 0);
        set_label_text(s_row_mark[slot], "");
        lv_obj_set_style_bg_opa(s_row_mark[slot], LV_OPA_TRANSP, 0);
        // The bubble itself already has the yellow focus outline. Keep the
        // surrounding message row transparent to avoid a double card.
        lv_obj_set_style_bg_opa(s_rows[slot], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_rows[slot], 0, 0);
        // style_list_row sets the default text color; restore bubble contrast.
        lv_obj_set_style_text_color(s_row_text[slot], lv_color_hex(FEISHU_TEXT), 0);
    }
    for(size_t i=FEISHU_MESSAGE_ROWS+1;i<FEISHU_VISIBLE_ROWS;++i)lv_obj_add_flag(s_rows[i],LV_OBJ_FLAG_HIDDEN);
    size_t slot = FEISHU_MESSAGE_ROWS;
    lv_obj_remove_flag(s_rows[slot], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_row_avatar[slot], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_row_subtext[slot], LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_rows[slot], 10, 234);
    lv_obj_set_size(s_rows[slot], 220, 32);
    lv_obj_set_style_bg_opa(s_rows[slot], LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_rows[slot], lv_color_hex(FEISHU_SURFACE), 0);
    lv_obj_set_style_border_width(s_rows[slot],
                                  s_state.nav.composer_selected ? 2 : 1, 0);
    lv_obj_set_style_border_color(s_rows[slot], lv_color_hex(
        s_state.nav.composer_selected ? FEISHU_FOCUS : FEISHU_RULE), 0);
    lv_obj_set_style_radius(s_rows[slot], 12, 0);
    lv_obj_set_width(s_row_text[slot],188);
    lv_obj_set_height(s_row_text[slot],LV_SIZE_CONTENT);
    lv_obj_align(s_row_text[slot],LV_ALIGN_CENTER,0,0);
    lv_obj_set_style_bg_opa(s_row_text[slot], LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_row_text[slot], 0, 0);
    lv_obj_set_style_pad_all(s_row_text[slot], 0, 0);
    lv_obj_set_style_text_align(s_row_text[slot], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_row_text[slot], lv_color_hex(FEISHU_TEXT), 0);
    lv_label_set_long_mode(s_row_text[slot],LV_LABEL_LONG_CLIP);
    set_label_text(s_row_text[slot], "发消息");
    set_label_text(s_row_mark[slot], "");
    lv_obj_set_style_bg_opa(s_row_mark[slot], LV_OPA_TRANSP, 0);
}

static bool pairing_console_qr(bool visible)
{
    if(!visible) {
        if(s_pair_qr)lv_obj_delete(s_pair_qr);
        if(s_pair_caption)lv_obj_delete(s_pair_caption);
        s_pair_qr=s_pair_caption=NULL;return true;
    }
    if(s_pair_qr)return true;
    s_pair_qr=lv_image_create(s_scr);
    if(!s_pair_qr)return false;
    lv_image_set_src(s_pair_qr,&feishu_xiao_console_qr);
    lv_obj_set_size(s_pair_qr,feishu_xiao_console_qr.header.w,feishu_xiao_console_qr.header.h);
    lv_obj_set_pos(s_pair_qr,46,86);
    lv_obj_set_style_pad_all(s_pair_qr,0,0);
    lv_obj_set_style_border_width(s_pair_qr,0,0);lv_obj_set_style_radius(s_pair_qr,8,0);
    s_pair_caption=lv_label_create(s_scr);
    if(!s_pair_caption){lv_obj_delete(s_pair_qr);s_pair_qr=NULL;return false;}
    lv_obj_set_style_text_font(s_pair_caption,&lv_font_ai_passport_14,0);
    lv_obj_set_style_text_color(s_pair_caption,lv_color_hex(FEISHU_MUTED),0);
    lv_label_set_text(s_pair_caption,"扫码打开小智控制台");
    lv_obj_align_to(s_pair_caption,s_pair_qr,LV_ALIGN_OUT_BOTTOM_MID,0,12);
    return true;
}

static bool render_startup(void)
{
    if(!s_state_lock)return false;
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    bool show=s_state.startup_pending && s_state.nav.page==FEISHU_PAGE_CHATS;
    xSemaphoreGive(s_state_lock);
    if(!show){if(s_startup_cover)lv_obj_delete(s_startup_cover);s_startup_cover=s_startup_logo=NULL;return false;}
    if(!s_startup_cover){
        s_startup_cover=lv_obj_create(s_scr);if(!s_startup_cover)return false;
        lv_obj_remove_flag(s_startup_cover,LV_OBJ_FLAG_SCROLLABLE);lv_obj_set_size(s_startup_cover,240,320);
        lv_obj_set_pos(s_startup_cover,0,0);lv_obj_set_style_pad_all(s_startup_cover,0,0);
        lv_obj_set_style_border_width(s_startup_cover,0,0);lv_obj_set_style_radius(s_startup_cover,0,0);
        lv_obj_set_style_bg_color(s_startup_cover,lv_color_hex(FEISHU_BG),0);lv_obj_set_style_bg_opa(s_startup_cover,LV_OPA_COVER,0);
        s_startup_logo=lv_label_create(s_startup_cover);
        if(!s_startup_logo){lv_obj_delete(s_startup_cover);s_startup_cover=NULL;return false;}
    }
    feishu_startup_label(s_startup_logo,lv_tick_get());return true;
}

static void render(lv_timer_t *timer)
{
    if(feishu_power_screen_off())return;

    (void)timer;
    if(render_startup())return;
#ifdef FEISHU_SCENARIO_AUTOTEST
    unsigned cancel_at=atomic_load(&s_scenario_cancel_at);
    if(cancel_at && (int32_t)((unsigned)(esp_timer_get_time()/1000)-cancel_at)>=0){
        atomic_store(&s_scenario_cancel_at,0);demo_feishu_back();atomic_fetch_add(&s_scenario_back_count,1);
        ESP_LOGI(TAG,"ASSISTANT_SCENARIO_BACK ui_task=1");
    }
#endif
#ifdef FEISHU_LVGL_RECOVERY_AUTOTEST
    atomic_fetch_add(&s_ui_heartbeat,1);
#endif
    char body[1200] = { 0 };
    char hint[96] = { 0 };
    bool detail_view=false,request_pairing=false,authorization_loading=false;
    uint32_t notice_color=FEISHU_SUCCESS;
    uint32_t detail_page=0;int64_t detail_id=0;int32_t detail_scroll=0;
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    static feishu_page_t power_seen_page=FEISHU_PAGE_STATUS;
    bool entered=power_seen_page!=s_state.nav.page;power_seen_page=s_state.nav.page;
    if(entered && s_state.ready && !s_state.loading && (s_state.nav.page==FEISHU_PAGE_CHATS || s_state.nav.page==FEISHU_PAGE_MESSAGES) &&
       !atomic_exchange(&s_front_queued,true)){
        work_command_t refresh=WORK_FRONT_REFRESH;if(xQueueSend(s_work_queue,&refresh,0)!=pdTRUE)atomic_store(&s_front_queued,false);
    }
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
    uint32_t assistant_now=(uint32_t)(esp_timer_get_time()/1000);
    bool assistant_busy=s_state.assistant_speech_detected || s_state.assistant_phase==FEISHU_ASSISTANT_PROCESSING ||
        s_state.assistant_phase==FEISHU_ASSISTANT_SPEAKING;
    if(!s_state.assistant_timed_out && feishu_assistant_idle_expired(assistant_now,&s_state.assistant_activity_ms,
        s_state.assistant_settings.timeout_minutes,s_state.assistant_enabled || s_state.assistant_active || s_state.assistant_requested,assistant_busy)){
        bool owns_audio=s_state.assistant_active || s_state.assistant_requested;
        s_state.assistant_timed_out=true;s_state.assistant_phase=FEISHU_ASSISTANT_STOPPING;s_state.assistant_enabled=s_state.assistant_requested=false;
        s_state.assistant_exit_requested=true;
        if(owns_audio){
            s_state.stop_recording=s_state.cancel_recording=true;
            feishu_utf8_copy(s_state.status,sizeof(s_state.status),"小智已超时退出");s_state.status_error=false;
        }
        ESP_LOGI(TAG,"ASSISTANT_TIMEOUT minutes=%u owning_audio=%u",s_state.assistant_settings.timeout_minutes,(unsigned)owns_audio);
    }
    unsigned submit_seconds=s_state.assistant_settings.silence_seconds;
    bool submit_manual=s_state.nav.page==FEISHU_PAGE_REVIEW || s_state.nav.page==FEISHU_PAGE_ASSISTANT;
    atomic_store(&s_assistant_submit_seconds,submit_seconds || submit_manual ? submit_seconds:2);
    if(s_assistant_badge){
        bool active=!s_state.assistant_timed_out && (s_state.assistant_enabled || s_state.assistant_active || s_state.assistant_requested);
        lv_color_t color=lv_color_hex(active ? 0x7EE2D2:0x657782);
        if(lv_color_to_u32(lv_obj_get_style_text_color(s_assistant_badge,0))!=lv_color_to_u32(color))
            lv_obj_set_style_text_color(s_assistant_badge,color,0);
        unsigned pulse=feishu_assistant_breath(assistant_now);
        lv_opa_t opacity=active ? (lv_opa_t)feishu_assistant_breath_opacity(assistant_now):LV_OPA_COVER;
        if(s_notice_listening && active && !s_state.stop_recording){
            char dots[4];snprintf(dots,sizeof(dots),"%.*s",(int)feishu_assistant_breath_dots(assistant_now),"...");
            set_label_text(s_notice_dots,dots);
            lv_obj_set_style_text_opa(s_notice_dots,(lv_opa_t)(96u+pulse*159u/1000u),0);
            lv_obj_set_style_text_opa(s_notice,(lv_opa_t)(96u+pulse*159u/1000u),0);
        }
        if(lv_obj_get_style_text_opa(s_assistant_badge,0)!=opacity)lv_obj_set_style_text_opa(s_assistant_badge,opacity,0);
    }
    // Style setters invalidate LVGL objects even when their values are equal.
    // Avoid repainting an unchanged page every 100 ms on the single-core C3.
    // This signature is only a paint optimization, not a storage/scope check.
    static uint64_t painted;
    static uint32_t painted_generation;
    static bool paint_valid;
    if(s_state.nav.page==FEISHU_PAGE_XIAOZHI && s_state.pairing.code[0] && s_state.pairing.expires_at_ms &&
       (int32_t)((uint32_t)(esp_timer_get_time()/1000)-s_state.pairing.expires_at_ms)>=0)s_state.pairing.code[0]=0;
    uint32_t pairing_now=(uint32_t)(esp_timer_get_time()/1000);
    if(feishu_pairing_auto_due(pairing_now,s_state.ready && !s_state.loading && !s_state.closing && s_state.nav.page==FEISHU_PAGE_XIAOZHI &&
        !s_state.cancel_pairing && !s_state.pairing_auto_blocked,s_state.pairing_busy,s_state.pairing.paired,
        s_state.pairing.code[0]!=0,s_state.pairing.expires_at_ms,s_state.pairing_retry_at)) {
        s_state.pairing_busy=true;s_state.status_error=false;++s_state.pairing_request;
        feishu_utf8_copy(s_state.status,sizeof(s_state.status),"正在获取最新设备码…");request_pairing=true;
    }
    bool stable=s_state.nav.page!=FEISHU_PAGE_RECORDING &&
        s_state.nav.page!=FEISHU_PAGE_TRANSCRIBING && !s_state.sending &&
        (!s_state.record_pending || s_state.assistant_active);
    uint64_t signature=UINT64_C(14695981039346656037);
    if(stable) {
        const unsigned char *bytes=(const unsigned char *)&s_state;
        for(size_t i=0;i<sizeof(s_state);++i){
            /* The capture clock is invisible on business pages. Hashing it
             * forced a full list/font layout on every frame, starving upload. */
            if(s_state.nav.page!=FEISHU_PAGE_ASSISTANT &&
               i>=offsetof(feishu_ui_state_t,elapsed_ms) &&
               i<offsetof(feishu_ui_state_t,elapsed_ms)+sizeof(s_state.elapsed_ms))continue;
            signature=(signature^bytes[i])*UINT64_C(1099511628211);
        }
        /* Network/editor state is page-owned rather than part of s_state.
         * Cursor and password edits must also invalidate the paint signature. */
        if(s_network_ui && network_page(s_state.nav.page)) {
            bytes=(const unsigned char *)s_network_ui;
            for(size_t i=0;i<sizeof(*s_network_ui);++i)signature=(signature^bytes[i])*UINT64_C(1099511628211);
        }
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
        snprintf(body, sizeof(body), "%s", s_state.status);
        snprintf(hint, sizeof(hint), "%s", s_state.loading ? "请稍候" : "长按确定：返回");
    } else if (s_state.loading && s_state.nav.page!=FEISHU_PAGE_MESSAGES &&
               s_state.nav.page!=FEISHU_PAGE_CHATS && !(s_state.sending && s_state.nav.page==FEISHU_PAGE_REVIEW)) {
        show_list_rows(false);
        set_label_text(s_title, s_state.nav.page==FEISHU_PAGE_DRAFTS ? "草稿管理":
            s_state.nav.page==FEISHU_PAGE_REVIEW ? "协同编辑":s_state.nav.page==FEISHU_PAGE_ACCOUNT ? "BOT授权":"飞书");
        snprintf(body, sizeof(body), "%s",s_state.nav.page==FEISHU_PAGE_ACCOUNT ? "正在准备授权二维码…":"正在加载…");
        snprintf(hint, sizeof(hint), "请稍候");
    } else {
        switch (s_state.nav.page) {
        case FEISHU_PAGE_CHATS:
            show_list_rows(s_state.nav.chat_count>0);
            if(s_state.nav.chat_count)render_chat_rows();
            else snprintf(body,sizeof(body),"暂无会话\n\n正在检查最新会话");
            char notice[25];
            feishu_utf8_copy(notice,sizeof(notice),s_state.status[0] ? s_state.status:s_state.recovery_generation ? "存储隔离待恢复":"上下选 确定进");
            snprintf(hint,sizeof(hint),"%lu页%s %s\n长按上下翻页/确定设置",
                (unsigned long)s_state.chat_page_number+1,s_state.chat_page_more ? "+":"",notice);
            break;
        case FEISHU_PAGE_MESSAGES: {
            bool same_chat=s_state.nav.chat_index<s_state.nav.chat_count &&
                !strcmp(s_state.loaded_chat,s_state.chats[s_state.nav.chat_index].chat_id);
            bool show_messages=!s_state.loading || same_chat;
            show_list_rows(show_messages);
            if(show_messages)render_message_rows();
            else {set_label_text(s_title,"会话消息");snprintf(body,sizeof(body),"正在加载消息…");}

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
        }
        case FEISHU_PAGE_MESSAGE_DETAIL:
            show_list_rows(false);
            set_label_text(s_title, "消息详情");
            if (s_state.nav.message_index < s_state.nav.message_count) {
                const feishu_message_t *message =
                    &s_state.messages[s_state.nav.message_index];
                char sender[49];
                char detail[FEISHU_TEXT_PAGE_CAP];
                feishu_utf8_copy(sender, sizeof(sender),
                    message->mine ? "我" : message->sender_name);
                feishu_utf8_copy(detail,sizeof(detail),s_state.detail_loaded ? s_state.detail_text : message->text);
                lv_label_set_text_fmt(s_title,"%s | 消息",sender);
                snprintf(body,sizeof(body),"%s",detail);
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
                snprintf(body, sizeof(body), "%s", s_state.status);
                snprintf(hint,sizeof(hint),"确定操作  长按返回");
            }
            break;
        case FEISHU_PAGE_IMAGE_ACTIONS: {
            set_label_text(s_title,"图片操作");
            static const char *labels[]={"回复此图","放大","缩小","切换移动方向","重试预览","返回图片"};
            render_choice_rows(labels,6,s_state.nav.settings_index,false);
            snprintf(hint,sizeof(hint),"上下选择  确定  长按返回");break;
        }
        case FEISHU_PAGE_RECORDING:
            show_list_rows(false);
            set_label_text(s_title,s_state.record_pending ? "准备录音":"语音输入 | 录音中");
            if(s_state.record_pending)snprintf(body,sizeof(body),"正在准备麦克风…");
            else snprintf(body,sizeof(body),"正在聆听  %u.%u秒",
                s_state.elapsed_ms/1000,(s_state.elapsed_ms%1000)/100);
            snprintf(hint,sizeof(hint),"确定提交  说再见退出  长按取消");
            break;
        case FEISHU_PAGE_TRANSCRIBING:
            show_list_rows(false);set_label_text(s_title,"正在识别语音");
            snprintf(body,sizeof(body),"正在整理识别文字\n\n完成后请先检查正文");
            snprintf(hint,sizeof(hint),"长按取消");
            break;
        case FEISHU_PAGE_REVIEW: {
            show_list_rows(false);
            set_label_text(s_title,s_state.pending_slot==RAM_REPLY_SLOT ? "确认消息":"审阅草稿");
            set_label_text(s_title,s_state.compose_send_confirm ? (s_state.pending.message[0] ? "确认回复":"确认发送"):
                s_state.assistant_composing ? (s_state.pending.message[0] ? "协作回复":"协作草稿"):"口述草稿");
            const char *recipient=s_state.pending.chat;
            for(size_t i=0;i<s_state.nav.chat_count;++i)if(!strcmp(s_state.pending.chat,s_state.chats[i].chat_id)){recipient=s_state.chats[i].name;break;}
            char target[96];feishu_text_preview(target,sizeof(target),recipient,false,24,1);
            snprintf(body,sizeof(body),"%s：%s\n\n%s",s_state.pending.message[0] ? "引用回复到":"发送到",target,s_state.assistant_composing && !s_state.pending.length ? "先说说想写什么，一起商量修改":s_state.detail_loaded ? s_state.detail_text:s_state.reply);
            if(s_state.detail_pages>1)snprintf(hint,sizeof(hint),"%lu/%lu页  上下翻页\n确定操作  长按确定保留返回",
                (unsigned long)s_state.detail_page+1,(unsigned long)s_state.detail_pages);
            else if(s_state.pending_slot==RAM_REPLY_SLOT)snprintf(hint,sizeof(hint),
                s_state.detail_scroll_max>0 ? "上下滚动 确定操作\n长按确定保留返回":"确定发送 上重录 下删除\n长按确定保留返回");
            else snprintf(hint,sizeof(hint),"确定发送 上重录 下删除\n长按确定保留返回");
            if(s_state.assistant_composing)snprintf(hint,sizeof(hint),"确定确认发送");
            if(s_state.compose_send_confirm)snprintf(hint,sizeof(hint),"再次确定发送  长按确定取消");
            if(s_state.sending)set_label_text(s_title,"正在发送");
            detail_view=true;detail_page=s_state.detail_page;detail_id=s_state.pending.object_id;
            detail_scroll=s_state.detail_scroll;
            break;
        }
        case FEISHU_PAGE_DRAFT_ACTIONS: {
            set_label_text(s_title,"草稿操作");
            const char *labels[]={s_state.pending_retry_warning ? "仍要重试（可能重复）":"发送全文（必要时分段）","重新录音","删除草稿"};
            render_choice_rows(labels,3,s_state.nav.settings_index,false);
            snprintf(hint,sizeof(hint),"上下选择  确定  长按返回审阅");break;
        }
        case FEISHU_PAGE_STATUS:
            show_list_rows(false);
            snprintf(body, sizeof(body), "%s", s_state.status);
            snprintf(hint, sizeof(hint), "长按确定：返回消息");
            break;
        case FEISHU_PAGE_DRAFTS:
            set_label_text(s_title,"草稿管理");
            if(s_state.nav.draft_count) {
                const char *labels[FEISHU_OUTBOX_SLOTS];
                for(size_t i=0;i<FEISHU_OUTBOX_SLOTS;++i)labels[i]=s_state.draft_labels[i];
                render_choice_rows(labels,s_state.nav.draft_count>FEISHU_OUTBOX_SLOTS ? FEISHU_OUTBOX_SLOTS:s_state.nav.draft_count,s_state.nav.draft_index,true);
            } else {show_list_rows(false);snprintf(body,sizeof(body),"没有待发送草稿");}
            lv_obj_remove_flag(s_rows[5],LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(s_rows[5],10,243);lv_obj_set_size(s_rows[5],220,25);
            style_list_row(5,s_state.nav.draft_index==s_state.nav.draft_count);
            lv_obj_add_flag(s_row_avatar[5],LV_OBJ_FLAG_HIDDEN);lv_obj_add_flag(s_row_subtext[5],LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_size(s_row_text[5],196,18);set_label_text(s_row_text[5],"清空草稿");
            lv_obj_align(s_row_text[5],LV_ALIGN_LEFT_MID,10,0);set_label_text(s_row_mark[5],"");
            snprintf(hint,sizeof(hint),"上下选择  确定操作  长按返回");break;
        case FEISHU_PAGE_NETWORK:
        case FEISHU_PAGE_NETWORK_SCAN:
        case FEISHU_PAGE_NETWORK_PASSWORD:
        case FEISHU_PAGE_NETWORK_ACTIONS:
            network_render(body,sizeof(body),hint,sizeof(hint));break;
        case FEISHU_PAGE_SETTINGS: {
            set_label_text(s_title,"设置");
            static const char *names[FEISHU_SETTING_COUNT]={
                [FEISHU_SETTING_ACCOUNT]="飞书账号",[FEISHU_SETTING_XIAOZHI]="小智设置",
                [FEISHU_SETTING_ASSISTANT]="小智助手",
                [FEISHU_SETTING_NETWORK]="网络连接",[FEISHU_SETTING_DRAFTS]="草稿管理",[FEISHU_SETTING_RETURN]="返回会话",[FEISHU_SETTING_SOUNDS]="音效设置",[FEISHU_SETTING_ABOUT]="关于",[FEISHU_SETTING_POWER]="省电设置"};
            render_choice_rows(names,FEISHU_SETTING_COUNT,s_state.nav.settings_index,false);
            snprintf(hint,sizeof(hint),"上下选择  确定打开  长按返回");break;
        }
        case FEISHU_PAGE_POWER_SETTINGS:power_render(hint,sizeof(hint));break;
        case FEISHU_PAGE_ABOUT:
            show_list_rows(false);set_label_text(s_title,"关于");
            snprintf(body,sizeof(body),"飞书终端\n\n版本  %s\n\n作者\ncexer@qq.com",esp_app_get_description()->version);
            snprintf(hint,sizeof(hint),"长按确定返回");break;
        case FEISHU_PAGE_SOUND_SETTINGS: {
            set_label_text(s_title,"音效设置");char a[48],b[48],c[48];
            snprintf(a,sizeof(a),"成功音    %s",s_state.sounds.success ? "开启":"关闭");
            snprintf(b,sizeof(b),"失败音    %s",s_state.sounds.failure ? "开启":"关闭");
            snprintf(c,sizeof(c),"按键音    %s",s_state.sounds.key ? "开启":"关闭");
            const char *rows[]={a,b,c};render_choice_rows(rows,3,s_state.nav.settings_index,false);
            snprintf(hint,sizeof(hint),"上下选择  确定切换  长按返回");break;
        }
        case FEISHU_PAGE_XIAOZHI_SETTINGS: {
            set_label_text(s_title,"小智设置");
            char volume[48],timeout[48];snprintf(volume,sizeof(volume),"音量    %u%%",s_state.assistant_settings.volume);
            if(s_state.assistant_settings.timeout_minutes)snprintf(timeout,sizeof(timeout),"空闲超时    %u分钟",s_state.assistant_settings.timeout_minutes);
            else snprintf(timeout,sizeof(timeout),"激活超时    不自动退出");
            const char *names[]={"账号设置","助手设置",volume,timeout};
            render_choice_rows(names,4,s_state.nav.settings_index,false);
            snprintf(hint,sizeof(hint),s_state.assistant_settings_editing ? "上下调整  确定保存  长按返回":"上下选择  确定打开  长按返回");break;
        }
        case FEISHU_PAGE_ASSISTANT_SETTINGS: {
            set_label_text(s_title,"助手设置");
            char boot[80],timing[80];unsigned seconds=s_state.assistant_settings.silence_seconds;
            snprintf(boot,sizeof(boot),"开机激活助手    %s",s_state.assistant_settings.boot ? "开启":"关闭");
            if(seconds)snprintf(timing,sizeof(timing),"命令发送时机    %u秒",seconds);
            else snprintf(timing,sizeof(timing),"命令发送时机    OK");
            const char *names[]={boot,timing};render_choice_rows(names,2,s_state.nav.settings_index,false);
            snprintf(hint,sizeof(hint),s_state.assistant_settings_editing ? "上下调整  确定保存  长按返回":"上下选择  确定修改  长按返回");break;
        }
        case FEISHU_PAGE_ASSISTANT:
            show_list_rows(false);set_label_text(s_title,"小智助手");
            if(!s_state.assistant_active && !s_state.assistant_heard[0] && !s_state.assistant_reply[0])
                snprintf(body,sizeof(body),"语音操作飞书\n\n查询会话 · 读取消息\n打开界面 · 准备草稿\n\n消息需在设备确认后发送");
            else if(s_state.assistant_active && !s_state.stop_recording)
                snprintf(body,sizeof(body),s_state.record_pending ? "正在准备麦克风…":"正在聆听  %u秒",s_state.elapsed_ms/1000);
            else if(!s_state.assistant_heard[0] && s_state.assistant_reply[0])snprintf(body,sizeof(body),"小智\n\n%s",s_state.assistant_reply);
            else if(!s_state.assistant_heard[0])snprintf(body,sizeof(body),"正在连接小智…");
            else if(!s_state.assistant_reply[0])snprintf(body,sizeof(body),"你说\n\n%s",s_state.assistant_heard);
            else snprintf(body,sizeof(body),"你说：\n%s\n\n小智：\n%s",s_state.assistant_heard,s_state.assistant_reply);
            if(!s_state.assistant_active)snprintf(hint,sizeof(hint),"确定重新进入  长按返回");
            else if(s_state.stop_recording)snprintf(hint,sizeof(hint),"长按退出");
            else if(s_state.assistant_settings.silence_seconds)snprintf(hint,sizeof(hint),"确定或停顿%u秒提交  长按退出",s_state.assistant_settings.silence_seconds);
            else snprintf(hint,sizeof(hint),"确定提交  长按退出");
            break;
        case FEISHU_PAGE_CONFIRM:
            show_list_rows(false);
            if(s_state.nav.confirm_action==4){
                set_label_text(s_title,"清空草稿");snprintf(body,sizeof(body),"删除所有本地草稿？\n\n已经发出的飞书消息不会撤回");
                snprintf(hint,sizeof(hint),"确定清空  上下取消");break;
            }
            set_label_text(s_title,"确认操作");
            snprintf(body, sizeof(body), "%s\n\n此操作完成后设备会重启",
                     s_state.nav.confirm_action == 1 ?
                     "重新配置 Wi-Fi？\n飞书绑定会保留" :
                     s_state.nav.confirm_action==2 ? "更换BOT或账号？\n完成前保留原绑定及草稿" :
                     "重新授权当前BOT？\nBOT和 Wi-Fi 会保留");
            snprintf(hint, sizeof(hint), "确定继续  上/下取消");
            break;
        case FEISHU_PAGE_XIAOZHI: {
            show_list_rows(false);set_label_text(s_title,"账号设置");
            bool expired=s_state.pairing.expires_at_ms && (int32_t)((uint32_t)(esp_timer_get_time()/1000)-s_state.pairing.expires_at_ms)>=0;
            if(s_state.pairing.paired)snprintf(body,sizeof(body),"设备 ID\n%s",s_state.pairing.device_id);
            else if(s_state.pairing.code[0] && !expired)snprintf(body,sizeof(body),"设备配对码\n%s",s_state.pairing.code);
            else snprintf(body,sizeof(body),"小智配置后台");
            snprintf(hint,sizeof(hint),"确定刷新  长按返回");break;
        }
        case FEISHU_PAGE_ACCOUNT: {
            set_label_text(s_title,"飞书账号");
            static const char *labels[]={"BOT授权","选择或新建BOT","返回设置"};
            render_choice_rows(labels,3,s_state.nav.settings_index,false);
            snprintf(hint,sizeof(hint),"上下选择  确定  长按返回");break;
        }
        default:
            break;
        }
    }
    char footer_notice[96],footer_hint[96];
    feishu_utf8_copy(footer_notice,sizeof(footer_notice),s_state.status);
    bool notice_error=s_state.status_error;
    if(!footer_notice[0])feishu_utf8_copy(footer_notice,sizeof(footer_notice),
        s_state.nav.page==FEISHU_PAGE_MESSAGES ? "双按上到消息，双按下到发送":s_state.nav.page==FEISHU_PAGE_CHATS ? "会话已加载":"");
    if(!notice_error){
        const char *ordinary[]={"会话已加载","缓存","缓存，后台更新中","后台更新中","缓存列表","缓存历史","缓存预览"};
        for(unsigned i=0;i<sizeof(ordinary)/sizeof(*ordinary);++i)if(!strcmp(footer_notice,ordinary[i])){
            feishu_utf8_copy(footer_notice,sizeof(footer_notice),s_state.assistant_enabled || s_state.assistant_active || s_state.assistant_requested ?
                feishu_assistant_phase_notice(s_state.assistant_phase):"双按确定激活小智");break;
        }
    }
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
    case FEISHU_PAGE_POWER_SETTINGS:snprintf(footer_hint,sizeof(footer_hint),s_power_editing ? "上下调整  确定保存  长按返回":"上下选择  确定修改  长按返回");break;
    case FEISHU_PAGE_ABOUT:snprintf(footer_hint,sizeof(footer_hint),"长按确定返回");break;
    case FEISHU_PAGE_SETTINGS:snprintf(footer_hint,sizeof(footer_hint),"上下选择  确定打开  长按返回");break;
    case FEISHU_PAGE_DRAFTS:snprintf(footer_hint,sizeof(footer_hint),"%u/%u 上下选择  确定审阅  长按返回",s_state.nav.draft_count && s_state.nav.draft_index<s_state.nav.draft_count ? (unsigned)s_state.nav.draft_index+1:0,(unsigned)s_state.nav.draft_count);break;
    case FEISHU_PAGE_DRAFT_ACTIONS:case FEISHU_PAGE_IMAGE_ACTIONS:case FEISHU_PAGE_ACCOUNT:
        snprintf(footer_hint,sizeof(footer_hint),"上下选择  确定  长按返回");break;
    case FEISHU_PAGE_CONFIRM:snprintf(footer_hint,sizeof(footer_hint),s_state.nav.confirm_action==4 ? "确定清空  上下取消":"确定继续  上下取消");break;
    case FEISHU_PAGE_IMAGE:snprintf(footer_hint,sizeof(footer_hint),
        s_state.image_ready ? "确定操作  长按上下缩放  长按返回":"确定操作  长按返回");break;
    case FEISHU_PAGE_XIAOZHI:snprintf(footer_hint,sizeof(footer_hint),"确定刷新  长按返回");break;
    case FEISHU_PAGE_NETWORK:
    case FEISHU_PAGE_NETWORK_SCAN:
    case FEISHU_PAGE_NETWORK_ACTIONS:
        snprintf(footer_hint,sizeof(footer_hint),"上下选择  确定打开  长按返回");break;
    case FEISHU_PAGE_NETWORK_PASSWORD:
        snprintf(footer_hint,sizeof(footer_hint),"上下选键  确定输入  长按返回");break;
    default:feishu_text_preview(footer_hint,sizeof(footer_hint),hint,false,32,1);break;
    }
    if(!s_state.ready || s_state.loading)snprintf(footer_hint,sizeof(footer_hint),"正在准备，请稍候");
    if(s_state.assistant_active && s_state.assistant_speech_detected && s_state.nav.page!=FEISHU_PAGE_REVIEW){
        unsigned seconds=s_state.assistant_settings.silence_seconds;
        if(s_state.nav.page==FEISHU_PAGE_CHATS)
            snprintf(footer_hint,sizeof(footer_hint),"停顿%u秒提交  长按确定设置",seconds ? seconds:2);
        else if(s_state.nav.page==FEISHU_PAGE_ASSISTANT)
            snprintf(footer_hint,sizeof(footer_hint),seconds ? "确定或停顿%u秒提交  长按返回":"确定提交指令  长按返回",seconds);
    }
    if(s_state.nav.page==FEISHU_PAGE_REVIEW){
        if(s_state.compose_send_confirm)snprintf(footer_hint,sizeof(footer_hint),s_state.assistant_active ? "说确认发送或按确定  长按取消":"再次确定发送  长按确定取消");
        else if(s_state.assistant_composing){
            if(s_state.assistant_speech_detected){
                unsigned seconds=s_state.assistant_settings.silence_seconds;
                if(seconds)snprintf(footer_hint,sizeof(footer_hint),"确定或停顿%u秒提交",seconds);
                else snprintf(footer_hint,sizeof(footer_hint),"确定提交指令");
            } else snprintf(footer_hint,sizeof(footer_hint),"确定确认发送");
        } else if(s_state.compose_send_confirm)snprintf(footer_hint,sizeof(footer_hint),s_state.assistant_active ? "说确认发送或按确定  长按取消":"再次确定发送  长按确定取消");
    }
    if(s_state.sending){
        feishu_utf8_copy(footer_notice,sizeof(footer_notice),"正在发送消息…");
        snprintf(footer_hint,sizeof(footer_hint),"请稍候  长按确定暂停发送");
    }
    if(!s_state.status[0] && s_state.nav.page==FEISHU_PAGE_REVIEW)
        feishu_utf8_copy(footer_notice,sizeof(footer_notice),s_state.assistant_composing ?
            feishu_assistant_phase_notice(s_state.assistant_phase):
            s_state.compose_send_confirm ? "请核对目标和正文":"上方是待发送正文，请确认");
    if(s_state.nav.page==FEISHU_PAGE_RECORDING && !s_state.status[0])feishu_utf8_copy(footer_notice,sizeof(footer_notice),
        s_state.record_pending ? "正在准备麦克风，请稍候":"正在录音，请说话");
    if(s_state.nav.page==FEISHU_PAGE_XIAOZHI && !s_state.pairing_busy && !s_state.pairing.paired &&
       (!s_state.pairing.code[0] || (s_state.pairing.expires_at_ms && (int32_t)((uint32_t)(esp_timer_get_time()/1000)-s_state.pairing.expires_at_ms)>=0))) {
        if(!notice_error || !footer_notice[0])feishu_utf8_copy(footer_notice,sizeof(footer_notice),
            s_state.pairing.expires_at_ms && (int32_t)((uint32_t)(esp_timer_get_time()/1000)-s_state.pairing.expires_at_ms)>=0 ? "配对码已过期":"尚未获取设备码");
        notice_error=true;
    }
    if(s_state.nav.page==FEISHU_PAGE_ACCOUNT && !s_state.loading)feishu_utf8_copy(footer_notice,sizeof(footer_notice),
        s_state.nav.settings_index==1 ? "选择BOT后继续授权":"授权BOT访问你的飞书数据");
    if(s_state.nav.page==FEISHU_PAGE_DRAFT_ACTIONS && s_state.pending_retry_warning) {
        feishu_utf8_copy(footer_notice,sizeof(footer_notice),"结果未确认，重试可能重复");notice_error=true;
    } else if(s_state.nav.page==FEISHU_PAGE_DRAFT_ACTIONS && s_state.pending.offset && !footer_notice[0])
        feishu_utf8_copy(footer_notice,sizeof(footer_notice),"将继续发送剩余内容");
    if(s_state.nav.page==FEISHU_PAGE_DRAFTS && s_state.recovery_generation) {
        feishu_utf8_copy(footer_notice,sizeof(footer_notice),"旧草稿隔离保留，恢复后查看");notice_error=true;
    }
    bool show_pairing=s_state.nav.page==FEISHU_PAGE_XIAOZHI;
    lv_obj_set_style_text_align(s_content,show_pairing ? LV_TEXT_ALIGN_CENTER:LV_TEXT_ALIGN_LEFT,0);
    if(!pairing_console_qr(show_pairing) && show_pairing)append_line(body,sizeof(body),"\n\n%s",FEISHU_XIAO_CONSOLE_URL);
    feishu_ui_battery(s_page_info,s_state.battery_percent);
    bool draft_view=s_state.nav.page==FEISHU_PAGE_REVIEW || s_state.nav.page==FEISHU_PAGE_DRAFT_ACTIONS || s_state.nav.page==FEISHU_PAGE_DRAFTS;
    bool warning=(s_state.assistant_timed_out && !strcmp(footer_notice,"小智已超时退出")) || (draft_view && (s_state.pending_slot==RAM_REPLY_SLOT || s_state.pending.state==FEISHU_OUTBOX_UNCERTAIN || s_state.pending_retry_warning)) ||
        (!online() && !s_state.loading && (s_state.nav.page==FEISHU_PAGE_CHATS || s_state.nav.page==FEISHU_PAGE_MESSAGES));
    notice_color=notice_error ? FEISHU_ERROR:warning ? FEISHU_FOCUS:FEISHU_SUCCESS;
    authorization_loading=s_state.loading && s_state.nav.page==FEISHU_PAGE_ACCOUNT;
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
    if(authorization_loading)footer_notice[0]=footer_hint[0]=0;
    s_notice_listening=!strncmp(footer_notice,"小智聆听中",strlen("小智聆听中"));
    if(s_notice_listening)lv_obj_remove_flag(s_notice_dots,LV_OBJ_FLAG_HIDDEN);
    else {lv_obj_add_flag(s_notice_dots,LV_OBJ_FLAG_HIDDEN);lv_obj_set_style_text_opa(s_notice,LV_OPA_COVER,0);}
    char notice_line[96];
    feishu_text_preview(notice_line,sizeof(notice_line),footer_notice,false,32,1);
    set_label_text(s_notice,notice_line);
    lv_obj_set_style_text_color(s_notice,lv_color_hex(notice_color),0);
    lv_obj_set_style_text_color(s_notice_dots,lv_color_hex(notice_color),0);
    lv_obj_t *groups[]={s_hint,s_hint_groups[0],s_hint_groups[1]};
    feishu_ui_actions(groups,footer_hint);
    if(request_pairing)enqueue(WORK_CHECK_XIAOZHI);
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
        command==WORK_NETWORK_LIST || command==WORK_LOAD_DRAFT_PAGE || command==WORK_LIST_DRAFTS || command==WORK_OPEN_DRAFT || command==WORK_CHAT_PAGE || command==WORK_MESSAGE_PAGE || command==WORK_LOAD_IMAGE) && s_cache_worker ? s_cache_queue : s_work_queue;
    if(queue && xQueueSend(queue,&command,0)==pdTRUE) return;
    if(s_state_lock) {
        xSemaphoreTake(s_state_lock,portMAX_DELAY);
        s_state.loading=false;
        if(command==WORK_SEND)s_state.sending=false;
        if(command==WORK_CHECK_XIAOZHI) {
            s_state.pairing_busy=false;s_state.status_error=true;
            s_state.pairing_retry_at=(uint32_t)(esp_timer_get_time()/1000)+30000;
        }
        if(command==WORK_ASSISTANT){s_state.assistant_requested=s_state.assistant_active=false;s_state.status_error=true;}
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
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    if(s_state.ready && !s_state.loading && s_state.nav.page==FEISHU_PAGE_CHATS && event==FEISHU_NAV_DOWN){
        int edge=feishu_chat_next_page(s_state.nav.chat_index,s_state.nav.chat_count,s_state.chat_page_more);
        if(edge){
            s_state.status_error=false;
            if(edge>0){s_state.chat_page_direction=1;++s_state.chat_page_request;s_state.loading=true;
                feishu_utf8_copy(s_state.status,sizeof(s_state.status),"正在读取下一页会话…");
            } else feishu_utf8_copy(s_state.status,sizeof(s_state.status),"已到最后一页");
            xSemaphoreGive(s_state_lock);
            if(edge>0){feishu_api_cancel_prefetch();enqueue(WORK_CHAT_PAGE);}
            return;
        }
    }
    if(s_state.nav.page==FEISHU_PAGE_SOUND_SETTINGS){
        bool save=false;
        if(event==FEISHU_NAV_BACK){s_state.nav.page=FEISHU_PAGE_SETTINGS;s_state.nav.settings_index=FEISHU_SETTING_SOUNDS;}
        else if(event==FEISHU_NAV_UP || event==FEISHU_NAV_DOWN)s_state.nav.settings_index=feishu_focus_step(s_state.nav.settings_index,3,event==FEISHU_NAV_UP ? -1:1,true);
        else if(event==FEISHU_NAV_OK){
            bool *value=s_state.nav.settings_index==0 ? &s_state.sounds.success:s_state.nav.settings_index==1 ? &s_state.sounds.failure:&s_state.sounds.key;
            *value=!*value;save=true;
        }
        xSemaphoreGive(s_state_lock);if(save)enqueue(WORK_SOUND_SAVE);return;
    }
    if(s_state.nav.page==FEISHU_PAGE_ASSISTANT && s_state.assistant_active && s_state.assistant_speech_detected && !s_state.stop_recording && event==FEISHU_NAV_OK){
        s_state.stop_recording=true;feishu_utf8_copy(s_state.status,sizeof(s_state.status),"正在提交协作指令…");
        xSemaphoreGive(s_state_lock);return;
    }
    if(s_state.assistant_composing && s_state.assistant_active && s_state.nav.page==FEISHU_PAGE_REVIEW &&
       s_state.compose_send_confirm && s_state.assistant_speech_detected && !s_state.stop_recording && event==FEISHU_NAV_OK){
        s_state.stop_recording=true;xSemaphoreGive(s_state_lock);return;
    }
    if(s_state.assistant_composing && s_state.assistant_active && !s_state.compose_send_confirm && s_state.nav.page==FEISHU_PAGE_REVIEW){
        if(event==FEISHU_NAV_BACK){
            s_state.compose_send_confirm=false;s_state.nav.page=s_state.nav.draft_manager ? FEISHU_PAGE_DRAFTS:FEISHU_PAGE_MESSAGES;
            s_state.loading=true;++s_state.message_page_request;
            bool drafts=s_state.nav.draft_manager;
            xSemaphoreGive(s_state_lock);enqueue(drafts ? WORK_LIST_DRAFTS:WORK_LOAD_MESSAGES);return;
        } else if(event==FEISHU_NAV_OK){
            if(s_state.assistant_speech_detected && !s_state.stop_recording){
                s_state.stop_recording=true;
                feishu_utf8_copy(s_state.status,sizeof(s_state.status),"正在提交协作指令…");
            } else if(s_state.compose_busy || s_state.stop_recording){
                feishu_utf8_copy(s_state.status,sizeof(s_state.status),"小智正在处理，请完成后再确认发送");
            } else if(!s_state.pending.length){
                feishu_utf8_copy(s_state.status,sizeof(s_state.status),"先写好正文，再确认发送");
            } else {
                s_state.compose_send_confirm=true;s_state.stop_recording=true;s_state.status_error=false;
                feishu_utf8_copy(s_state.status,sizeof(s_state.status),"请核对目标和正文，再次确定发送");
            }
        } else if(event==FEISHU_NAV_UP || event==FEISHU_NAV_DOWN){
            int32_t step=event==FEISHU_NAV_UP ? -32:32;
            s_state.detail_scroll+=step;if(s_state.detail_scroll<0)s_state.detail_scroll=0;
            if(s_state.detail_scroll>s_state.detail_scroll_max)s_state.detail_scroll=s_state.detail_scroll_max;
        }
        xSemaphoreGive(s_state_lock);return;
    }
    if(s_state.nav.page==FEISHU_PAGE_XIAOZHI_SETTINGS &&
       (s_state.assistant_settings_editing || (s_state.nav.settings_index>=2 && event==FEISHU_NAV_OK))){
        bool save=false;
        if(event==FEISHU_NAV_OK || event==FEISHU_NAV_BACK){
            save=s_state.assistant_settings_editing;s_state.assistant_settings_editing=!save && event==FEISHU_NAV_OK;
        } else if(event==FEISHU_NAV_UP || event==FEISHU_NAV_DOWN){
            if(s_state.nav.settings_index==2){
                unsigned v=s_state.assistant_settings.volume;
                s_state.assistant_settings.volume=event==FEISHU_NAV_UP ? (v<=90 ? v+10:100):(v>=10 ? v-10:0);
            } else {
                const unsigned choices[]={1,3,5,10,15,20,30,60,0};unsigned i=3;
                for(unsigned n=0;n<9;++n)if(choices[n]==s_state.assistant_settings.timeout_minutes)i=n;
                i=(i+(event==FEISHU_NAV_UP ? 1:8))%9;s_state.assistant_settings.timeout_minutes=choices[i];
            }
        }
        xSemaphoreGive(s_state_lock);if(save)enqueue(WORK_ASSISTANT_SAVE);return;
    }
    if(s_state.nav.page==FEISHU_PAGE_ASSISTANT_SETTINGS){
        bool save=false;
        if(event==FEISHU_NAV_BACK){save=s_state.assistant_settings_editing;s_state.assistant_settings_editing=false;s_state.nav.page=FEISHU_PAGE_XIAOZHI_SETTINGS;s_state.nav.settings_index=1;}
        else if(event==FEISHU_NAV_OK){save=s_state.assistant_settings_editing;s_state.assistant_settings_editing=!s_state.assistant_settings_editing;}
        else if(event==FEISHU_NAV_UP || event==FEISHU_NAV_DOWN){
            if(!s_state.assistant_settings_editing)s_state.nav.settings_index=1-s_state.nav.settings_index%2;
            else if(s_state.nav.settings_index==0)s_state.assistant_settings.boot=!s_state.assistant_settings.boot;
            else {unsigned v=s_state.assistant_settings.silence_seconds;unsigned i=v ? v-1:0;
                i=(i+(event==FEISHU_NAV_UP ? 4:1))%5;s_state.assistant_settings.silence_seconds=i ? i+1:0;}
        }
        xSemaphoreGive(s_state_lock);if(save)enqueue(WORK_ASSISTANT_SAVE);return;
    }
    xSemaphoreGive(s_state_lock);
    if(power_navigation(event))return;
    if(network_navigation(event))return;
    if((event==FEISHU_NAV_UP || event==FEISHU_NAV_DOWN) && move_detail(event==FEISHU_NAV_UP ? -1 : 1,false))return;
    if((event==FEISHU_NAV_UP || event==FEISHU_NAV_DOWN) && change_image(event==FEISHU_NAV_UP ? -1:1,0,false,false))return;
    feishu_nav_action_t action;bool schedule_image=false;
    feishu_page_t previous_page;
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    if(s_state.nav.page==FEISHU_PAGE_ASSISTANT) {
        bool start=false,cancel_read=false;
        if(event==FEISHU_NAV_BACK) {
            if(s_state.assistant_active){s_state.stop_recording=s_state.cancel_recording=true;cancel_read=true;}
            else {s_state.assistant_requested=false;s_state.nav.page=FEISHU_PAGE_SETTINGS;s_state.nav.settings_index=FEISHU_SETTING_ASSISTANT;}
        } else if(event==FEISHU_NAV_OK) {
            if(s_state.assistant_active)s_state.stop_recording=true;
            else if(online() && !s_state.assistant_requested) {
                s_state.assistant_active=true;s_state.record_pending=true;s_state.status_error=false;
                s_state.stop_recording=s_state.cancel_recording=false;s_state.elapsed_ms=0;
                s_state.assistant_heard[0]=s_state.assistant_reply[0]=s_state.status[0]=0;start=true;
            } else if(!s_state.assistant_requested){s_state.status_error=true;feishu_utf8_copy(s_state.status,sizeof(s_state.status),"请先连接网络");}
        }
        xSemaphoreGive(s_state_lock);
        if(cancel_read)feishu_api_cancel_prefetch();
        if(start)enqueue(WORK_ASSISTANT);
        return;
    }
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
    if(previous_page==FEISHU_PAGE_XIAOZHI) {
        bool check=event==FEISHU_NAV_OK && !s_state.pairing_busy;
        if(event==FEISHU_NAV_BACK) {
            s_state.cancel_pairing=true;++s_state.pairing_request;s_state.pairing_busy=false;
            s_state.nav.page=s_state.pairing_return_page;s_state.status[0]=0;s_state.status_error=false;
        } else if(check) {
            s_state.cancel_pairing=false;s_state.pairing_busy=true;++s_state.pairing_request;
            s_state.status_error=false;
            feishu_utf8_copy(s_state.status,sizeof(s_state.status),"正在检查配对状态…");
        }
        xSemaphoreGive(s_state_lock);if(check)enqueue(WORK_CHECK_XIAOZHI);sync_audio_focus();return;
    }
    s_state.status_error=false;
    if(previous_page==FEISHU_PAGE_REVIEW && event==FEISHU_NAV_BACK) {
        if(s_state.compose_send_confirm){s_state.compose_send_confirm=false;
            feishu_utf8_copy(s_state.status,sizeof(s_state.status),"已取消发送，草稿保留");
            xSemaphoreGive(s_state_lock);return;}

        bool manager=s_state.nav.draft_manager;
        s_state.nav.page=manager ? FEISHU_PAGE_DRAFTS:FEISHU_PAGE_MESSAGES;s_state.pending_retry_warning=false;
        feishu_utf8_copy(s_state.status,sizeof(s_state.status),s_state.pending_slot==RAM_REPLY_SLOT ?
            "内存草稿已保留，断电丢失":"草稿已保留，进入回复可继续");
        xSemaphoreGive(s_state_lock);if(manager)enqueue(WORK_LIST_DRAFTS);sync_audio_focus();return;
    }
    if(previous_page==FEISHU_PAGE_REVIEW && !s_state.compose_send_confirm && (s_state.detail_pages>1 ||
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
        if(previous_page!=FEISHU_PAGE_POWER_SETTINGS && s_state.nav.page==FEISHU_PAGE_POWER_SETTINGS){
            s_power_editing=false;feishu_utf8_copy(s_state.status,sizeof(s_state.status),"任意按键唤醒，首键不执行");
        }
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
        feishu_utf8_copy(s_state.status,sizeof(s_state.status),"结果未确认，重试可能重复");
        xSemaphoreGive(s_state_lock);return;
    }
    if(previous_page==FEISHU_PAGE_DRAFT_ACTIONS && (event==FEISHU_NAV_UP || event==FEISHU_NAV_DOWN))s_state.pending_retry_warning=false;
    if(action==FEISHU_NAV_SEND_REPLY && !s_state.pending.length){
        s_state.sending=s_state.loading=false;s_state.nav.page=FEISHU_PAGE_REVIEW;
        feishu_utf8_copy(s_state.status,sizeof(s_state.status),"正文为空，请先编辑");
        xSemaphoreGive(s_state_lock);return;
    }
    if(action==FEISHU_NAV_SEND_REPLY && !s_state.compose_send_confirm){
        s_state.compose_send_confirm=true;s_state.nav.page=FEISHU_PAGE_REVIEW;
        feishu_utf8_copy(s_state.status,sizeof(s_state.status),"请核对目标和正文");
        xSemaphoreGive(s_state_lock);return;
    }
    if(action==FEISHU_NAV_SEND_REPLY || action==FEISHU_NAV_START_RECORDING || action==FEISHU_NAV_RETRY_RECORDING)
        s_state.compose_send_confirm=false;
    bool read_pending=previous_page!=FEISHU_PAGE_REPLY_READY && s_state.nav.page==FEISHU_PAGE_REPLY_READY;
    if(s_state.nav.draft_manager && s_state.nav.page==FEISHU_PAGE_MESSAGES &&
       (action==FEISHU_NAV_CANCEL_REPLY || previous_page==FEISHU_PAGE_STATUS))s_state.nav.page=FEISHU_PAGE_DRAFTS;
    if(action==FEISHU_NAV_CLEAR_DRAFTS)s_state.loading=true;
    if(action==FEISHU_NAV_LIST_DRAFTS || action==FEISHU_NAV_OPEN_DRAFT) {
        s_state.loading=true;feishu_utf8_copy(s_state.status,sizeof(s_state.status),"正在读取草稿...");
    }
    if(action==FEISHU_NAV_CHECK_XIAOZHI) {
        s_state.pairing_return_page=previous_page;s_state.pairing_busy=true;s_state.cancel_pairing=false;s_state.pairing_auto_blocked=false;s_state.pairing_retry_at=0;++s_state.pairing_request;
        feishu_utf8_copy(s_state.status,sizeof(s_state.status),"正在读取小智配置…");
    }
    if(read_pending) {
        s_state.nav.draft_manager=false;
        /* Ordinary recording must not wait for a disposable database. Existing
         * durable drafts remain reachable through the separate draft manager. */
        read_pending=false;s_state.pending_retry_warning=false;s_state.loading=false;s_state.status[0]=0;
        if(s_state.pending_slot==RAM_REPLY_SLOT){
            if(s_state.nav.chat_index<s_state.nav.chat_count && !strcmp(s_state.pending.chat,s_state.chats[s_state.nav.chat_index].chat_id))
                apply_pending_draft(RAM_REPLY_SLOT,&s_state.pending);
        } else s_state.pending_slot=-1;
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
        if(action==FEISHU_NAV_START_RECORDING) {
            s_state.nav.draft_manager=false;s_state.pending_retry_warning=false;
            if(s_state.pending_slot!=RAM_REPLY_SLOT){s_state.pending_slot=-1;memset(&s_state.pending,0,sizeof(s_state.pending));}
        }
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
            s_state.loading=true;feishu_utf8_copy(s_state.status,sizeof(s_state.status),"正在删除草稿...");
        }
        if(!(s_state.pending_slot==RAM_REPLY_SLOT && (previous_page==FEISHU_PAGE_RECORDING || previous_page==FEISHU_PAGE_TRANSCRIBING)))
            memset(s_state.reply,0,sizeof(s_state.reply));
    } else if (action == FEISHU_NAV_SEND_REPLY) {
        s_state.cancel_send=false;s_state.sending=true;s_state.nav.page=FEISHU_PAGE_REVIEW;
        if (s_state.reply_uuid[0] == '\0') make_reply_uuid(s_state.reply_uuid);
        s_state.loading = true;
        feishu_utf8_copy(s_state.status, sizeof(s_state.status),
                         s_state.nav.reply_to_message ?
                         "正在发送回复..." : "正在发送消息...");
    }
    if(action==FEISHU_NAV_REAUTHORIZE || action==FEISHU_NAV_UNBIND) {
        s_state.loading=true;feishu_utf8_copy(s_state.status,sizeof(s_state.status),"正在准备BOT授权…");
    }
    xSemaphoreGive(s_state_lock);
    if(action==FEISHU_NAV_LOAD_MESSAGES || read_pending || read_detail || action==FEISHU_NAV_START_RECORDING || action==FEISHU_NAV_RETRY_RECORDING ||
       action==FEISHU_NAV_LOAD_IMAGE || leaving_image || action==FEISHU_NAV_REAUTHORIZE || action==FEISHU_NAV_UNBIND)
        feishu_api_cancel_prefetch();
    if(read_detail)enqueue(WORK_LOAD_TEXT_PAGE);
    if(read_pending)enqueue(WORK_LOAD_DRAFT);
    if(action==FEISHU_NAV_LIST_DRAFTS || (previous_page==FEISHU_PAGE_STATUS && event==FEISHU_NAV_BACK))enqueue(WORK_LIST_DRAFTS);
    if(action==FEISHU_NAV_OPEN_DRAFT)enqueue(WORK_OPEN_DRAFT);
    if(action==FEISHU_NAV_CLEAR_DRAFTS)enqueue(WORK_CLEAR_DRAFTS);
    if(action==FEISHU_NAV_CANCEL_REPLY && (previous_page==FEISHU_PAGE_REVIEW || previous_page==FEISHU_PAGE_DRAFT_ACTIONS))enqueue(WORK_DISCARD_DRAFT);

    if (action == FEISHU_NAV_LOAD_MESSAGES){ESP_LOGI(TAG,"MESSAGE_OPEN queued=1");enqueue(WORK_LOAD_MESSAGES);}
    if (action == FEISHU_NAV_LOAD_IMAGE && schedule_image) enqueue(WORK_LOAD_IMAGE);
    if(action==FEISHU_NAV_IMAGE_ZOOM_IN || action==FEISHU_NAV_IMAGE_ZOOM_OUT || action==FEISHU_NAV_IMAGE_AXIS)
        change_image(0,action==FEISHU_NAV_IMAGE_ZOOM_IN ? 1:action==FEISHU_NAV_IMAGE_ZOOM_OUT ? -1:0,action==FEISHU_NAV_IMAGE_AXIS,false);
    if (action == FEISHU_NAV_START_RECORDING || action == FEISHU_NAV_RETRY_RECORDING) enqueue(WORK_RECORD);
    if (action == FEISHU_NAV_SEND_REPLY) enqueue(WORK_SEND);
    if(action==FEISHU_NAV_ENTER_ASSISTANT)assistant_toggle();
    if(action==FEISHU_NAV_LIST_NETWORK)network_enter();
    if (action == FEISHU_NAV_RESET_WIFI) enqueue(WORK_RESET_WIFI);
    if (action == FEISHU_NAV_UNBIND) enqueue(WORK_UNBIND);
    if (action == FEISHU_NAV_REAUTHORIZE) enqueue(WORK_REAUTHORIZE);
    if (action == FEISHU_NAV_CHECK_XIAOZHI) enqueue(WORK_CHECK_XIAOZHI);
    sync_audio_focus();
}

#ifdef FEISHU_UI_AUTOTEST
#include "../tools/ui_probe/pairing_ui.inc"
#endif
void demo_feishu_enter(void)
{
    ++s_screen_generation;
    free(s_sent_echo);s_sent_echo=NULL;
    memset(&s_state, 0, sizeof(s_state));
    s_state.pending_slot=-1;
    s_state.startup_pending=true;s_startup_cover=s_startup_logo=NULL;
    s_state.assistant_settings=(feishu_assistant_settings_t){false,2,60,10};s_notice_listening=false;
    s_state.sounds=(feishu_sound_settings_t){true,true,true};
    memset(&s_session, 0, sizeof(s_session));
    s_authenticated=false;
    atomic_store(&s_audio_focus_queued,false);
    s_focus_audio_reserved=false;
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
    lv_obj_add_event_cb(s_scr,network_keyboard_draw,LV_EVENT_DRAW_POST,NULL);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(FEISHU_BG), 0);
    lv_obj_set_style_bg_opa(s_scr,LV_OPA_COVER,0);
    lv_obj_set_style_border_width(s_scr, 0, 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);
    feishu_ui_header(s_scr,&s_title,&s_page_info,&s_assistant_badge);
    s_viewport=lv_obj_create(s_scr);
    lv_obj_set_size(s_viewport,216,FEISHU_CONTENT_HEIGHT);lv_obj_set_pos(s_viewport,12,FEISHU_CONTENT_TOP);
    lv_obj_remove_flag(s_viewport,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_viewport,LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_style_bg_opa(s_viewport,LV_OPA_TRANSP,0);
    lv_obj_set_style_border_width(s_viewport,0,0);lv_obj_set_style_pad_all(s_viewport,0,0);
    lv_obj_set_style_radius(s_viewport,0,0);lv_obj_set_style_shadow_width(s_viewport,0,0);
    s_content = lv_label_create(s_viewport);
    lv_obj_set_size(s_content,216,LV_SIZE_CONTENT);
    lv_obj_set_pos(s_content,0,0);
    lv_obj_set_style_text_font(s_content, &lv_font_ai_passport_14, 0);
    lv_obj_set_style_text_color(s_content, lv_color_hex(FEISHU_TEXT), 0);
    lv_label_set_long_mode(s_content, LV_LABEL_LONG_WRAP);
    s_image = lv_image_create(s_scr);
    lv_obj_set_pos(s_image, 12, 42);
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
                                    lv_color_hex(FEISHU_TEXT), 0);
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
        lv_obj_set_style_bg_color(s_row_mark[i], lv_color_hex(FEISHU_ERROR), 0);
        lv_obj_set_style_bg_opa(s_row_mark[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_font(s_row_mark[i], &lv_font_ai_passport_14, 0);
        lv_obj_set_style_text_align(s_row_mark[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_add_flag(s_rows[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_t *actions[3];feishu_ui_footer(s_scr,&s_notice,actions);
    // Keep the centered phrase stationary; animate only its independent suffix.
    s_notice_dots=lv_label_create(lv_obj_get_parent(s_notice));
    lv_label_set_text(s_notice_dots,"");
    lv_obj_set_style_text_font(s_notice_dots,&lv_font_ai_passport_14,0);
    lv_obj_set_style_text_align(s_notice_dots,LV_TEXT_ALIGN_LEFT,0);
    lv_point_t listening_size;
    lv_text_get_size(&listening_size,"小智聆听中",&lv_font_ai_passport_14,0,0,212,LV_TEXT_FLAG_NONE);
    lv_obj_set_pos(s_notice_dots,13+(212+listening_size.x)/2,4);
    lv_obj_set_size(s_notice_dots,30,18);
    lv_obj_add_flag(s_notice_dots,LV_OBJ_FLAG_HIDDEN);
    s_hint=actions[0];s_hint_groups[0]=actions[1];s_hint_groups[1]=actions[2];
    (void)render_startup();
    lv_screen_load(s_scr);

    if (s_state_lock == NULL || s_worker_done == NULL || s_work_queue == NULL) {
        s_state.loading = false;s_state.startup_pending=false;
        feishu_utf8_copy(s_state.status, sizeof(s_state.status), "Not enough memory");
        set_label_text(s_content, "\n\n内存不足，请重启设备");
        set_label_text(s_hint, "长按确定：返回");
        return;
    }

    s_timer = lv_timer_create(render, 100, NULL);
#ifdef FEISHU_UI_AUTOTEST
    s_ui_smoke_timer=lv_timer_create(pairing_ui_smoke,100,NULL);
#endif
    // Message parsing and HTTPS each have non-trivial fixed stack peaks. Image
    // resource keys also enlarge the bounded message arrays kept on this task.
    // Message parsing needs more than the original 10 KiB stack, while a
    // 16 KiB stack leaves too little contiguous heap for Feishu's unusually
    // long OAuth Authorization header plus TLS buffers on an ESP32-C3.
    feishu_asr_set_resource_hooks(reserve_audio_resources,release_audio_resources);
    if (xTaskCreate(worker_task, "feishu", 12288, NULL, 4, &s_worker) != pdPASS) {
        s_state.loading = false;s_state.startup_pending=false;
        feishu_utf8_copy(s_state.status, sizeof(s_state.status), "内存不足，请重启设备");
        return;
    }
    enqueue(WORK_INIT);
}

void demo_feishu_exit(void)
{
#ifdef FEISHU_UI_AUTOTEST
    if(s_ui_smoke_timer){lv_timer_delete(s_ui_smoke_timer);s_ui_smoke_timer=NULL;}
#endif
    feishu_api_cancel_prefetch();
    if(s_state_lock) {
        xSemaphoreTake(s_state_lock,portMAX_DELAY);
        s_state.closing=true;
        s_state.stop_recording=true;
        s_state.cancel_recording=true;
        s_state.cancel_pairing=true;++s_state.pairing_request;
        xSemaphoreGive(s_state_lock);
    }
    if (s_worker != NULL) {
        enqueue(WORK_EXIT);
        /* Network requests have bounded timeouts. Never free a live worker's
         * queues, mutexes or SQLite connection underneath an in-flight call. */
        xSemaphoreTake(s_worker_done,portMAX_DELAY);
    }
    feishu_network_stop();
    if(s_network_ui){memset(s_network_ui,0,sizeof(*s_network_ui));free(s_network_ui);s_network_ui=NULL;}
    if (s_timer != NULL) { lv_timer_delete(s_timer); s_timer = NULL; }
    if (s_scr != NULL) { lv_obj_delete(s_scr); s_scr = NULL; }
    s_pair_qr=s_pair_caption=NULL;
    feishu_cache_unmount();
    if (s_work_queue != NULL) { vQueueDelete(s_work_queue); s_work_queue = NULL; }
    if (s_cache_queue != NULL) { vQueueDelete(s_cache_queue); s_cache_queue = NULL; }
    feishu_asr_set_resource_hooks(NULL,NULL);
    if (s_cache_done != NULL) { vSemaphoreDelete(s_cache_done); s_cache_done = NULL; }
    if (s_worker_done != NULL) { vSemaphoreDelete(s_worker_done); s_worker_done = NULL; }
    if (s_state_lock != NULL) { vSemaphoreDelete(s_state_lock); s_state_lock = NULL; }
    memset(&s_session, 0, sizeof(s_session));
    free(s_sent_echo);s_sent_echo=NULL;
}

void demo_feishu_power_tick(void)
{
 if(s_worker && s_focus_audio_reserved && feishu_asr_prepare_expired() && !bsp_audio_active() && !atomic_exchange(&s_audio_focus_queued,true)){
  work_command_t c=WORK_AUDIO_FOCUS;if(xQueueSend(s_work_queue,&c,0)!=pdTRUE)atomic_store(&s_audio_focus_queued,false);
 }
 if(s_worker && feishu_power_network_pending() && !atomic_exchange(&s_power_queued,true)){
  work_command_t c=WORK_POWER_APPLY;if(xQueueSend(s_work_queue,&c,0)!=pdTRUE)atomic_store(&s_power_queued,false);
 }
}
void demo_feishu_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if(s_state_lock && (ev==BSP_BTN_CLICK || ev==BSP_BTN_DOUBLE))feedback_request(FEISHU_SOUND_KEY);
    if(btn==BSP_BTN_OK && ev==BSP_BTN_DOUBLE){assistant_toggle();return;}
    if(btn==BSP_BTN_UP && ev==BSP_BTN_DOUBLE) {handle_navigation(FEISHU_NAV_BROWSE);return;}
    if(ev==BSP_BTN_LONG && (btn==BSP_BTN_UP || btn==BSP_BTN_DOWN) && network_long_key(btn))return;
    if(btn==BSP_BTN_DOWN && ev==BSP_BTN_DOUBLE) {handle_navigation(FEISHU_NAV_COMPOSE);return;}
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
    if(cancel_read) {feishu_api_cancel_prefetch();sync_audio_focus();return true;}
    if (consume && ready && !loading) {
        handle_navigation(FEISHU_NAV_BACK);
    }
    return consume;
}

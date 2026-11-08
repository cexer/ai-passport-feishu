/* Included after ordinary worker helpers. All network/storage calls execute on
 * the existing worker; UI state is accessed only under s_state_lock. */
typedef struct {
    uint32_t epoch;
    feishu_page_t target;
    size_t chat;
    unsigned tools_mask,send_notice;
    uint32_t turn,confirmation_turn;
    bool notice_listened;
    bool composition,handoff,read_deferred;
    work_command_t deferred_read;
    TickType_t refresh_at,battery_at;
#ifdef FEISHU_GLOBAL_AUTOTEST
    bool global_injected,global_job,global_mentions,global_key_sound;
    bool background_refresh_injected,background_refresh_deferred;
#endif
    feishu_compose_snapshot_t *undo;
#ifdef FEISHU_DRAFT_RAM_AUTOTEST
    bool diagnostic_draft;
#endif
#ifdef FEISHU_LVGL_RECOVERY_AUTOTEST
    bool reply_injected,back_tested;unsigned ui_before;
#endif
#ifdef FEISHU_COMPOSE_FLOW_AUTOTEST
    bool flow_started,flow_done,flow_pass;int64_t flow_deadline;
#endif
    bool changed,draft,target_unresolved,tool_failed;
} assistant_ui_t;
#ifdef FEISHU_ASSISTANT_AUTOTEST
static unsigned s_assistant_test_case,s_assistant_tests_passed;
static bool s_assistant_suite_pending;
/* Cleanup runs after the voice turn. Prevent the compiler from reserving its
 * two outbox records on the live ASR/HTTP/decoder call stack. */
static bool __attribute__((noinline)) assistant_test_cleanup(bool draft)
{
    if(!draft)return true;
#ifdef FEISHU_DRAFT_RAM_AUTOTEST
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    bool memory=s_state.pending_slot==RAM_REPLY_SLOT && s_state.reply[0] &&
        s_state.pending.length==strlen(s_state.reply) && s_state.pending.chat[0];
    s_state.nav.page=FEISHU_PAGE_DRAFTS;s_state.nav.draft_index=0;
    xSemaphoreGive(s_state_lock);
    list_pending_drafts();
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    bool listed=s_state.nav.draft_count==1 && s_state.draft_slots[0]==FEISHU_OUTBOX_SLOTS;
    xSemaphoreGive(s_state_lock);
    open_pending_draft();
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    bool restored=s_state.nav.page==FEISHU_PAGE_REVIEW && s_state.detail_loaded &&
        !strcmp(s_state.detail_text,s_state.reply) && s_state.pending_slot==RAM_REPLY_SLOT;
    s_state.pending_slot=-1;memset(&s_state.pending,0,sizeof(s_state.pending));s_state.reply[0]=0;
    xSemaphoreGive(s_state_lock);
    ESP_LOGI(TAG,"ASSISTANT_RAM_TEST prepared=%u listed=%u restored=%u sends=0",(unsigned)memory,(unsigned)listed,(unsigned)restored);
    return memory && listed && restored;
#else
    feishu_outbox_t pending,owned;int slot;
    xSemaphoreTake(s_state_lock,portMAX_DELAY);pending=s_state.pending;slot=s_state.pending_slot;xSemaphoreGive(s_state_lock);
    if(slot<0 || feishu_cache_pending_read((unsigned)slot,&owned)!=ESP_OK ||
       owned.object_id!=pending.object_id || memcmp(owned.uuid,pending.uuid,sizeof(owned.uuid)))return false;
    owned.state=FEISHU_OUTBOX_EMPTY;
    if(feishu_cache_pending_write((unsigned)slot,&owned,NULL)!=ESP_OK)return false;
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    memset(&s_state.pending,0,sizeof(s_state.pending));s_state.pending_slot=-1;s_state.reply[0]=0;
    xSemaphoreGive(s_state_lock);return true;
#endif
}
#endif
static void assistant_event(void *context,feishu_assistant_event_t event,const char *text)
{
    assistant_ui_t *a=context;
    if((event==FEISHU_ASSISTANT_HEARD || event==FEISHU_ASSISTANT_REPLY) && text && *text)feishu_power_activity();
#ifdef FEISHU_ASSISTANT_AUTOTEST
    if(event==FEISHU_ASSISTANT_HEARD && text && *text)ESP_LOGI(TAG,"ASSISTANT_HEARD_TEST text=%s",text);
#endif
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    if(a->epoch==s_owner_epoch && s_state.assistant_active && !s_state.cancel_recording) {
#ifdef FEISHU_ASSISTANT_TIMEOUT_AUTOTEST
        if(event==FEISHU_ASSISTANT_LISTENING){
            s_state.assistant_settings.timeout_minutes=10;
            s_state.assistant_activity_ms=(uint32_t)(esp_timer_get_time()/1000)-600000u+800u;
            ESP_LOGI(TAG,"ASSISTANT_TIMEOUT_ARM remaining_ms=800 persist=0");
        }
#endif
        if((event==FEISHU_ASSISTANT_HEARD || event==FEISHU_ASSISTANT_REPLY) && text && *text)
            s_state.assistant_activity_ms=(uint32_t)(esp_timer_get_time()/1000);
        if(event==FEISHU_ASSISTANT_GREETED){s_state.assistant_greeted=true;xSemaphoreGive(s_state_lock);return;}
        if(event==FEISHU_ASSISTANT_PAUSED){s_state.assistant_phase=FEISHU_ASSISTANT_PAUSED_WORK;
            feishu_utf8_copy(s_state.status,sizeof(s_state.status),"小智暂停聆听");}
        if(event==FEISHU_ASSISTANT_RECONNECTING){s_state.assistant_phase=FEISHU_ASSISTANT_CONNECTING;
            s_state.stop_recording=s_state.record_pending=true;s_state.assistant_speech_detected=false;
            feishu_utf8_copy(s_state.status,sizeof(s_state.status),"连接中断，正在恢复小智");}
        if(event==FEISHU_ASSISTANT_PREPARING){s_state.assistant_phase=FEISHU_ASSISTANT_PREPARING_MIC;feishu_utf8_copy(s_state.status,sizeof(s_state.status),"麦克风准备中");}
        if(event==FEISHU_ASSISTANT_LISTENING)s_state.assistant_phase=FEISHU_ASSISTANT_READY;
        if(event==FEISHU_ASSISTANT_THINKING || event==FEISHU_ASSISTANT_HEARD || event==FEISHU_ASSISTANT_TOOL)s_state.assistant_phase=FEISHU_ASSISTANT_PROCESSING;
        if(event==FEISHU_ASSISTANT_REPLY)s_state.assistant_phase=FEISHU_ASSISTANT_SPEAKING;
        if(event==FEISHU_ASSISTANT_LISTENING){s_state.stop_recording=false;s_state.record_pending=false;s_state.assistant_speech_detected=false;s_state.assistant_heard[0]=s_state.assistant_reply[0]=0;}
        if(event==FEISHU_ASSISTANT_THINKING){s_state.stop_recording=true;s_state.record_pending=false;}
        if(event==FEISHU_ASSISTANT_HEARD){++a->turn;a->target_unresolved=false;a->tool_failed=false;a->draft=false;feishu_utf8_copy(s_state.assistant_heard,sizeof(s_state.assistant_heard),text);}
        if(event==FEISHU_ASSISTANT_REPLY)feishu_utf8_copy(s_state.assistant_reply,sizeof(s_state.assistant_reply),text);
        if(event==FEISHU_ASSISTANT_THINKING)feishu_utf8_copy(s_state.status,sizeof(s_state.status),text);
        else if(event==FEISHU_ASSISTANT_TOOL)feishu_utf8_copy(s_state.status,sizeof(s_state.status),"小智正在干活");
        else if(event==FEISHU_ASSISTANT_LISTENING)feishu_utf8_copy(s_state.status,sizeof(s_state.status),"小智聆听中");
        else if(event==FEISHU_ASSISTANT_HEARD){s_state.stop_recording=true;feishu_utf8_copy(s_state.status,sizeof(s_state.status),"小智正在干活");}
        else if(event==FEISHU_ASSISTANT_REPLY)feishu_utf8_copy(s_state.status,sizeof(s_state.status),"小智正在回复");
        if(event==FEISHU_ASSISTANT_LISTENING)a->notice_listened=true;
        if(event==FEISHU_ASSISTANT_HEARD && a->notice_listened && text && *text)a->send_notice=0;
        if(a->send_notice){
            feishu_utf8_copy(s_state.status,sizeof(s_state.status),a->send_notice==1 ? "消息已发送":
                a->send_notice==2 ? "发送未完成，草稿已保留":"发送结果未确认，请先查看飞书");
            s_state.status_error=a->send_notice!=1;
        } else s_state.status_error=false;
        if(s_state.compose_send_confirm)feishu_utf8_copy(s_state.status,sizeof(s_state.status),"核对后说确认发送，或按确定");
        if(a->tool_failed){s_state.status_error=true;feishu_utf8_copy(s_state.status,sizeof(s_state.status),"操作未完成，可重试");}
    }
    xSemaphoreGive(s_state_lock);
}
#include "feishu_compose_ui.inc"
#include "feishu_assistant_settings.inc"
static bool assistant_tool(void *context,const char *name,const cJSON *args,char *text,size_t capacity)
{
    assistant_ui_t *a=context;
    bool ok=false;size_t chat=FEISHU_MAX_CHATS;char *formatted=NULL;
    char chat_id[FEISHU_CHAT_ID_MAX]={0},chat_name[FEISHU_CHAT_NAME_MAX]={0};
    const cJSON *id=cJSON_GetObjectItemCaseSensitive(args,"chat_id");
    const cJSON *index=cJSON_GetObjectItemCaseSensitive(args,"chat_index");
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    bool valid=a->epoch==s_owner_epoch && !s_state.cancel_recording && !s_state.closing;
    if(valid) {
        if(cJSON_IsNumber(index) && index->valueint>=0 && (size_t)index->valueint<s_state.nav.chat_count)chat=(size_t)index->valueint;
        else if(cJSON_IsString(id))for(size_t i=0;i<s_state.nav.chat_count;++i)
            if(!strcmp(id->valuestring,s_state.chats[i].chat_id)){chat=i;break;}
        /* An omitted recipient means CURRENT conversation only. A cursor in
         * the conversation list is not consent to choose that recipient. */
        if(!id && !index && (!strcmp(name,"self.feishu.open_composer") || !strcmp(name,"self.feishu.find_member")) &&
           (s_state.nav.page==FEISHU_PAGE_MESSAGES || s_state.nav.page==FEISHU_PAGE_MESSAGE_DETAIL ||
            s_state.nav.page==FEISHU_PAGE_REPLY_READY || s_state.nav.page==FEISHU_PAGE_REVIEW))
            chat=s_state.nav.chat_index;
        if(chat<s_state.nav.chat_count) {
            feishu_utf8_copy(chat_id,sizeof(chat_id),s_state.chats[chat].chat_id);
            feishu_utf8_copy(chat_name,sizeof(chat_name),s_state.chats[chat].name);
        }
    }
    xSemaphoreGive(s_state_lock);
    if(!valid){snprintf(text,capacity,"Cancelled or account changed; no action performed.");return false;}
    if(!strcmp(name,"self.feishu.settings")){
        bool success=assistant_settings_tool(a,args,text,capacity);a->tool_failed=!success;return success;
    }
    if(!strcmp(name,"self.feishu.get_context") || !strcmp(name,"self.feishu.read_draft") ||
       !strcmp(name,"self.feishu.edit_draft") || !strcmp(name,"self.feishu.undo_draft") || !strcmp(name,"self.feishu.request_send"))
    {
        bool success=assistant_compose_tool(a,name,args,text,capacity);a->tool_failed=!success;
        if(success && !strcmp(name,"self.feishu.edit_draft"))a->tools_mask|=32;
        if(success && !strcmp(name,"self.feishu.undo_draft"))a->tools_mask|=64;
        feedback_request(success ? FEISHU_SOUND_SUCCESS:FEISHU_SOUND_FAILURE);return success;
    }
    if(chat<FEISHU_MAX_CHATS)ESP_LOGI(TAG,"ASSISTANT_TARGET name=%s index=%u",name,(unsigned)chat);
    cJSON *result=cJSON_CreateObject();if(!result){snprintf(text,capacity,"Not enough memory.");return false;}
    if(!strcmp(name,"self.feishu.list_chats")) {
        const cJSON *query=cJSON_GetObjectItemCaseSensitive(args,"query"),*offset=cJSON_GetObjectItemCaseSensitive(args,"offset");
        const char *q=cJSON_IsString(query) ? query->valuestring:"";
        size_t skip=cJSON_IsNumber(offset) ? (size_t)offset->valueint:0,matched=0,added=0;bool full=false;
        cJSON *items=cJSON_AddArrayToObject(result,"chats");
        xSemaphoreTake(s_state_lock,portMAX_DELAY);
        for(size_t i=0;items && i<s_state.nav.chat_count;++i) {
            if(*q && !strstr(s_state.chats[i].name,q))continue;
            if(matched++<skip || added>=4 || full)continue;
            cJSON *item=cJSON_CreateObject();
            if(!item || !cJSON_AddItemToArray(items,item)){cJSON_Delete(item);break;}
            cJSON_AddStringToObject(item,"chat_id",s_state.chats[i].chat_id);
            cJSON_AddNumberToObject(item,"chat_index",(double)i);
            cJSON_AddStringToObject(item,"name",s_state.chats[i].name);
            /* Keep room for pagination metadata. Typical names fit four rows;
             * escaping/long names reduce the page instead of overflowing it. */
            if(capacity<=96 || !cJSON_PrintPreallocated(result,text,(int)(capacity-96),false)) {
                cJSON_Delete(cJSON_DetachItemViaPointer(items,item));full=true;continue;
            }
            ++added;
        }
        xSemaphoreGive(s_state_lock);
        ESP_LOGI(TAG,"ASSISTANT_CHAT_SNAPSHOT matched=%u returned=%u offset=%u query_bytes=%u",
            (unsigned)matched,(unsigned)added,(unsigned)skip,(unsigned)strlen(q));
        if(*q && matched!=1)a->target_unresolved=true; /* Latch until the user speaks again. */
        cJSON_AddBoolToObject(result,"cached",true);cJSON_AddNumberToObject(result,"next_offset",skip+added);
        cJSON_AddBoolToObject(result,"more",matched>skip+added);ok=items!=NULL;
    } else if(!strcmp(name,"self.feishu.navigate")) {
        const char *page=cJSON_GetObjectItemCaseSensitive(args,"page")->valuestring;
        if(!strcmp(page,"network")){network_enter();a->changed=a->read_deferred=false;
            xSemaphoreTake(s_state_lock,portMAX_DELAY);bool opened=s_state.nav.page==FEISHU_PAGE_NETWORK;xSemaphoreGive(s_state_lock);
            cJSON_AddStringToObject(result,"result",opened ? "Wi-Fi manager opened.":"Could not open Wi-Fi manager.");ok=opened;goto formatted;
        }
        a->target=!strcmp(page,"settings") ? FEISHU_PAGE_SETTINGS:!strcmp(page,"drafts") ? FEISHU_PAGE_DRAFTS:
            !strcmp(page,"xiaozhi_settings") ? FEISHU_PAGE_XIAOZHI_SETTINGS:!strcmp(page,"assistant_settings") ? FEISHU_PAGE_ASSISTANT_SETTINGS:
            !strcmp(page,"xiaozhi_account") ? FEISHU_PAGE_XIAOZHI:!strcmp(page,"account") ? FEISHU_PAGE_ACCOUNT:
            !strcmp(page,"sounds") ? FEISHU_PAGE_SOUND_SETTINGS:FEISHU_PAGE_CHATS;
        xSemaphoreTake(s_state_lock,portMAX_DELAY);a->chat=s_state.nav.chat_index;
        s_state.nav.page=a->target;s_state.nav.settings_index=0;s_state.assistant_settings_editing=false;s_state.loading=a->target==FEISHU_PAGE_DRAFTS;xSemaphoreGive(s_state_lock);
        a->changed=false;a->read_deferred=false;
        if(a->target==FEISHU_PAGE_DRAFTS)process_work_command(WORK_LIST_DRAFTS);
        cJSON_AddStringToObject(result,"result","Page opened. Give only a brief completion confirmation.");ok=true;
    } else if(chat==FEISHU_MAX_CHATS) {
        snprintf(text,capacity,"No known conversation selected. Ask which recipient; do not claim the editor opened. Use a valid chat_index or chat_id from list_chats.");goto done;
    } else if(!strcmp(name,"self.feishu.find_member")) {
        const char *query=cJSON_GetObjectItemCaseSensitive(args,"name")->valuestring;
        if(*query=='@')++query;
        feishu_mention_t *member=calloc(1,sizeof(*member));
        esp_err_t found=member ? feishu_api_resolve_mention(&s_session,chat_id,query,member,&s_state.cancel_recording):ESP_ERR_NO_MEM;
        if(found==ESP_OK){
            cJSON_AddStringToObject(result,"chat_id",chat_id);cJSON_AddStringToObject(result,"member_id",member->id);
            cJSON_AddStringToObject(result,"name",member->name);cJSON_AddBoolToObject(result,"verified",true);
            cJSON_AddStringToObject(result,"instruction","Use this display name in mentions when preparing/editing. Do not insert plain @text.");ok=true;
        } else {
            cJSON_AddBoolToObject(result,"verified",false);cJSON_AddNumberToObject(result,"error",found);
            cJSON_AddStringToObject(result,"result","No complete unique member result. May be missing, ambiguous, incomplete, cancelled or unauthorized; do not invent an ID or claim a mention is ready.");
        }
        ESP_LOGI(TAG,"ASSISTANT_MEMBER_RESULT result=%d verified=%u",found,(unsigned)ok);
        free(member);
    } else if(!strcmp(name,"self.feishu.open_composer")) {
        if(a->target_unresolved){snprintf(text,capacity,"Recipient unresolved; ask for the exact conversation. Editor unchanged.");goto done;}
        const cJSON *reply_id=cJSON_GetObjectItemCaseSensitive(args,"reply_to_message_id");
        const char *reply_to=cJSON_IsString(reply_id) ? reply_id->valuestring:"";
        char uuid[41];make_reply_uuid(uuid);
        xSemaphoreTake(s_state_lock,portMAX_DELAY);
        bool target_known=!*reply_to;
        if(!target_known && !strcmp(s_state.loaded_chat,chat_id))
            for(size_t i=0;i<s_state.nav.message_count;++i)
                if(!strcmp(s_state.messages[i].message_id,reply_to)){target_known=true;break;}
        if(!target_known){xSemaphoreGive(s_state_lock);snprintf(text,capacity,"Reply target not found in this conversation. Open its message list and select the exact message. No ordinary-send fallback.");goto done;}
        bool preserve=s_state.pending.state==FEISHU_OUTBOX_READY && !s_state.pending.offset &&
            !strcmp(s_state.pending.chat,chat_id) && !strcmp(s_state.pending.message,reply_to) && s_state.pending.length<FEISHU_COMPOSE_CAP &&
            strlen(s_state.reply)==s_state.pending.length;
        if(!preserve){
            memset(&s_state.pending,0,sizeof(s_state.pending));s_state.pending.version=1;s_state.pending.state=FEISHU_OUTBOX_READY;
            feishu_utf8_copy(s_state.pending.chat,sizeof(s_state.pending.chat),chat_id);
            feishu_utf8_copy(s_state.pending.message,sizeof(s_state.pending.message),reply_to);
            feishu_utf8_copy(s_state.pending.uuid,sizeof(s_state.pending.uuid),uuid);
            feishu_utf8_copy(s_state.reply_uuid,sizeof(s_state.reply_uuid),uuid);
            s_state.pending_slot=RAM_REPLY_SLOT;s_state.reply[0]=0;
        }
        s_state.nav.page=FEISHU_PAGE_REVIEW;s_state.nav.chat_index=chat;s_state.nav.reply_to_message=*reply_to!=0;
        s_state.reply_from_assistant=s_state.assistant_composing=true;s_state.compose_send_confirm=false;
        s_state.loading=false;s_state.detail_loaded=true;s_state.detail_pages=1;s_state.detail_page=0;
        s_state.detail_scroll=s_state.detail_scroll_max=0;
        feishu_mentions_display(s_state.reply,s_state.detail_text,sizeof(s_state.detail_text));
        xSemaphoreGive(s_state_lock);
        a->composition=a->draft=true;a->changed=false;a->read_deferred=false;a->chat=chat;a->target=FEISHU_PAGE_REVIEW;
        ESP_LOGI(TAG,"ASSISTANT_EDITOR_OPEN ready=1 empty=%u current=%u",(unsigned)!s_state.reply[0],(unsigned)(!id && !index));
        free(a->undo);a->undo=NULL;
        cJSON_AddStringToObject(result,"reply_to_message_id",reply_to);
        cJSON_AddStringToObject(result,"result",preserve ? "Collaborative editor opened with current draft. Read/edit it; request_send opens user confirmation; a later explicit spoken confirmation or physical OK can send.":"Empty collaborative editor opened. Discuss what to write, then read_draft/edit_draft. Do not invent text. request_send requires nonempty content and user confirmation.");ok=true;
    } else if(!strcmp(name,"self.feishu.open_chat")) {
        a->target=!strcmp(name,"self.feishu.open_chat") ? FEISHU_PAGE_MESSAGES:FEISHU_PAGE_REPLY_READY;
        a->chat=chat;a->changed=false;a->read_deferred=false;
        xSemaphoreTake(s_state_lock,portMAX_DELAY);
        s_state.nav.page=a->target;s_state.nav.chat_index=chat;s_state.nav.reply_to_message=false;
        s_state.loading=true;++s_state.message_page_request;xSemaphoreGive(s_state_lock);
        /* Run inside the tool before returning its result. The capture and
         * Opus decoder have already been released by the voice owner. */
        TickType_t started=xTaskGetTickCount();
        esp_err_t loaded=load_messages_for_chat(chat,true,true);
        xSemaphoreTake(s_state_lock,portMAX_DELAY);
        bool current=s_state.nav.page==FEISHU_PAGE_MESSAGES && s_state.nav.chat_index==chat;
        ok=loaded==ESP_OK && current && !strcmp(s_state.loaded_chat,chat_id);
        if(current){s_state.loading=false;s_state.status_error=!ok;
            feishu_utf8_copy(s_state.status,sizeof(s_state.status),ok ? "":"消息加载未完成，可重试");}
        xSemaphoreGive(s_state_lock);
        ESP_LOGI(TAG,"ASSISTANT_OPEN_CHAT ready=%u result=%d elapsed_ms=%u",(unsigned)ok,loaded,
            (unsigned)((xTaskGetTickCount()-started)*portTICK_PERIOD_MS));
        cJSON_AddStringToObject(result,"result",ok ? "Conversation message list is now displayed. Confirm completion briefly.":
            "Message list did not finish loading or the user left it. Do not claim completion. Report failure briefly.");
    } else if(!strcmp(name,"self.feishu.get_messages")) {
        feishu_message_t *message=calloc(1,sizeof(*message));bool present=false;
        esp_err_t err=message ? feishu_api_message_preview(&s_session,chat_id,message,&present):ESP_ERR_NO_MEM;
        if(err!=ESP_OK){free(message);snprintf(text,capacity,"Message query failed; device remains usable. Retry later.");goto done;}
        cJSON_AddStringToObject(result,"chat",chat_name);
        cJSON_AddBoolToObject(result,"has_message",present);
        cJSON_AddBoolToObject(result,"preview_only",true);
        if(present) {
            cJSON_AddStringToObject(result,"message_id",message->message_id);
            cJSON_AddStringToObject(result,"sender",message->mine ? "me":message->sender_name);
            cJSON_AddStringToObject(result,"text",message->text);
            cJSON_AddBoolToObject(result,"truncated",message->text_truncated);
        }
        free(message);ok=true;
    } else if(!strcmp(name,"self.feishu.prepare_message")) {
        if(a->draft || a->composition){snprintf(text,capacity,"A draft is being edited. Use read_draft/edit_draft to change its body; do not replace its recipient with prepare_message.");goto done;}
        const char *content=cJSON_GetObjectItemCaseSensitive(args,"text")->valuestring;
        formatted=calloc(1,FEISHU_REPLY_MAX);
        if(!formatted || assistant_format_message(chat_id,args,content,NULL,formatted,FEISHU_REPLY_MAX)!=ESP_OK){
            snprintf(text,capacity,"Cannot resolve a unique real mention, missing permission, or message too long. Use mentions names separately; do not send plain @Name. No draft replaced.");goto done;
        }
        content=formatted;

        if(a->target_unresolved){snprintf(text,capacity,"Requested conversation was not uniquely found. Ask the user for its exact name; do not substitute another chat.");goto done;}
        int slot=-1;esp_err_t err=feishu_cache_draft_slot(&slot);
#if defined(FEISHU_DRAFT_RAM_AUTOTEST) || defined(FEISHU_GLOBAL_AUTOTEST)
        err=ESP_FAIL; /* Inject storage outage without mutating the user's draft. */
#endif
        feishu_data_writer_t writer={0};feishu_outbox_t pending={0};char uuid[41],key[64];
        make_reply_uuid(uuid);snprintf(key,sizeof(key),"assistant_%s",uuid);
        if(err==ESP_OK)err=feishu_cache_draft_begin(&writer);
        if(err==ESP_OK)err=feishu_cache_text_append(&writer,content,strlen(content));
        if(err==ESP_OK && !feishu_outbox_create(&pending,writer.id,writer.raw,key,chat_id,"",uuid))err=ESP_ERR_INVALID_RESPONSE;
        if(err==ESP_OK)err=feishu_cache_pending_write((unsigned)slot,&pending,&writer);
        feishu_cache_text_abort(&writer);
        bool persisted=err==ESP_OK;
        ESP_LOGI(TAG,"ASSISTANT_DRAFT persisted=%u result=%d slot=%d",(unsigned)persisted,err,slot);
        if(persisted)(void)feishu_cache_prune_drafts(slot,true);
        else {
            /* Tool text is bounded below FEISHU_REPLY_MAX. Keep the complete
             * short payload available for physical confirmation even if flash fails. */
            memset(&pending,0,sizeof(pending));pending.version=1;pending.state=FEISHU_OUTBOX_READY;
            pending.length=strlen(content);feishu_utf8_copy(pending.chat,sizeof(pending.chat),chat_id);
            feishu_utf8_copy(pending.uuid,sizeof(pending.uuid),uuid);slot=RAM_REPLY_SLOT;
        }
        xSemaphoreTake(s_state_lock,portMAX_DELAY);
        s_state.pending=pending;s_state.pending_slot=slot;s_state.pending_retry_warning=false;s_state.reply_from_assistant=true;
        feishu_utf8_copy(s_state.reply,sizeof(s_state.reply),content);
        feishu_utf8_copy(s_state.reply_uuid,sizeof(s_state.reply_uuid),uuid);
        s_state.detail_pages=1;s_state.detail_page=0;s_state.detail_scroll=s_state.detail_scroll_max=0;
        s_state.detail_loaded=!persisted;
        if(!persisted)feishu_mentions_display(content,s_state.detail_text,sizeof(s_state.detail_text));
        xSemaphoreGive(s_state_lock);
        a->draft=true;a->changed=true;a->chat=chat;a->target=FEISHU_PAGE_REVIEW;
#if !defined(FEISHU_ASSISTANT_AUTOTEST) || defined(FEISHU_GLOBAL_AUTOTEST)
        a->composition=true;a->changed=false;
        xSemaphoreTake(s_state_lock,portMAX_DELAY);
        s_state.nav.page=FEISHU_PAGE_REVIEW;s_state.nav.chat_index=chat;s_state.nav.reply_to_message=pending.message[0]!=0;
        s_state.assistant_composing=true;s_state.compose_send_confirm=false;
        s_state.loading=false;s_state.detail_loaded=true;
        feishu_mentions_display(content,s_state.detail_text,sizeof(s_state.detail_text));
        xSemaphoreGive(s_state_lock);
#endif

        cJSON_AddStringToObject(result,"recipient",chat_name);
        cJSON_AddBoolToObject(result,"persisted",persisted);
        cJSON_AddStringToObject(result,"result",persisted ? "Message prepared. NOT SENT. User must confirm recipient and text on the device.":
            "Message prepared in RAM. NOT saved and NOT SENT. It can still be confirmed and sent on the device; power loss discards it.");ok=true;
    }
formatted:;
    bool fits=cJSON_PrintPreallocated(result,text,(int)capacity,false);
    if(!fits && !strcmp(name,"self.feishu.get_messages")) {
        /* JSON escaping can double an otherwise bounded text preview. Keep a
         * smaller UTF-8 prefix instead of making the same query fail forever. */
        cJSON *preview=cJSON_GetObjectItemCaseSensitive(result,"text");
        if(cJSON_IsString(preview) && strlen(preview->valuestring)>96) {
            size_t cut=96;while(cut && ((unsigned char)preview->valuestring[cut]&0xc0)==0x80)--cut;
            preview->valuestring[cut]=0;
            cJSON *truncated=cJSON_GetObjectItemCaseSensitive(result,"truncated");
            if(cJSON_IsBool(truncated))cJSON_SetBoolValue(truncated,true);
            fits=cJSON_PrintPreallocated(result,text,(int)capacity,false);
        }
    }
    if(!fits) {
        snprintf(text,capacity,"Result exceeds preview limit; narrow the query.");ok=false;
    }
    if(ok) {
        if(!strcmp(name,"self.feishu.list_chats"))a->tools_mask|=1;
        if(!strcmp(name,"self.feishu.get_messages"))a->tools_mask|=2;
        if(!strcmp(name,"self.feishu.open_chat"))a->tools_mask|=4;
        if(!strcmp(name,"self.feishu.navigate"))a->tools_mask|=8;
        if(!strcmp(name,"self.feishu.prepare_message"))a->tools_mask|=16;
    }
done:
    a->tool_failed=!ok;
    feedback_request(ok ? FEISHU_SOUND_SUCCESS:FEISHU_SOUND_FAILURE);
    free(formatted);cJSON_Delete(result);return ok;
}
#ifdef FEISHU_DRAFT_RAM_AUTOTEST
static bool __attribute__((noinline)) assistant_target_guard_test(assistant_ui_t *state)
{
    char *output=calloc(1,640);cJSON *query=cJSON_CreateObject(),*args=cJSON_CreateObject();
    bool pass=false;
    if(output && query && args){
        cJSON_AddStringToObject(query,"query","__missing_draft_probe__");
        cJSON_AddNumberToObject(args,"chat_index",0);cJSON_AddStringToObject(args,"text","target guard fixture");
        bool lookup=assistant_tool(state,"self.feishu.list_chats",query,output,640);
        bool denied=!assistant_tool(state,"self.feishu.prepare_message",args,output,640);
        cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(query,"query"),"");
        bool all=assistant_tool(state,"self.feishu.list_chats",query,output,640);
        bool denied_again=!assistant_tool(state,"self.feishu.prepare_message",args,output,640);
        xSemaphoreTake(s_state_lock,portMAX_DELAY);
        if(s_state.nav.chat_count)cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(query,"query"),s_state.chats[0].name);
        xSemaphoreGive(s_state_lock);
        bool refined=assistant_tool(state,"self.feishu.list_chats",query,output,640);
        bool latched=!assistant_tool(state,"self.feishu.prepare_message",args,output,640);
        pass=lookup && denied && all && denied_again && refined && latched && !state->draft;
    }
    cJSON_Delete(query);cJSON_Delete(args);free(output);
    assistant_event(state,FEISHU_ASSISTANT_HEARD,"");state->tools_mask=0;
    ESP_LOGI(TAG,"ASSISTANT_TARGET_GUARD pass=%u sends=0",(unsigned)pass);return pass;
}
#endif
static void assistant_enter(void)
{
    if(!s_state_lock)return;
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    bool start=!s_state.closing && !s_state.assistant_active && !s_state.assistant_requested;
    if(start){
        s_state.assistant_exit_requested=false;s_state.assistant_timed_out=false;
        s_state.assistant_phase=FEISHU_ASSISTANT_CONNECTING;s_state.assistant_memory_retried=false;
        if(!s_state.assistant_enabled){
            s_state.assistant_greeted=false;
            s_state.assistant_activity_ms=(uint32_t)(esp_timer_get_time()/1000);
        }
        if(s_network_ui)network_close_locked();
        s_state.assistant_requested=online();s_state.assistant_enabled=s_state.assistant_requested;
        s_state.assistant_composing=s_state.nav.page==FEISHU_PAGE_REVIEW;
        s_state.compose_send_confirm=false;
        s_state.stop_recording=s_state.cancel_recording=true;
        s_state.cancel_pairing=true;++s_state.pairing_request;s_state.pairing_busy=false;
        s_state.status_error=!s_state.assistant_requested;
        feishu_utf8_copy(s_state.status,sizeof(s_state.status),s_state.assistant_requested ? "正在进入小智助手…":"网络未连接，联网后可重试");
        start=s_state.assistant_requested;
    }
    xSemaphoreGive(s_state_lock);
    if(start){feishu_api_cancel_prefetch();enqueue(WORK_ASSISTANT);}
}
/* Non-blocking shortcut. The worker owns audio teardown; clearing the
 * active flag here would let another request reuse still-live resources. */
static void assistant_toggle(void)
{
    if(!s_state_lock)return;
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    if(s_state.closing){xSemaphoreGive(s_state_lock);return;}
    feishu_assistant_toggle_t action=feishu_assistant_toggle_action(
        s_state.assistant_active || s_state.assistant_enabled,s_state.assistant_requested,s_state.assistant_exit_requested);
    if(action==FEISHU_ASSISTANT_DEACTIVATE){
        s_state.assistant_phase=FEISHU_ASSISTANT_STOPPING;
        s_state.assistant_requested=s_state.assistant_enabled=false;s_state.assistant_exit_requested=true;
        s_state.stop_recording=s_state.cancel_recording=true;
        s_state.reply_from_assistant=false;s_state.assistant_send_notice=0;
        s_state.status_error=false;
        if(!s_state.assistant_active && s_state.nav.page==FEISHU_PAGE_ASSISTANT)
            s_state.nav.page=FEISHU_PAGE_CHATS;
        feishu_utf8_copy(s_state.status,sizeof(s_state.status),s_state.assistant_active ?
            "正在关闭小智助手…":"小智助手已关闭");
    }
    xSemaphoreGive(s_state_lock);
    if(action==FEISHU_ASSISTANT_ACTIVATE)assistant_enter();
    else if(action==FEISHU_ASSISTANT_DEACTIVATE)feishu_api_cancel_prefetch();
}
#ifdef FEISHU_GLOBAL_AUTOTEST
static bool assistant_diagnostic_ready(void *context);
#endif
#if !defined(FEISHU_ASSISTANT_AUTOTEST) || defined(FEISHU_GLOBAL_AUTOTEST)
static bool assistant_work_pending(void *context)
{
#if defined(FEISHU_GLOBAL_AUTOTEST) && !defined(FEISHU_ASSISTANT_TIMEOUT_AUTOTEST)
    assistant_ui_t *a=context;
    bool setup_ready=assistant_diagnostic_ready(a);
    if(setup_ready && !a->global_injected && s_state.elapsed_ms>=100){
        a->global_key_sound=feishu_asr_feedback(FEISHU_SOUND_KEY,true,false)==ESP_OK;
        ESP_LOGI(TAG,"ASSISTANT_LIVE_FEEDBACK pass=%u",(unsigned)a->global_key_sound);
        xSemaphoreTake(s_state_lock,portMAX_DELAY);
        s_state.nav.page=FEISHU_PAGE_MESSAGES;s_state.nav.chat_index=0;s_state.loading=true;
        xSemaphoreGive(s_state_lock);
        a->global_injected=true;enqueue(WORK_LOAD_MESSAGES);
        ESP_LOGI(TAG,"ASSISTANT_GLOBAL_IDLE queued=1");
    }
#else
    (void)context;
#endif
    assistant_ui_t *state=context;TickType_t now=xTaskGetTickCount();
    if((int32_t)(now-state->battery_at)>=0){
        int percent=bsp_battery_soc();
        xSemaphoreTake(s_state_lock,portMAX_DELAY);s_state.battery_percent=percent;xSemaphoreGive(s_state_lock);
        state->battery_at=now+pdMS_TO_TICKS(15000);
#ifdef FEISHU_GLOBAL_AUTOTEST
        ESP_LOGI(TAG,"ASSISTANT_CAPTURE_BATTERY retained=1 elapsed=%u",s_state.elapsed_ms);
        TaskHandle_t ui_task=xTaskGetHandle("taskLVGL");
        ESP_LOGI(TAG,"ASSISTANT_UI_STACK allocated=%u free_min=%u jpeg_builtin=%u",
            LV_USE_TJPGD ? 12288u:7168u,ui_task ? (unsigned)uxTaskGetStackHighWaterMark(ui_task):0,(unsigned)LV_USE_TJPGD);
#endif
    }
    if(xSemaphoreTake(s_state_lock,0)!=pdTRUE)return uxQueueMessagesWaiting(s_work_queue)>0;
    bool confirm=s_state.compose_send_confirm;
    bool refresh=s_state.ready && !s_state.loading && online() &&
        (s_state.nav.page==FEISHU_PAGE_CHATS || s_state.nav.page==FEISHU_PAGE_MESSAGES);
    xSemaphoreGive(s_state_lock);
    return state->changed || state->read_deferred || confirm || uxQueueMessagesWaiting(s_work_queue)>0 ||
        (refresh && (int32_t)(now-state->refresh_at)>=0);
}
/* Only explicit queued work or an MCP page selection preempts a reply.
 * Periodic refresh/battery polling must not cut off normal speech. */
static bool assistant_reply_work_pending(void *context,bool playing)
{
    assistant_ui_t *a=context;
#ifdef FEISHU_LVGL_RECOVERY_AUTOTEST
    if(playing && !a->reply_injected){
        a->reply_injected=true;a->ui_before=atomic_load(&s_ui_heartbeat);
        atomic_store(&s_glyph_inject,1);
        xSemaphoreTake(s_state_lock,portMAX_DELAY);
        s_state.nav.page=FEISHU_PAGE_MESSAGES;s_state.nav.chat_index=0;s_state.loading=true;
        xSemaphoreGive(s_state_lock);enqueue(WORK_LOAD_MESSAGES);
        ESP_LOGI(TAG,"ASSISTANT_REPLY_WORK_TEST queued=1 playing=1");
    }
#endif
    (void)a; /* Tool-selected navigation belongs to this reply, not a new interrupt. */
#ifdef FEISHU_GLOBAL_AUTOTEST
    /* The cache-hit path can queue this during playback. Reproduce that exact
     * background event without pretending it is a physical user command. */
    if(playing && a->global_job && !a->background_refresh_injected){
        work_command_t refresh=WORK_REFRESH_MESSAGES;
        if(xQueueSend(s_work_queue,&refresh,0)==pdTRUE){a->background_refresh_injected=true;
            ESP_LOGI(TAG,"ASSISTANT_REPLY_BACKGROUND injected=1 command=%u",(unsigned)refresh);}
    }
#endif
    work_command_t command;
    if(xQueuePeek(s_work_queue,&command,0)!=pdTRUE)return false;
#ifdef FEISHU_GLOBAL_AUTOTEST
    if(playing && command==WORK_REFRESH_MESSAGES && !a->background_refresh_deferred){
        a->background_refresh_deferred=true;ESP_LOGI(TAG,"ASSISTANT_REPLY_BACKGROUND deferred=1");
    }
#endif
    return feishu_assistant_reply_interrupt(playing,
        command!=WORK_FEEDBACK && command!=WORK_AUDIO_FOCUS && command!=WORK_ASSISTANT &&
        command!=WORK_REFRESH_MESSAGES);
}
static bool assistant_continue(void *context);
#ifdef FEISHU_COMPOSE_FLOW_AUTOTEST
#include "feishu_assistant_controls_test.inc"
static void render(lv_timer_t *timer);
static bool __attribute__((noinline)) assistant_flow_step(assistant_ui_t *a)
{
    if(a->flow_done)return true;
    if(a->flow_started){
        if(esp_timer_get_time()<a->flow_deadline)return true;
        xSemaphoreTake(s_state_lock,portMAX_DELAY);
        ESP_LOGI(TAG,"ASSISTANT_CONFIRM_STATE before=1 page=%u confirm=%u loading=%u sending=%u composing=%u length=%u text=%u epoch=%u owner=%u",
            (unsigned)s_state.nav.page,(unsigned)s_state.compose_send_confirm,(unsigned)s_state.loading,(unsigned)s_state.sending,
            (unsigned)s_state.assistant_composing,(unsigned)s_state.pending.length,(unsigned)strlen(s_state.reply),a->epoch,s_owner_epoch);
        xSemaphoreGive(s_state_lock);
        handle_navigation(FEISHU_NAV_BACK);
        xSemaphoreTake(s_state_lock,portMAX_DELAY);
        ESP_LOGI(TAG,"ASSISTANT_CONFIRM_STATE before=0 page=%u confirm=%u loading=%u sending=%u composing=%u length=%u text=%u epoch=%u owner=%u",
            (unsigned)s_state.nav.page,(unsigned)s_state.compose_send_confirm,(unsigned)s_state.loading,(unsigned)s_state.sending,
            (unsigned)s_state.assistant_composing,(unsigned)s_state.pending.length,(unsigned)strlen(s_state.reply),a->epoch,s_owner_epoch);
        bool retained=!s_state.compose_send_confirm && !s_state.sending && s_state.pending.length && s_state.reply[0];
        xSemaphoreGive(s_state_lock);
        work_command_t discard;
        while(xQueuePeek(s_work_queue,&discard,0)==pdTRUE && (discard==WORK_FEEDBACK || discard==WORK_AUDIO_FOCUS)){
            (void)xQueueReceive(s_work_queue,&discard,0);
            if(discard==WORK_FEEDBACK)atomic_store(&s_feedback_queued,false);
            if(discard==WORK_AUDIO_FOCUS)atomic_store(&s_audio_focus_queued,false);
        }
        char *result=calloc(1,640);cJSON *edit=cJSON_CreateObject(),*snapshot=NULL;
        bool resumed=result && edit && assistant_compose_tool(a,"self.feishu.read_draft",edit,result,640);
        if(resumed)snapshot=cJSON_Parse(result);
        const cJSON *version=cJSON_GetObjectItemCaseSensitive(snapshot,"revision");
        if(cJSON_IsString(version)){
            cJSON_AddStringToObject(edit,"revision",version->valuestring);
            cJSON_AddStringToObject(edit,"text","Revised after cancelling confirmation; never transmitted.");
        } else resumed=false;
        if(resumed)resumed=assistant_compose_tool(a,"self.feishu.edit_draft",edit,result,640);
        cJSON_Delete(snapshot);cJSON_Delete(edit);free(result);
        /* Exercise the actual loading renderer with the outgoing draft kept
         * visible. This worker never runs a POST for this fixture. */
        xSemaphoreTake(s_state_lock,portMAX_DELAY);
        s_state.nav.page=FEISHU_PAGE_REVIEW;s_state.loading=s_state.sending=true;
        xSemaphoreGive(s_state_lock);
        bool send_view=false;
        if(bsp_lvgl_lock(2000)){
            render(NULL);
            send_view=!strcmp(lv_label_get_text(s_title),"正在发送") &&
                strstr(lv_label_get_text(s_content),"Revised after cancelling confirmation") &&
                strstr(lv_label_get_text(s_content),"发送到：");
            bsp_lvgl_unlock();
        }
        xSemaphoreTake(s_state_lock,portMAX_DELAY);
        s_state.loading=s_state.sending=false;
        xSemaphoreGive(s_state_lock);
        ESP_LOGI(TAG,"ASSISTANT_SEND_VIEW pass=%u retained=1 sends=0",(unsigned)send_view);
        a->flow_pass=a->flow_pass && send_view;
        enqueue(WORK_SEND);
        /* No POST is authorized by the diagnostic. Exercise ownership handoff
         * with sending=false, then remove our exact queued command. */
        a->flow_done=true;
        bool relinquished=!assistant_continue(a) && a->handoff;
        work_command_t command=WORK_ASSISTANT;
        bool queued=xQueueReceive(s_work_queue,&command,0)==pdTRUE && command==WORK_SEND;
        a->handoff=false;
        xSemaphoreTake(s_state_lock,portMAX_DELAY);
        s_state.pending_slot=-1;memset(&s_state.pending,0,sizeof(s_state.pending));s_state.reply[0]=0;
        s_state.nav.page=FEISHU_PAGE_CHATS;s_state.assistant_composing=false;s_state.stop_recording=false;
        s_state.status[0]=0;s_state.status_error=false;
        xSemaphoreGive(s_state_lock);
        a->composition=a->draft=a->tool_failed=false;free(a->undo);a->undo=NULL;
        a->flow_pass=a->flow_pass && retained && resumed && relinquished && queued;
        ESP_LOGI(TAG,"ASSISTANT_COMPOSE_FLOW pass=%u retained=%u handoff=%u queued=%u sends=0",(unsigned)a->flow_pass,(unsigned)retained,(unsigned)relinquished,(unsigned)queued);
        return a->flow_pass;
    }
    struct flow_work {char text[640],revision[18];} *w=calloc(1,sizeof(*w));
    cJSON *args=cJSON_CreateObject(),*reply=NULL;
    if(!w || !args){free(w);cJSON_Delete(args);return false;}
    if(!assistant_controls_native_test(a)){free(w);cJSON_Delete(args);return false;}
    assistant_event(a,FEISHU_ASSISTANT_THINKING,"正在处理…");assistant_event(a,FEISHU_ASSISTANT_HEARD,"测试指令");
    xSemaphoreTake(s_state_lock,portMAX_DELAY);bool phase=strcmp(s_state.status,"小智聆听中")!=0;xSemaphoreGive(s_state_lock);
    assistant_event(a,FEISHU_ASSISTANT_REPLY,"测试答复");
    xSemaphoreTake(s_state_lock,portMAX_DELAY);phase=phase && strcmp(s_state.status,"小智聆听中")!=0;xSemaphoreGive(s_state_lock);
    /* No recipient on the conversation-list page must never guess a chat.
     * Inside a known conversation the same empty arguments open its editor. */
    xSemaphoreTake(s_state_lock,portMAX_DELAY);s_state.nav.page=FEISHU_PAGE_CHATS;xSemaphoreGive(s_state_lock);
    bool no_guess=!assistant_tool(a,"self.feishu.open_composer",args,w->text,sizeof(w->text));
    xSemaphoreTake(s_state_lock,portMAX_DELAY);s_state.nav.page=FEISHU_PAGE_MESSAGES;s_state.nav.chat_index=0;xSemaphoreGive(s_state_lock);
    bool opened=no_guess && assistant_tool(a,"self.feishu.open_composer",args,w->text,sizeof(w->text));
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    opened=opened && s_state.nav.page==FEISHU_PAGE_REVIEW && s_state.assistant_composing && !s_state.loading;
    xSemaphoreGive(s_state_lock);
    ESP_LOGI(TAG,"ASSISTANT_EDITOR_CONTEXT no_guess=%u opened=%u",(unsigned)no_guess,(unsigned)opened);
    bool read=opened && assistant_compose_tool(a,"self.feishu.read_draft",args,w->text,sizeof(w->text));
    if(read)reply=cJSON_Parse(w->text);
    const cJSON *rev=cJSON_GetObjectItemCaseSensitive(reply,"revision"),*body=cJSON_GetObjectItemCaseSensitive(reply,"text");
    bool empty=read && cJSON_IsString(rev) && cJSON_IsString(body) && !body->valuestring[0];
    if(empty)feishu_utf8_copy(w->revision,sizeof(w->revision),rev->valuestring);
    cJSON_Delete(reply);cJSON_AddStringToObject(args,"revision",w->revision);
    bool rejected=empty && !assistant_compose_tool(a,"self.feishu.request_send",args,w->text,sizeof(w->text));
    cJSON_AddStringToObject(args,"text","Native collaborative editor test; never transmitted.");
    bool edited=rejected && assistant_compose_tool(a,"self.feishu.edit_draft",args,w->text,sizeof(w->text));
    if(edited)reply=cJSON_Parse(w->text);else reply=NULL;
    rev=cJSON_GetObjectItemCaseSensitive(reply,"revision");
    if(cJSON_IsString(rev))feishu_utf8_copy(w->revision,sizeof(w->revision),rev->valuestring);else edited=false;
    cJSON_Delete(reply);cJSON_DeleteItemFromObjectCaseSensitive(args,"text");
    bool stale=edited && !assistant_compose_tool(a,"self.feishu.request_send",args,w->text,sizeof(w->text));
    cJSON_ReplaceItemInObjectCaseSensitive(args,"revision",cJSON_CreateString(w->revision));
    bool confirm=stale && assistant_compose_tool(a,"self.feishu.request_send",args,w->text,sizeof(w->text));
    xSemaphoreTake(s_state_lock,portMAX_DELAY);bool safe=s_state.compose_send_confirm && !s_state.sending && s_state.pending.length;xSemaphoreGive(s_state_lock);
    a->flow_pass=phase && opened && empty && rejected && edited && stale && confirm && safe;
    a->flow_started=true;a->flow_deadline=esp_timer_get_time()+18000000;
    ESP_LOGI(TAG,"ASSISTANT_COMPOSE_BEGIN pass=%u phase=%u empty=%u rejected=%u edited=%u stale=%u confirm=%u sends=0",
        (unsigned)a->flow_pass,(unsigned)phase,(unsigned)empty,(unsigned)rejected,(unsigned)edited,(unsigned)stale,(unsigned)confirm);
    cJSON_Delete(args);free(w);return a->flow_pass;
}
#endif
#if defined(FEISHU_SCENARIO_AUTOTEST) || defined(FEISHU_GLOBAL_AUTOTEST)
static bool s_scenario_quiet;
static bool scenario_quiet(void)
{
    if(s_scenario_quiet)return true;
    feishu_assistant_settings_t settings;feishu_sound_settings_t sounds,verify;
    if(feishu_store_load_assistant(&settings)!=ESP_OK || feishu_store_load_sounds(&sounds)!=ESP_OK)return false;
    unsigned original=(sounds.success ? 1:0)|(sounds.failure ? 2:0)|(sounds.key ? 4:0),volume=settings.volume;
    sounds=(feishu_sound_settings_t){false,false,false};settings.volume=0;
    bool ok=feishu_store_save_sounds(&sounds)==ESP_OK && feishu_store_save_assistant(&settings)==ESP_OK &&
        feishu_store_load_sounds(&verify)==ESP_OK && !verify.success && !verify.failure && !verify.key;
    feishu_assistant_settings_t after;
    ok=ok && feishu_store_load_assistant(&after)==ESP_OK && after.volume==0;
    ESP_LOGI(TAG,"ASSISTANT_QUIET_PREF original_mask=%u original_volume=%u saved=%u",original,volume,(unsigned)ok);
    if(!ok)return false;
    xSemaphoreTake(s_state_lock,portMAX_DELAY);s_state.sounds=sounds;s_state.assistant_settings=settings;
    s_state.assistant_settings.timeout_minutes=0; /* RAM-only stress override. */
    xSemaphoreGive(s_state_lock);s_scenario_quiet=true;return true;
}
#endif
#ifdef FEISHU_SCENARIO_AUTOTEST
static bool s_scenario_done,s_scenario_pass;
static unsigned scenario_next(unsigned *state)
{*state^=*state<<13;*state^=*state>>17;*state^=*state<<5;return *state;}
static bool scenario_drain(void)
{
    work_command_t command;unsigned count=0;
    while(xQueueReceive(s_work_queue,&command,0)==pdTRUE){
        if(++count>12)return false;
        switch(command){
        case WORK_LOAD_MESSAGES:case WORK_REFRESH_MESSAGES:case WORK_LOAD_TEXT_PAGE:case WORK_FETCH_TEXT:
        case WORK_LOAD_DRAFT_PAGE:case WORK_LIST_DRAFTS:case WORK_MESSAGE_PAGE:case WORK_FETCH_MESSAGE_PAGE:
        case WORK_LOAD_IMAGE:case WORK_FETCH_IMAGE:case WORK_FEEDBACK:case WORK_AUDIO_FOCUS:
            process_work_command(command);break;
        default:ESP_LOGE(TAG,"ASSISTANT_SCENARIO_FORBIDDEN command=%u",(unsigned)command);return false;
        }
    }
    return true;
}
static bool __attribute__((noinline)) scenario_run(assistant_ui_t *a)
{
    work_command_t priority;
    if(xQueuePeek(s_work_queue,&priority,0)==pdTRUE && priority==WORK_SEND)return true;
    if(s_scenario_done)return s_scenario_pass;
#ifdef FEISHU_COMPOSE_FLOW_AUTOTEST
    if(!a->flow_done)return true;
#endif
    s_scenario_done=true;
#ifdef FEISHU_NATIVE_FOCUSED_CASES
    bool storage=assistant_storage_idle_native_test();
    s_scenario_pass=storage && assistant_detail_native_test(a);
    xSemaphoreTake(s_state_lock,portMAX_DELAY);s_state.nav.page=FEISHU_PAGE_CHATS;s_state.loading=false;
    s_state.status[0]=0;s_state.status_error=false;s_state.assistant_composing=false;xSemaphoreGive(s_state_lock);
    a->changed=a->composition=a->tool_failed=false;a->tools_mask=0;
    ESP_LOGI(TAG,"ASSISTANT_FOCUSED_DONE pass=%u storage=%u sends=0",(unsigned)s_scenario_pass,(unsigned)storage);
    return s_scenario_pass;
#endif
    unsigned groups; xSemaphoreTake(s_state_lock,portMAX_DELAY);groups=(unsigned)s_state.nav.chat_count;xSemaphoreGive(s_state_lock);
    if(!groups || groups>FEISHU_MAX_CHATS)return false;
    char *result=calloc(1,640);cJSON *args=cJSON_CreateObject();
    if(!result || !args){free(result);cJSON_Delete(args);return false;}
    const unsigned seeds[]={0x00260021u,0x0badc0deu,0x00658731u};
    unsigned reads=0,warm=0,operations=0,cancelled=0,backs=0,details=0,images=0;
    bool ok=true;
    for(unsigned pass=0;pass<3 && ok;++pass){
        unsigned rng=seeds[pass],order[FEISHU_MAX_CHATS];for(unsigned i=0;i<groups;++i)order[i]=i;
        for(unsigned i=groups;i>1;--i){unsigned j=scenario_next(&rng)%i,t=order[i-1];order[i-1]=order[j];order[j]=t;}
        if(pass==0 && groups>3){for(unsigned i=0;i<groups;++i)if(order[i]==3){unsigned t=order[0];order[0]=3;order[i]=t;break;}}
        for(unsigned k=0;k<groups && ok;++k){
            unsigned chat=order[k];
            xSemaphoreTake(s_state_lock,portMAX_DELAY);
            s_state.nav.page=FEISHU_PAGE_MESSAGES;s_state.nav.chat_index=chat;s_state.nav.composer_selected=false;
            s_state.message_page_number=0;s_state.message_page_loading=false;s_state.loaded_chat[0]=0;s_state.loading=true;
            s_state.assistant_phase=FEISHU_ASSISTANT_PROCESSING;++s_state.message_page_request;
            xSemaphoreGive(s_state_lock);
            esp_err_t err=load_messages_for_chat(chat,false,false);++reads;
            if(err==ESP_OK){err=load_messages_for_chat(chat,false,true);++warm;}
            xSemaphoreTake(s_state_lock,portMAX_DELAY);bool ready=!s_state.loading && s_state.nav.page==FEISHU_PAGE_MESSAGES &&
                !strcmp(s_state.loaded_chat,s_state.chats[chat].chat_id);unsigned rows=(unsigned)s_state.nav.message_count;
            xSemaphoreGive(s_state_lock);
            ESP_LOGI(TAG,"ASSISTANT_SCENARIO_READ seed=%u chat=%u rows=%u result=%d ready=%u heap=%u largest=%u",
                seeds[pass],chat,rows,err,(unsigned)ready,(unsigned)esp_get_free_heap_size(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
            ok=err==ESP_OK && ready && feishu_http_progress();
        }
        /* Cancel a real cold HTTPS request from the actual LVGL/input task. */
        if(ok){
            xSemaphoreTake(s_state_lock,portMAX_DELAY);s_state.nav.page=FEISHU_PAGE_MESSAGES;s_state.nav.chat_index=order[0];
            s_state.loading=true;s_state.loaded_chat[0]=0;s_state.message_page_number=0;s_state.message_page_loading=false;
            ++s_state.message_page_request;xSemaphoreGive(s_state_lock);
            unsigned before=atomic_load(&s_scenario_back_count);
            atomic_store(&s_scenario_cancel_at,(unsigned)(esp_timer_get_time()/1000)+250);
            esp_err_t err=load_messages_for_chat(order[0],false,false);
            for(unsigned n=0;n<20 && atomic_load(&s_scenario_cancel_at);++n)vTaskDelay(pdMS_TO_TICKS(50));
            xSemaphoreTake(s_state_lock,portMAX_DELAY);bool recovered=s_state.nav.page==FEISHU_PAGE_CHATS && !s_state.loading;xSemaphoreGive(s_state_lock);
            bool back=atomic_load(&s_scenario_back_count)==before+1;
            bool expected=err==FEISHU_HTTP_ERR_CANCELLED || err==ESP_OK;
            ok=back && recovered && expected;backs+=ok;cancelled+=ok && err==FEISHU_HTTP_ERR_CANCELLED;
            ESP_LOGI(TAG,"ASSISTANT_SCENARIO_CANCEL seed=%u result=%d back=%u recovered=%u",seeds[pass],err,(unsigned)back,(unsigned)recovered);
        }
        for(unsigned n=0;n<48 && ok;++n){
            unsigned action=scenario_next(&rng)%10,chat=scenario_next(&rng)%groups;
            cJSON_Delete(args);args=cJSON_CreateObject();if(!args){ok=false;break;}
            switch(action){
            case 0:handle_navigation(FEISHU_NAV_UP);break;
            case 1:handle_navigation(FEISHU_NAV_DOWN);break;
            case 2:demo_feishu_key(BSP_BTN_UP,BSP_BTN_DOUBLE);break;
            case 3:demo_feishu_key(BSP_BTN_DOWN,BSP_BTN_DOUBLE);break;
            case 4:demo_feishu_back();break;
            case 5:
                cJSON_AddStringToObject(args,"page","settings");ok=assistant_tool(a,"self.feishu.navigate",args,result,640);a->changed=false;break;
            case 6:
                cJSON_AddNumberToObject(args,"chat_index",chat);ok=assistant_tool(a,"self.feishu.open_chat",args,result,640);a->changed=false;
                if(ok){esp_err_t err=load_messages_for_chat(chat,false,true);if(err==ESP_ERR_NOT_FOUND)err=load_messages_for_chat(chat,false,false);if(err!=ESP_OK)ok=false;}break;
            case 7:ok=assistant_compose_tool(a,"self.feishu.get_context",args,result,640);break;
            case 8:{
                xSemaphoreTake(s_state_lock,portMAX_DELAY);bool list=s_state.nav.page==FEISHU_PAGE_MESSAGES && s_state.nav.message_count;
                if(list){s_state.nav.message_index=0;s_state.nav.composer_selected=false;}
                bool image=list && s_state.messages[0].type==FEISHU_MESSAGE_IMAGE;
                bool allow=list && (image ? images<2:details<9);
                xSemaphoreGive(s_state_lock);
                if(allow){handle_navigation(FEISHU_NAV_OK);if(image)++images;else ++details;}
                break;
            }
            default:
                cJSON_AddStringToObject(args,"page","drafts");ok=assistant_tool(a,"self.feishu.navigate",args,result,640);a->changed=false;
                if(ok){process_work_command(WORK_LIST_DRAFTS);}break;
            }
            ok=ok && scenario_drain() && feishu_http_progress();++operations;
            vTaskDelay(pdMS_TO_TICKS(120));
        }
        ESP_LOGI(TAG,"ASSISTANT_SCENARIO_SEED seed=%u pass=%u operations=%u",seeds[pass],(unsigned)ok,operations);
    }
    cJSON_Delete(args);free(result);
    if(ok)ok=assistant_detail_native_test(a);
    xSemaphoreTake(s_state_lock,portMAX_DELAY);s_state.nav.page=FEISHU_PAGE_CHATS;s_state.loading=false;
    s_state.status[0]=0;s_state.status_error=false;s_state.assistant_composing=false;xSemaphoreGive(s_state_lock);
    a->changed=a->composition=a->tool_failed=false;a->tools_mask=0;
    s_scenario_pass=ok && reads==groups*3 && warm==reads && backs==3 && cancelled>0 && operations==144;
    ESP_LOGI(TAG,"ASSISTANT_SCENARIO_DONE pass=%u cold=%u warm=%u operations=%u cancellations=%u backs=%u details=%u images=%u sends=0",
        (unsigned)s_scenario_pass,reads,warm,operations,cancelled,backs,details,images);
    return s_scenario_pass;
}
#endif
static bool assistant_can_listen(void *context)
{
    (void)context;feishu_power_network_poll();
    xSemaphoreTake(s_state_lock,portMAX_DELAY);bool listen=!s_state.sending;xSemaphoreGive(s_state_lock);return listen;
}
static bool assistant_waiting_confirmation(void *context)
{
    (void)context;xSemaphoreTake(s_state_lock,portMAX_DELAY);bool confirm=s_state.compose_send_confirm;xSemaphoreGive(s_state_lock);return confirm;
}
static bool assistant_exclusive_work(work_command_t command)
{
    return command==WORK_SEND || command==WORK_RECORD || command==WORK_INIT || command==WORK_RESET_WIFI ||
        command==WORK_UNBIND || command==WORK_REAUTHORIZE || command==WORK_EXIT ||
        command==WORK_NETWORK_CONNECT || command==WORK_NETWORK_DISCONNECT || command==WORK_CHECK_XIAOZHI;
}
/* One coalesced read belongs to this visit, not the ordinary command queue.
 * It must not be mistaken for a fresh UI command that aborts the cloud reply. */
static bool assistant_read_yielded(assistant_ui_t *a,work_command_t command)
{
    if(feishu_http_progress())return false;
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    bool voice=s_state.assistant_speech_detected && !s_state.cancel_recording;
    xSemaphoreGive(s_state_lock);
    if(voice && (command==WORK_LOAD_MESSAGES || command==WORK_REFRESH_MESSAGES || command==WORK_CHAT_PAGE ||
       command==WORK_MESSAGE_PAGE || command==WORK_LOAD_TEXT_PAGE || command==WORK_FETCH_TEXT || command==WORK_LOAD_DRAFT_PAGE)){
        a->deferred_read=command;a->read_deferred=true;
#ifdef FEISHU_GLOBAL_AUTOTEST
        ESP_LOGI(TAG,"ASSISTANT_READ_DEFERRED stored=1 command=%u",(unsigned)command);
#endif
    }
    return true;
}
static bool assistant_continue(void *context)
{
    assistant_ui_t *a=context;
#ifdef FEISHU_SCENARIO_AUTOTEST
    xSemaphoreTake(s_state_lock,portMAX_DELAY);bool business_phase=s_state.assistant_phase!=FEISHU_ASSISTANT_READY || (!s_state.record_pending && !s_state.stop_recording);xSemaphoreGive(s_state_lock);
    ESP_LOGI(TAG,"ASSISTANT_BUSINESS_PHASE pass=%u",(unsigned)business_phase);
    if(!business_phase)return false;
#endif
#ifdef FEISHU_MEMORY_RECOVERY_AUTOTEST
    /* Do not start a non-idempotent test fixture from the preliminary transport
     * retry before the deliberate memory fault/owner restart has happened. */
    if(!s_state.assistant_memory_retried)return true;
#endif
#ifdef FEISHU_COMPOSE_FLOW_AUTOTEST
    if(!assistant_flow_step(a))return false;
#endif
#ifdef FEISHU_SCENARIO_AUTOTEST
    if(!scenario_run(a))return false;
#endif
#ifdef FEISHU_LVGL_RECOVERY_AUTOTEST
    bool business_probe_done=true;
#ifdef FEISHU_GLOBAL_AUTOTEST
    business_probe_done=a->global_job;
#endif
    if(business_probe_done && a->reply_injected && !a->back_tested && atomic_load(&s_glyph_failures) && atomic_load(&s_ui_heartbeat)>a->ui_before+2){
        xSemaphoreTake(s_state_lock,portMAX_DELAY);
        s_state.nav.page=FEISHU_PAGE_MESSAGES;s_state.loading=false;
        xSemaphoreGive(s_state_lock);
        demo_feishu_back();
        xSemaphoreTake(s_state_lock,portMAX_DELAY);
        a->back_tested=s_state.nav.page==FEISHU_PAGE_CHATS;
        xSemaphoreGive(s_state_lock);
        ESP_LOGI(TAG,"ASSISTANT_UI_RECOVERY_TEST glyph_failures=%u heartbeat=%u back=%u pass=%u",
            atomic_load(&s_glyph_failures),atomic_load(&s_ui_heartbeat)-a->ui_before,(unsigned)a->back_tested,(unsigned)a->back_tested);
    }
#endif
    /* Start capture before ordinary reads after a greeting/reply/reconnect.
     * Exclusive audio/auth/network handoffs and physical confirmation still
     * release the owner; test setup runs once before this production path. */
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    bool owner_valid=a->epoch==s_owner_epoch && !s_state.cancel_recording && !s_state.closing;
    bool defer_reads=s_state.assistant_phase!=FEISHU_ASSISTANT_READY && !s_state.compose_send_confirm;
    xSemaphoreGive(s_state_lock);
    if(!owner_valid)return false;
#ifdef FEISHU_SCENARIO_AUTOTEST
    if(!a->flow_done || !s_scenario_pass)defer_reads=false;
#endif
    if(defer_reads){
        work_command_t pending;
        if(xQueuePeek(s_work_queue,&pending,0)==pdTRUE && assistant_exclusive_work(pending)){a->handoff=true;return false;}
        return true;
    }
    if(a->changed){
        xSemaphoreTake(s_state_lock,portMAX_DELAY);
        bool valid=a->epoch==s_owner_epoch && !s_state.cancel_recording && !s_state.closing;
        bool current=s_state.nav.page==a->target && s_state.nav.chat_index==a->chat;
        if(valid && current){
            s_state.nav.page=a->target;s_state.nav.chat_index=a->chat;s_state.nav.reply_to_message=false;
            s_state.nav.draft_manager=a->target==FEISHU_PAGE_DRAFTS;s_state.nav.settings_index=0;
            s_state.loading=a->target==FEISHU_PAGE_MESSAGES || a->target==FEISHU_PAGE_DRAFTS;
            if(a->target==FEISHU_PAGE_MESSAGES)++s_state.message_page_request;
        }
        xSemaphoreGive(s_state_lock);
        if(!valid)return false;
        a->changed=false;a->read_deferred=false; /* A new page supersedes the old read. */
        if(current && a->target==FEISHU_PAGE_MESSAGES){
            process_work_command(WORK_LOAD_MESSAGES);
            if(assistant_read_yielded(a,WORK_LOAD_MESSAGES))return true;
        } else if(current && a->target==FEISHU_PAGE_DRAFTS)process_work_command(WORK_LIST_DRAFTS);
    }
    /* Capture may remain alive. Its bounded queue wins over cancellable reads;
     * exclusive send/record operations still release the voice owner first. */
    for(unsigned i=0;i<2;++i){
        work_command_t command;
        if(!feishu_http_progress())break;
        if(xQueuePeek(s_work_queue,&command,0)==pdTRUE){
            if(assistant_exclusive_work(command)){a->handoff=true;return false;}
            (void)xQueueReceive(s_work_queue,&command,0);
        } else if(a->read_deferred){
            command=a->deferred_read;a->read_deferred=false;
#ifdef FEISHU_GLOBAL_AUTOTEST
            ESP_LOGI(TAG,"ASSISTANT_READ_DEFERRED resumed=1 command=%u",(unsigned)command);
#endif
        } else break;
        if(command==WORK_AUDIO_FOCUS){atomic_store(&s_audio_focus_queued,false);continue;}
        if(command==WORK_ASSISTANT)continue;
        process_work_command(command);
        if(assistant_read_yielded(a,command))break;
#ifdef FEISHU_GLOBAL_AUTOTEST
        if(command==WORK_LOAD_MESSAGES && a->global_injected){
            xSemaphoreTake(s_state_lock,portMAX_DELAY);
            a->global_job=s_state.nav.page==FEISHU_PAGE_MESSAGES && !s_state.loading;
            xSemaphoreGive(s_state_lock);
            ESP_LOGI(TAG,"ASSISTANT_GLOBAL_PUMP pass=%u heap=%u stack=%u",(unsigned)a->global_job,
                (unsigned)esp_get_free_heap_size(),(unsigned)uxTaskGetStackHighWaterMark(NULL));
            xSemaphoreTake(s_state_lock,portMAX_DELAY);s_state.nav.page=FEISHU_PAGE_CHATS;xSemaphoreGive(s_state_lock);
            a->refresh_at=xTaskGetTickCount()-1;
            struct member_probe {char chat[64];feishu_mention_t member;} *probe=calloc(1,sizeof(*probe));
            if(probe){
                xSemaphoreTake(s_state_lock,portMAX_DELAY);
                feishu_utf8_copy(probe->chat,sizeof(probe->chat),s_state.chats[0].chat_id);xSemaphoreGive(s_state_lock);
                esp_err_t lookup=feishu_api_resolve_mention(&s_session,probe->chat,"一人一口酥",&probe->member,&s_state.cancel_recording);
                a->global_mentions=lookup==ESP_OK && !strcmp(probe->member.id,s_session.self_open_id);
                ESP_LOGI(TAG,"ASSISTANT_MEMBER_LOOKUP result=%d self=%u minimum=%u",lookup,(unsigned)a->global_mentions,(unsigned)esp_get_minimum_free_heap_size());
                memset(probe,0,sizeof(*probe));free(probe);
            }
        }
#endif
    }
    TickType_t now=xTaskGetTickCount();
    if((int32_t)(now-a->battery_at)>=0){
        int percent=bsp_battery_soc();xSemaphoreTake(s_state_lock,portMAX_DELAY);s_state.battery_percent=percent;xSemaphoreGive(s_state_lock);
        a->battery_at=xTaskGetTickCount()+pdMS_TO_TICKS(15000);
    }
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    feishu_page_t page=s_state.nav.page;
    bool refresh=s_state.ready && !s_state.loading && online() && !s_state.compose_send_confirm;
    xSemaphoreGive(s_state_lock);
    TickType_t refresh_interval=pdMS_TO_TICKS(page==FEISHU_PAGE_MESSAGES ? 3000:8000);
    if(a->refresh_at && (int32_t)(a->refresh_at-now)>(int32_t)refresh_interval)a->refresh_at=now+refresh_interval;
    if(!a->refresh_at)a->refresh_at=now+refresh_interval;
    else if((int32_t)(now-a->refresh_at)>=0 && refresh && uxQueueMessagesWaiting(s_work_queue)==0){
#ifdef FEISHU_GLOBAL_AUTOTEST
        bool retained=s_state.assistant_phase==FEISHU_ASSISTANT_READY;
        unsigned before=s_state.elapsed_ms;
#endif
        if(feishu_power_background_allowed() && page==FEISHU_PAGE_CHATS){
            esp_err_t err=refresh_conversations();recover_revoked_auth(err);
            ESP_LOGI(TAG,"ASSISTANT_REFRESH chats=1 result=%d stack=%u minimum=%u",err,(unsigned)uxTaskGetStackHighWaterMark(NULL),(unsigned)esp_get_minimum_free_heap_size());
        } else if(feishu_power_background_allowed() && page==FEISHU_PAGE_MESSAGES){esp_err_t err=poll_visible_messages();recover_revoked_auth(err);}
        a->refresh_at=xTaskGetTickCount()+refresh_interval;
#ifdef FEISHU_GLOBAL_AUTOTEST
        if(retained)ESP_LOGI(TAG,"ASSISTANT_CAPTURE_REFRESH retained=1 before=%u after=%u",before,s_state.elapsed_ms);
#endif
    }
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    bool valid=a->epoch==s_owner_epoch && !s_state.cancel_recording && !s_state.closing;
    bool composition=s_state.nav.page==FEISHU_PAGE_REVIEW;
    s_state.assistant_composing=composition;
    xSemaphoreGive(s_state_lock);
    if(!composition && a->composition){free(a->undo);a->undo=NULL;}
    a->composition=composition;return valid;
}
#endif
#ifdef FEISHU_DRAFT_RAM_AUTOTEST
static bool __attribute__((noinline)) assistant_compose_native_test(assistant_ui_t *a)
{
    struct test_work {char result[640],original[512],recipient[64],revision[18],updated[18];} *w=calloc(1,sizeof(*w));
    if(!w)return false;
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    bool owned=(a->draft || a->diagnostic_draft) && s_state.pending_slot==RAM_REPLY_SLOT;
    if(owned){s_state.nav.page=FEISHU_PAGE_REVIEW;s_state.assistant_composing=true;
        feishu_utf8_copy(w->original,sizeof(w->original),s_state.reply);
        feishu_utf8_copy(w->recipient,sizeof(w->recipient),s_state.pending.chat);}
    xSemaphoreGive(s_state_lock);
    if(!owned){free(w);return false;}
    cJSON *args=cJSON_CreateObject(),*readback=NULL;
    bool read=args && assistant_compose_tool(a,"self.feishu.read_draft",args,w->result,sizeof(w->result));
    if(read)readback=cJSON_Parse(w->result);
    const cJSON *revision=cJSON_GetObjectItemCaseSensitive(readback,"revision");
    read=read && cJSON_IsString(revision);
    if(read)feishu_utf8_copy(w->revision,sizeof(w->revision),revision->valuestring);
    cJSON_Delete(readback);readback=NULL;
    cJSON_AddStringToObject(args,"revision",w->revision);cJSON_AddStringToObject(args,"text","Revised native composition");
    bool edit=read && assistant_compose_tool(a,"self.feishu.edit_draft",args,w->result,sizeof(w->result));
    if(edit)readback=cJSON_Parse(w->result);
    revision=cJSON_GetObjectItemCaseSensitive(readback,"revision");
    edit=edit && cJSON_IsString(revision);
    if(edit)feishu_utf8_copy(w->updated,sizeof(w->updated),revision->valuestring);
    cJSON_Delete(readback);
    bool stale=edit && !assistant_compose_tool(a,"self.feishu.edit_draft",args,w->result,sizeof(w->result));
    cJSON_DeleteItemFromObjectCaseSensitive(args,"text");
    cJSON_ReplaceItemInObjectCaseSensitive(args,"revision",cJSON_CreateString(w->updated));
    bool undo=edit && assistant_compose_tool(a,"self.feishu.undo_draft",args,w->result,sizeof(w->result));
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    bool target=!strcmp(s_state.pending.chat,w->recipient) && !strcmp(s_state.reply,w->original) && s_state.pending_slot==RAM_REPLY_SLOT;
    s_state.pending.state=FEISHU_OUTBOX_UNCERTAIN;
    xSemaphoreGive(s_state_lock);
    cJSON_DeleteItemFromObjectCaseSensitive(args,"revision");
    bool uncertain=!assistant_compose_tool(a,"self.feishu.read_draft",args,w->result,sizeof(w->result));
    xSemaphoreTake(s_state_lock,portMAX_DELAY);s_state.pending.state=FEISHU_OUTBOX_READY;
    uint64_t saved_length=s_state.pending.length;s_state.pending.length=10000;xSemaphoreGive(s_state_lock);
    bool oversized=!assistant_compose_tool(a,"self.feishu.read_draft",args,w->result,sizeof(w->result));
    xSemaphoreTake(s_state_lock,portMAX_DELAY);s_state.pending.length=saved_length;
    s_state.assistant_speech_detected=s_state.stop_recording=s_state.cancel_recording=false;
    xSemaphoreGive(s_state_lock);
    handle_navigation(FEISHU_NAV_OK);
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    bool confirmation=s_state.compose_send_confirm && !s_state.sending && s_state.nav.page==FEISHU_PAGE_REVIEW;
    s_state.compose_send_confirm=s_state.assistant_composing=false;
    s_state.stop_recording=s_state.cancel_recording=s_state.assistant_exit_requested=false;
    xSemaphoreGive(s_state_lock);
    cJSON_Delete(args);free(a->undo);a->undo=NULL;
    bool pass=read && edit && stale && undo && target && uncertain && oversized && confirmation;
    ESP_LOGI(TAG,"ASSISTANT_COMPOSE_TEST read=%u edit=%u stale=%u undo=%u recipient=%u uncertain=%u oversized=%u confirmation=%u sends=0 pass=%u",
        (unsigned)read,(unsigned)edit,(unsigned)stale,(unsigned)undo,(unsigned)target,(unsigned)uncertain,(unsigned)oversized,(unsigned)confirmation,(unsigned)pass);
    memset(w,0,sizeof(*w));free(w);return pass;
}
#endif
#ifdef FEISHU_GLOBAL_AUTOTEST
static bool assistant_diagnostic_ready(void *context)
{
    assistant_ui_t *a=context;
#ifdef FEISHU_COMPOSE_FLOW_AUTOTEST
    if(!a->flow_done || !a->flow_pass)return false;
#endif
#ifdef FEISHU_SCENARIO_AUTOTEST
    if(!s_scenario_done || !s_scenario_pass)return false;
#endif
    (void)a;return true;
}
#endif
static unsigned assistant_silence(void *context)
{
    (void)context;return atomic_load(&s_assistant_submit_seconds);
}
static unsigned assistant_volume(void *context)
{
    (void)context;
#ifdef FEISHU_ASSISTANT_AUTOTEST
    return 0;
#else
    xSemaphoreTake(s_state_lock,portMAX_DELAY);unsigned v=s_state.assistant_settings.volume;xSemaphoreGive(s_state_lock);return v;
#endif
}
static void assistant_worker(void)
{
#if defined(FEISHU_SCENARIO_AUTOTEST) || defined(FEISHU_GLOBAL_AUTOTEST)
    if(!scenario_quiet()){ESP_LOGE(TAG,"ASSISTANT_QUIET_PREF saved=0");return;}
#endif
    assistant_ui_t state={0};feishu_assistant_hooks_t hooks={.execute=assistant_tool,.event=assistant_event,.context=&state,.volume=60};
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
#if !defined(FEISHU_ASSISTANT_AUTOTEST) || defined(FEISHU_GLOBAL_AUTOTEST)
    if(!s_state.assistant_requested && !s_state.assistant_active){xSemaphoreGive(s_state_lock);return;}
    /* A global-assistant takeover retires the old dictation presentation as
     * well as its microphone owner; never leave a fake recording screen up. */
    if(s_state.nav.page==FEISHU_PAGE_RECORDING || s_state.nav.page==FEISHU_PAGE_TRANSCRIBING ||
       s_state.nav.page==FEISHU_PAGE_REPLY_READY || s_state.nav.page==FEISHU_PAGE_ASSISTANT)
        s_state.nav.page=s_state.nav.chat_index<s_state.nav.chat_count ? FEISHU_PAGE_MESSAGES:FEISHU_PAGE_CHATS;
    state.composition=s_state.assistant_composing;
    s_state.assistant_requested=false;s_state.assistant_phase=FEISHU_ASSISTANT_CONNECTING;
    s_state.assistant_enabled=s_state.assistant_active=true;s_state.record_pending=true;s_state.loading=false;
    s_state.stop_recording=s_state.cancel_recording=false;s_state.elapsed_ms=0;
    s_state.assistant_heard[0]=s_state.assistant_reply[0]=0;feishu_utf8_copy(s_state.status,sizeof(s_state.status),"正在连接小智");
    hooks.skip_greeting=s_state.assistant_greeted;
    hooks.activation_chime=s_state.sounds.success && !hooks.skip_greeting;
    ESP_LOGI(TAG,"ASSISTANT_VISIT_GREETING required=%u",(unsigned)!hooks.skip_greeting);
    hooks.speech_detected=&s_state.assistant_speech_detected;
    hooks.continuous=true;hooks.silence_seconds=s_state.assistant_settings.silence_seconds;
    hooks.volume=s_state.assistant_settings.volume;hooks.get_volume=assistant_volume;hooks.get_silence=assistant_silence;
    unsigned seconds=s_state.assistant_settings.silence_seconds;
    bool manual=s_state.nav.page==FEISHU_PAGE_REVIEW || s_state.nav.page==FEISHU_PAGE_ASSISTANT;
    atomic_store(&s_assistant_submit_seconds,seconds || manual ? seconds:2);
#ifdef FEISHU_GLOBAL_AUTOTEST
    hooks.diagnostic_ready=assistant_diagnostic_ready;
#endif
    hooks.continue_session=assistant_continue;hooks.work_pending=assistant_work_pending;hooks.reply_work_pending=assistant_reply_work_pending;hooks.can_listen=assistant_can_listen;hooks.waiting_confirmation=assistant_waiting_confirmation;hooks.feedback=assistant_feedback;
#endif
    state.epoch=s_owner_epoch;state.send_notice=s_state.assistant_send_notice;s_state.assistant_send_notice=0;
    hooks.status=(feishu_mcp_status_t){"assistant",(unsigned)s_state.nav.chat_count,
        (unsigned)s_state.nav.message_count,s_state.battery_percent,online()};
    xSemaphoreGive(s_state_lock);
    feishu_api_cancel_prefetch();
#if !defined(FEISHU_ASSISTANT_AUTOTEST) || defined(FEISHU_GLOBAL_AUTOTEST)
    s_session.defer_list_bodies=true;
#endif
#ifdef FEISHU_DRAFT_RAM_AUTOTEST
    bool target_guard_pass=assistant_target_guard_test(&state);
#endif
#ifdef FEISHU_COLLAB_AUTOTEST
    cJSON *seed=cJSON_CreateObject();char *seed_result=calloc(1,640);bool seeded=false;
    if(seed && seed_result){
        cJSON_AddNumberToObject(seed,"chat_index",0);cJSON_AddStringToObject(seed,"text","明天下午开会。");
        seeded=assistant_tool(&state,"self.feishu.prepare_message",seed,seed_result,640);
    }
    cJSON_Delete(seed);free(seed_result);
    state.diagnostic_draft=seeded;state.composition=true;state.changed=false;state.tools_mask=0;
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    s_state.nav.page=FEISHU_PAGE_REVIEW;s_state.nav.chat_index=0;s_state.assistant_composing=true;s_state.detail_loaded=true;
    feishu_mentions_display(s_state.reply,s_state.detail_text,sizeof(s_state.detail_text));
    xSemaphoreGive(s_state_lock);hooks.status.page="draft_editor";
    ESP_LOGI(TAG,"ASSISTANT_COLLAB_SEED pass=%u ram_only=1 sends=0",(unsigned)seeded);
#endif
#ifdef FEISHU_ASSISTANT_AUTOTEST
    hooks.volume=0;
    esp_err_t err=feishu_assistant_autotest_run(&s_state.stop_recording,&s_state.cancel_recording,
#else
    esp_err_t err=feishu_assistant_run(&s_state.stop_recording,&s_state.cancel_recording,
#endif
        &s_state.elapsed_ms,&s_state.record_pending,&hooks
#ifdef FEISHU_ASSISTANT_AUTOTEST
        ,s_assistant_test_case
#endif
        );
    s_session.defer_list_bodies=false;
#ifdef FEISHU_DRAFT_RAM_AUTOTEST
#ifdef FEISHU_COLLAB_AUTOTEST
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    bool cloud_edit_pass=seeded && (state.tools_mask&32) && s_state.pending_slot==RAM_REPLY_SLOT &&
        strcmp(s_state.reply,"明天下午开会。") && !strcmp(s_state.pending.chat,s_state.chats[0].chat_id);
    xSemaphoreGive(s_state_lock);
    ESP_LOGI(TAG,"ASSISTANT_CLOUD_EDIT pass=%u target_fixed=1 sends=0",(unsigned)cloud_edit_pass);
#endif
    bool compose_pass=assistant_compose_native_test(&state);
#endif
#if defined(FEISHU_GLOBAL_AUTOTEST) && !defined(FEISHU_ASSISTANT_TIMEOUT_AUTOTEST)
    esp_err_t success_sound=feishu_asr_feedback(FEISHU_SOUND_SUCCESS,false,false);
    esp_err_t failure_sound=feishu_asr_feedback(FEISHU_SOUND_FAILURE,false,false);
    bool sound_pass=state.global_key_sound && success_sound==ESP_OK && failure_sound==ESP_OK;
    ESP_LOGI(TAG,"ASSISTANT_FEEDBACK_TEST live_key=%u success=%d failure=%d pass=%u",(unsigned)state.global_key_sound,success_sound,failure_sound,(unsigned)sound_pass);
#endif
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    bool cancelled=s_state.cancel_recording || s_state.closing || state.epoch!=s_owner_epoch;
    bool memory_recover=err==ESP_ERR_NO_MEM && feishu_assistant_memory_recovery(s_state.assistant_enabled,cancelled,s_state.closing,s_state.assistant_memory_retried);
    if(memory_recover){s_state.assistant_memory_retried=true;s_state.loading=false;state.handoff=true;}
    s_state.assistant_phase=state.handoff ? FEISHU_ASSISTANT_HANDOFF:FEISHU_ASSISTANT_OFF;
    if(memory_recover)ESP_LOGW(TAG,"ASSISTANT_MEMORY_RECOVERY scheduled=1 retries=1");
    s_state.assistant_active=s_state.record_pending=false;
    s_state.assistant_composing=s_state.compose_busy=false;
#if !defined(FEISHU_ASSISTANT_AUTOTEST) || defined(FEISHU_GLOBAL_AUTOTEST)
    if(!state.handoff)s_state.assistant_enabled=false;
#endif
    s_state.status_error=err!=ESP_OK && !cancelled && !memory_recover;
    if(memory_recover){feishu_utf8_copy(s_state.status,sizeof(s_state.status),"资源忙，正在恢复小智");}
    else if(cancelled) {
        if(s_state.assistant_exit_requested){
            /* Keep a business page selected while cancellation was pending.
             * The legacy dedicated view returns to the conversation list. */
            if(s_state.nav.page==FEISHU_PAGE_ASSISTANT)s_state.nav.page=FEISHU_PAGE_CHATS;
            feishu_utf8_copy(s_state.status,sizeof(s_state.status),s_state.assistant_timed_out ? "小智已超时退出":s_state.compose_send_confirm ?
                "请核对目标和正文，再次确定发送":"小智助手已关闭");
        } else feishu_utf8_copy(s_state.status,sizeof(s_state.status),"已取消小智助手");
    }
    else if(err==FEISHU_ERR_XIAO_PAIRING) {
        s_state.nav.page=FEISHU_PAGE_XIAOZHI;s_state.nav.settings_index=0;
        s_state.pairing_busy=true;s_state.cancel_pairing=false;s_state.pairing_return_page=FEISHU_PAGE_XIAOZHI_SETTINGS;++s_state.pairing_request;
    } else if(state.changed) {
        s_state.nav.page=state.target;s_state.nav.chat_index=state.chat;s_state.nav.reply_to_message=false;
        s_state.nav.draft_manager=false;s_state.nav.settings_index=0;
        if(state.target==FEISHU_PAGE_MESSAGES){s_state.loading=true;++s_state.message_page_request;}
        if(state.target==FEISHU_PAGE_DRAFTS){s_state.loading=true;s_state.nav.draft_manager=true;}
    }
    if(!cancelled && !state.composition && !state.handoff)feishu_utf8_copy(s_state.status,sizeof(s_state.status),err==ESP_OK ?
        (state.draft ? (s_state.pending_slot==RAM_REPLY_SLOT ? "仅在内存，断电丢失；可确认发送":"请核对目标和正文"):"本轮助手已结束"):
        err==FEISHU_ERR_AUDIO_BACKPRESSURE ? "语音缓冲繁忙，请重新激活":"助手连接或处理失败，可重试或长按返回");
    if(!cancelled && state.composition && !state.handoff && err!=FEISHU_ERR_XIAO_PAIRING && !state.changed)
        feishu_utf8_copy(s_state.status,sizeof(s_state.status),err==ESP_OK ?
            "协作已结束，草稿保留":"协作连接失败，草稿保留，可重试");
    const char *exit_reason=s_state.assistant_timed_out ? "deadline":state.handoff ? "handoff":cancelled ? "cancel":err==ESP_OK ? "farewell":"error";
    ESP_LOGI(TAG,"ASSISTANT_EXIT reason=%s result=%d page=%u",exit_reason,err,(unsigned)s_state.nav.page);
#ifdef FEISHU_ASSISTANT_TIMEOUT_AUTOTEST
    bool timeout_pass=s_state.assistant_timed_out && cancelled && !s_state.assistant_active && !s_state.assistant_enabled;
    ESP_LOGI(TAG,"ASSISTANT_TIMEOUT_TEST pass=%u cancelled=%u mic_owner=0",(unsigned)timeout_pass,(unsigned)cancelled);
#endif
    free(state.undo);state.undo=NULL;
    xSemaphoreGive(s_state_lock);
    if(!cancelled && err==FEISHU_ERR_XIAO_PAIRING)enqueue(WORK_CHECK_XIAOZHI);
    else if(!cancelled && state.changed && state.target==FEISHU_PAGE_MESSAGES)enqueue(WORK_LOAD_MESSAGES);
    else if(!cancelled && state.changed && state.target==FEISHU_PAGE_DRAFTS)enqueue(WORK_LIST_DRAFTS);
    else if(!cancelled && state.changed && state.target==FEISHU_PAGE_REVIEW)enqueue(WORK_LOAD_DRAFT_PAGE);
#ifdef FEISHU_ASSISTANT_AUTOTEST
#ifdef FEISHU_DRAFT_RAM_AUTOTEST
#ifdef FEISHU_COLLAB_AUTOTEST
    unsigned expected_mask=32;
#else
    unsigned expected_mask=16;
#endif
#else
    unsigned expected_mask=s_assistant_test_case==0 ? 1:s_assistant_test_case==1 ? 8:
        s_assistant_test_case==2 ? 16:s_assistant_test_case==6 ? 4:2;
#endif
    bool passed=s_assistant_test_case==3 ? err==FEISHU_HTTP_ERR_CANCELLED:
        s_assistant_test_case==4 ? err==ESP_FAIL:err==ESP_OK && (state.tools_mask&expected_mask)==expected_mask;
#ifdef FEISHU_DRAFT_RAM_AUTOTEST
    bool cleanup=assistant_test_cleanup(state.draft || state.diagnostic_draft);
#else
    bool cleanup=assistant_test_cleanup(state.draft);
#endif
    passed=passed && cleanup;
#ifdef FEISHU_SCENARIO_AUTOTEST
    passed=passed && s_scenario_done && s_scenario_pass;
#endif
#ifdef FEISHU_DRAFT_RAM_AUTOTEST
    passed=passed && target_guard_pass && compose_pass;
#ifdef FEISHU_COLLAB_AUTOTEST
    passed=passed && cloud_edit_pass;
#endif
#endif
#if defined(FEISHU_GLOBAL_AUTOTEST) && !defined(FEISHU_ASSISTANT_TIMEOUT_AUTOTEST)
    passed=passed && state.global_injected && state.global_job && sound_pass;
#endif
#ifdef FEISHU_ASSISTANT_TIMEOUT_AUTOTEST
    passed=timeout_pass && cleanup;
#endif
    if(passed)++s_assistant_tests_passed;
    ESP_LOGI(TAG,"ASSISTANT_SMOKE_DONE case=%u pass=%u tools_mask=%u cleanup=%u silent=1 sends=0",
        s_assistant_test_case,(unsigned)passed,state.tools_mask,(unsigned)cleanup);
#endif
}
#ifdef FEISHU_ASSISTANT_AUTOTEST
#ifndef FEISHU_ASSISTANT_TEST_FIRST
#define FEISHU_ASSISTANT_TEST_FIRST 0
#endif
static void assistant_suite(void)
{
    /* Run deferred open-chat last so the ordinary worker/cache queue can apply
     * its message page after the suite returns, rather than cancelling it by
     * immediately switching to the continuous assistant case. */
    for(unsigned step=FEISHU_ASSISTANT_TEST_FIRST;step<8;++step) {
        s_assistant_test_case=FEISHU_ASSISTANT_TEST_FIRST==7 ? 7:step==6 ? 7:step==7 ? 6:step;
        xSemaphoreTake(s_state_lock,portMAX_DELAY);
#ifdef FEISHU_GLOBAL_AUTOTEST
        s_state.nav.page=FEISHU_PAGE_CHATS;s_state.assistant_requested=s_state.assistant_enabled=true;
#else
        s_state.nav.page=FEISHU_PAGE_ASSISTANT;
#endif
        s_state.assistant_active=true;s_state.record_pending=true;s_state.assistant_timed_out=false;
        s_state.assistant_activity_ms=(uint32_t)(esp_timer_get_time()/1000);
        s_state.stop_recording=s_state.cancel_recording=false;s_state.elapsed_ms=0;
        s_state.assistant_heard[0]=s_state.assistant_reply[0]=s_state.status[0]=0;
        xSemaphoreGive(s_state_lock);
        ESP_LOGI(TAG,"ASSISTANT_CASE_BEGIN case=%u heap=%u",s_assistant_test_case,(unsigned)esp_get_free_heap_size());
        assistant_worker();vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGI(TAG,"ASSISTANT_SUITE_DONE pass=%u cases=%u silent=1 sends=0",(unsigned)(s_assistant_tests_passed==8-FEISHU_ASSISTANT_TEST_FIRST),s_assistant_tests_passed);
}
#endif

#include "esp_http_client.h"
#if __has_include("../build/assistant_group_fixture.h")
#include "../build/assistant_group_fixture.h"
#endif
/* Diagnostic only: exercise persisted settings and send queue, never POST. */
static bool __attribute__((noinline)) assistant_controls_native_test(assistant_ui_t *a)
{
    struct controls_work {char text[640],message[64],revision[18];} *w=calloc(1,sizeof(*w));
    cJSON *args=cJSON_CreateObject();
    if(!w || !args){free(w);cJSON_Delete(args);return false;}
    esp_http_client_config_t transport_test={.url="https://open.feishu.cn/",.method=HTTP_METHOD_POST,.disable_auto_redirect=true};
    esp_http_client_handle_t http=esp_http_client_init(&transport_test);
    bool post_safe=http && esp_http_client_defer_get_tx_buffer(http)==ESP_ERR_INVALID_ARG;
    if(http)esp_http_client_cleanup(http);
    transport_test.method=HTTP_METHOD_GET;transport_test.disable_auto_redirect=false;
    http=esp_http_client_init(&transport_test);
    bool redirect_safe=http && esp_http_client_defer_get_tx_buffer(http)==ESP_ERR_INVALID_ARG;
    if(http)esp_http_client_cleanup(http);
    transport_test.disable_auto_redirect=true;http=esp_http_client_init(&transport_test);
    bool get_safe=http && esp_http_client_defer_get_tx_buffer(http)==ESP_OK && esp_http_client_defer_get_tx_buffer(http)==ESP_OK;
    if(http)esp_http_client_cleanup(http);
    bool http_scope=post_safe && redirect_safe && get_safe;
    ESP_LOGI(TAG,"ASSISTANT_HTTP_TX_SCOPE post_rejected=%u redirect_rejected=%u get_opt_in=%u sends=0 pass=%u",
        (unsigned)post_safe,(unsigned)redirect_safe,(unsigned)get_safe,(unsigned)http_scope);
    feishu_assistant_settings_t disk,ram,check;
    feishu_sound_settings_t disk_sounds,ram_sounds,check_sounds;
    bool loaded=feishu_store_load_assistant(&disk)==ESP_OK && feishu_store_load_sounds(&disk_sounds)==ESP_OK;
    if(!loaded){free(w);cJSON_Delete(args);return false;}
    xSemaphoreTake(s_state_lock,portMAX_DELAY);ram=s_state.assistant_settings;ram_sounds=s_state.sounds;xSemaphoreGive(s_state_lock);
    const char *keys[]={"boot","silence_seconds","volume","timeout_minutes","success_sound","failure_sound","key_sound"};
    const int values[]={!ram.boot,5,0,60,!ram_sounds.success,!ram_sounds.failure,!ram_sounds.key};
    bool settings=true;
    for(unsigned i=0;i<7 && settings;++i){
        cJSON_Delete(args);args=cJSON_CreateObject();if(!args){settings=false;break;}
        cJSON_AddStringToObject(args,"action","set");cJSON_AddStringToObject(args,"key",keys[i]);
        if(i==0 || i>3)cJSON_AddBoolToObject(args,"value",values[i]);else cJSON_AddNumberToObject(args,"value",values[i]);
        settings=assistant_settings_tool(a,args,w->text,sizeof(w->text)) && feishu_store_load_assistant(&check)==ESP_OK && feishu_store_load_sounds(&check_sounds)==ESP_OK;
        if(settings)switch(i){case 0:settings=check.boot==!!values[i];break;case 1:settings=check.silence_seconds==5;break;
            case 2:settings=check.volume==0;break;case 3:settings=check.timeout_minutes==60;break;
            case 4:settings=check_sounds.success==!!values[i];break;case 5:settings=check_sounds.failure==!!values[i];break;
            default:settings=check_sounds.key==!!values[i];break;}
    }
    /* Relative volume is persisted but diagnostic playback remains forced zero. */
    cJSON_Delete(args);args=cJSON_CreateObject();
    if(args){cJSON_AddStringToObject(args,"action","set");cJSON_AddStringToObject(args,"key","volume");cJSON_AddNumberToObject(args,"delta",10);
        settings=settings && assistant_settings_tool(a,args,w->text,sizeof(w->text)) && feishu_store_load_assistant(&check)==ESP_OK && check.volume==10;}
    else settings=false;
    bool restored=feishu_store_save_assistant(&disk)==ESP_OK && feishu_store_save_sounds(&disk_sounds)==ESP_OK;
    xSemaphoreTake(s_state_lock,portMAX_DELAY);s_state.assistant_settings=ram;s_state.sounds=ram_sounds;
    atomic_store(&s_assistant_submit_seconds,ram.silence_seconds ? ram.silence_seconds:2);xSemaphoreGive(s_state_lock);
    bool navigation=restored;
    const char *pages[]={"xiaozhi_settings","assistant_settings","sounds","account"};
    const feishu_page_t expected[]={FEISHU_PAGE_XIAOZHI_SETTINGS,FEISHU_PAGE_ASSISTANT_SETTINGS,FEISHU_PAGE_SOUND_SETTINGS,FEISHU_PAGE_ACCOUNT};
    for(unsigned i=0;i<4 && navigation;++i){
        cJSON_Delete(args);args=cJSON_CreateObject();if(!args){navigation=false;break;}
        cJSON_AddStringToObject(args,"page",pages[i]);navigation=assistant_tool(a,"self.feishu.navigate",args,w->text,sizeof(w->text));
        xSemaphoreTake(s_state_lock,portMAX_DELAY);navigation=navigation && s_state.nav.page==expected[i];xSemaphoreGive(s_state_lock);
    }
    cJSON_Delete(args);args=cJSON_CreateObject();if(args)cJSON_AddNumberToObject(args,"chat_index",0);
    bool reply=args && assistant_tool(a,"self.feishu.open_chat",args,w->text,sizeof(w->text));
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    if(s_state.nav.message_count)feishu_utf8_copy(w->message,sizeof(w->message),s_state.messages[0].message_id);
    xSemaphoreGive(s_state_lock);
    uint32_t previous_activity;
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    s_state.nav.page=FEISHU_PAGE_MESSAGE_DETAIL;s_state.nav.message_index=0;s_state.loading=true;
    previous_activity=s_state.assistant_activity_ms;s_state.assistant_activity_ms=(uint32_t)(esp_timer_get_time()/1000)-600001u;
    xSemaphoreGive(s_state_lock);
    assistant_event(a,FEISHU_ASSISTANT_HEARD,"Reply to this message");
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    bool activity=!feishu_assistant_expired((uint32_t)(esp_timer_get_time()/1000),s_state.assistant_activity_ms,10,true);
    s_state.assistant_activity_ms=previous_activity;xSemaphoreGive(s_state_lock);
    cJSON *context_args=cJSON_CreateObject(),*context_reply=NULL;
    bool loading_context=context_args && assistant_tool(a,"self.feishu.get_context",context_args,w->text,sizeof(w->text));
    if(loading_context)context_reply=cJSON_Parse(w->text);
    const cJSON *selected=cJSON_GetObjectItemCaseSensitive(context_reply,"message_id");
    loading_context=loading_context && cJSON_IsString(selected) && !strcmp(selected->valuestring,w->message);
    cJSON_Delete(context_reply);cJSON_Delete(context_args);
    if(args)cJSON_AddStringToObject(args,"reply_to_message_id",w->message);
    reply=reply && w->message[0] && assistant_tool(a,"self.feishu.open_composer",args,w->text,sizeof(w->text));
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    reply=reply && s_state.nav.reply_to_message && !strcmp(s_state.pending.message,w->message);
    xSemaphoreGive(s_state_lock);
    if(args)cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(args,"reply_to_message_id"),"om_not_in_loaded_window");
    bool wrong=reply && !assistant_tool(a,"self.feishu.open_composer",args,w->text,sizeof(w->text));
    cJSON_Delete(args);args=cJSON_CreateObject();cJSON *out=NULL;
    bool read=args && assistant_compose_tool(a,"self.feishu.read_draft",args,w->text,sizeof(w->text));
    if(read)out=cJSON_Parse(w->text);
    const cJSON *rev=cJSON_GetObjectItemCaseSensitive(out,"revision");
    if(cJSON_IsString(rev))feishu_utf8_copy(w->revision,sizeof(w->revision),rev->valuestring);else read=false;
    cJSON_Delete(out);out=NULL;
    if(args){cJSON_AddStringToObject(args,"revision",w->revision);cJSON_AddStringToObject(args,"text","Voice confirmation and true reply diagnostic; never transmitted.");}
    bool edited=read && assistant_compose_tool(a,"self.feishu.edit_draft",args,w->text,sizeof(w->text));
    if(edited)out=cJSON_Parse(w->text);
    rev=cJSON_GetObjectItemCaseSensitive(out,"revision");
    if(cJSON_IsString(rev))feishu_utf8_copy(w->revision,sizeof(w->revision),rev->valuestring);else edited=false;
    cJSON_Delete(out);
    if(args){cJSON_DeleteItemFromObjectCaseSensitive(args,"text");cJSON_ReplaceItemInObjectCaseSensitive(args,"revision",cJSON_CreateString(w->revision));}
    bool preview=edited && assistant_compose_tool(a,"self.feishu.request_send",args,w->text,sizeof(w->text));
    if(args)cJSON_AddBoolToObject(args,"confirmed",true);
    bool same=preview && !assistant_compose_tool(a,"self.feishu.request_send",args,w->text,sizeof(w->text));
    ++a->turn;
    if(args)cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(args,"revision"),"d0000000000000000");
    bool stale=same && !assistant_compose_tool(a,"self.feishu.request_send",args,w->text,sizeof(w->text));
    if(args)cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(args,"revision"),w->revision);
    /* Remove only our cosmetic/background work before testing the exact send. */
    work_command_t command;
    while(xQueuePeek(s_work_queue,&command,0)==pdTRUE && (command==WORK_FEEDBACK || command==WORK_AUDIO_FOCUS || command==WORK_REFRESH_MESSAGES)){
        (void)xQueueReceive(s_work_queue,&command,0);
        if(command==WORK_FEEDBACK)atomic_store(&s_feedback_queued,false);
        if(command==WORK_AUDIO_FOCUS)atomic_store(&s_audio_focus_queued,false);
    }
    unsigned filled=0;work_command_t filler=WORK_FEEDBACK;
    while(filled<32 && xQueueSend(s_work_queue,&filler,0)==pdTRUE)++filled;
    bool full=stale && filled>0 && !assistant_compose_tool(a,"self.feishu.request_send",args,w->text,sizeof(w->text));
    xSemaphoreTake(s_state_lock,portMAX_DELAY);full=full && !s_state.sending && s_state.compose_send_confirm;xSemaphoreGive(s_state_lock);
    for(unsigned i=0;i<filled;++i){if(xQueueReceive(s_work_queue,&command,0)!=pdTRUE || command!=WORK_FEEDBACK)full=false;}
    ESP_LOGI(TAG,"ASSISTANT_SEND_QUEUE_FULL pass=%u",(unsigned)full);
    bool queued=full && assistant_compose_tool(a,"self.feishu.request_send",args,w->text,sizeof(w->text));
    bool duplicate=queued && !assistant_compose_tool(a,"self.feishu.request_send",args,w->text,sizeof(w->text));
    bool exact=queued && xQueueReceive(s_work_queue,&command,0)==pdTRUE && command==WORK_SEND;
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    reply=reply && wrong && !strcmp(s_state.pending.message,w->message);
    s_state.sending=s_state.loading=s_state.compose_send_confirm=s_state.stop_recording=false;
    xSemaphoreGive(s_state_lock);a->handoff=false;a->tool_failed=false;
    ESP_LOGI(TAG,"ASSISTANT_DETAIL_REPLY loading_context=%u reply=%u activity=%u",(unsigned)loading_context,(unsigned)reply,(unsigned)activity);
    bool pass=http_scope && loading_context && activity && settings && restored && navigation && reply && preview && same && stale && queued && duplicate && exact;
    ESP_LOGI(TAG,"ASSISTANT_CONTROLS_TEST settings=%u navigation=%u reply=%u same_turn=%u stale=%u duplicate=%u queue=%u sends=0 pass=%u",
        (unsigned)settings,(unsigned)navigation,(unsigned)reply,(unsigned)same,(unsigned)stale,(unsigned)duplicate,(unsigned)exact,(unsigned)pass);
    cJSON_Delete(args);free(w);return pass;
}

#include "feishu_flash_port.h"
#include "feishu_root_flashdb.h"
static bool assistant_storage_idle_native_test(void)
{
    feishu_flash_scheduler_probe_t before=feishu_flash_scheduler_probe();bool scanned=true;
    esp_err_t locked=feishu_cache_pool_lock();
    if(locked==ESP_OK){
        scanned=feishu_fdb_probe_cold()==FEISHU_FDB_OK;
        feishu_root_flashdb_probe_cold();feishu_cache_pool_unlock();
    }
    feishu_credentials_t *credentials=calloc(1,sizeof(*credentials));
    int64_t began=esp_timer_get_time();
    esp_err_t loaded=credentials ? feishu_store_load_credentials(credentials):ESP_ERR_NO_MEM;
    unsigned elapsed=(unsigned)((esp_timer_get_time()-began)/1000);
    if(credentials){memset(credentials,0,sizeof(*credentials));free(credentials);}
    feishu_flash_scheduler_probe_t after=feishu_flash_scheduler_probe();
    bool pass=locked==ESP_OK && scanned && loaded==ESP_OK && elapsed<3000 && after.idles>before.idles && after.yields>before.yields;
    ESP_LOGI(TAG,"ASSISTANT_STORAGE_IDLE pass=%u root_ms=%u idle_delta=%u yields_delta=%u calls_delta=%u",(unsigned)pass,elapsed,
        (unsigned)(after.idles-before.idles),(unsigned)(after.yields-before.yields),(unsigned)(after.calls-before.calls));
    return pass;
}

/* Real FlashDB paging, executed with live standby capture after the mixed suite. */
static bool __attribute__((noinline)) assistant_detail_native_test(assistant_ui_t *a)
{
    feishu_message_t *next=calloc(1,sizeof(*next));if(!next)return false;
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    size_t chat=s_state.nav.chat_index;
    if(chat>=s_state.nav.chat_count)chat=0;
    size_t groups=s_state.nav.chat_count,selected=SIZE_MAX;
    if(chat<groups && !strcmp(s_state.loaded_chat,s_state.chats[chat].chat_id))
        for(size_t i=0;i<s_state.nav.message_count;++i)
            if(s_state.messages[i].type==FEISHU_MESSAGE_TEXT &&
               (selected==SIZE_MAX || s_state.messages[i].text_length>s_state.messages[selected].text_length))selected=i;
    if(selected!=SIZE_MAX)*next=s_state.messages[selected];
    xSemaphoreGive(s_state_lock);
    /* A random pressure scene can end on an image-only window. Locate a real
     * text message before deliberately replacing the RAM access token. */
    for(size_t group=0;selected==SIZE_MAX && group<groups;++group){
        xSemaphoreTake(s_state_lock,portMAX_DELAY);
        s_state.nav.page=FEISHU_PAGE_MESSAGES;s_state.nav.chat_index=group;s_state.loading=true;
        s_state.message_page_number=0;++s_state.message_page_request;xSemaphoreGive(s_state_lock);
        if(load_messages_for_chat(group,false,false)!=ESP_OK)continue;
        xSemaphoreTake(s_state_lock,portMAX_DELAY);
        for(size_t i=0;i<s_state.nav.message_count;++i)if(s_state.messages[i].type==FEISHU_MESSAGE_TEXT &&
            (selected==SIZE_MAX || s_state.messages[i].text_length>s_state.messages[selected].text_length))selected=i;
        if(selected!=SIZE_MAX){*next=s_state.messages[selected];chat=group;}
        xSemaphoreGive(s_state_lock);
    }
    if(selected==SIZE_MAX){ESP_LOGW(TAG,"ASSISTANT_DETAIL_SETUP no_text=1");free(next);return false;}
    ESP_LOGI(TAG,"ASSISTANT_DETAIL_SETUP text=1 chat=%u",(unsigned)chat);
    char message_id[FEISHU_MESSAGE_ID_MAX];feishu_utf8_copy(message_id,sizeof(message_id),next->message_id);
    /* RAM-only invalid access token forces the real 401 -> refresh path.
     * Persist only the server's newly issued credentials through normal APIs. */
    strcpy(s_session.user_access_token,"diagnostic-expired-access-token");
    int64_t began=esp_timer_get_time();
    esp_err_t fetch_error=next->type==FEISHU_MESSAGE_TEXT ? feishu_api_get_message(&s_session,message_id,next):ESP_ERR_INVALID_STATE;
#ifdef FEISHU_NATIVE_FOCUSED_CASES
    if(fetch_error==FEISHU_ERR_AUTH_REVOKED){
        free(next);
        if(feishu_store_request_auth(FEISHU_AUTH_REAUTHORIZE)==ESP_OK){
            ESP_LOGW(TAG,"ASSISTANT_AUTH_REQUIRED invalid_grant=1 reboot=1");
            vTaskDelay(pdMS_TO_TICKS(100));esp_restart();
        }
        return false;
    }
#endif
    bool fetched=fetch_error==ESP_OK && next->text_object_id>0 && !next->text_unavailable;
    unsigned fetch_ms=(unsigned)((esp_timer_get_time()-began)/1000);
    ESP_LOGI(TAG,"ASSISTANT_AUTH_REFRESH pass=%u",(unsigned)(fetched && strncmp(s_session.user_access_token,"diagnostic-",11)));
    ESP_LOGI(TAG,"ASSISTANT_DETAIL_FETCH pass=%u ms=%u bytes=%llu",(unsigned)fetched,fetch_ms,(unsigned long long)next->text_length);
    bool pass=fetched && fetch_ms<20000;free(next);
    struct detail_work {feishu_message_t saved;feishu_data_writer_t writer;char block[FEISHU_BLOCK_BYTES];} *w=calloc(1,sizeof(*w));
    if(!w)return false;
    xSemaphoreTake(s_state_lock,portMAX_DELAY);w->saved=s_state.messages[0];xSemaphoreGive(s_state_lock);
    int64_t id=0;bool staged=feishu_cache_text_begin(&w->writer)==ESP_OK;
    for(unsigned i=0;i<12 && staged;++i){memset(w->block,'A'+i,sizeof(w->block));staged=feishu_cache_text_append(&w->writer,w->block,sizeof(w->block))==ESP_OK;}
    staged=staged && feishu_cache_text_commit(&w->writer,"om_native_paging_r54",&id)==ESP_OK;
    unsigned maximum=0,read=0;
    if(staged){
        feishu_message_t message={0};strcpy(message.message_id,"om_native_paging_r54");message.type=FEISHU_MESSAGE_TEXT;
        message.text_object_id=id;message.text_length=12u*FEISHU_BLOCK_BYTES;message.text_truncated=true;strcpy(message.text,"Native paging fixture");
        xSemaphoreTake(s_state_lock,portMAX_DELAY);s_state.messages[0]=message;s_state.nav.page=FEISHU_PAGE_MESSAGE_DETAIL;
        s_state.nav.message_index=0;s_state.nav.chat_index=chat;s_state.bookmark_dirty=false;xSemaphoreGive(s_state_lock);
        const unsigned pages[]={0,3,7,1};
        for(unsigned i=0;i<4;++i){
            xSemaphoreTake(s_state_lock,portMAX_DELAY);s_state.detail_page=pages[i];s_state.detail_restore=false;
            s_state.detail_loaded=false;s_state.loading=true;xSemaphoreGive(s_state_lock);
            int64_t start=esp_timer_get_time();load_text_page();unsigned ms=(unsigned)((esp_timer_get_time()-start)/1000);
            if(ms>maximum)maximum=ms;
            xSemaphoreTake(s_state_lock,portMAX_DELAY);bool ready=s_state.detail_loaded && !s_state.loading && s_state.detail_text[0] && s_state.detail_page==pages[i];xSemaphoreGive(s_state_lock);
            if(ready)++read;
        }
    }
    feishu_cache_text_abort(&w->writer);feishu_cache_unpin(&s_text_pin);
    xSemaphoreTake(s_state_lock,portMAX_DELAY);s_state.messages[0]=w->saved;s_state.nav.page=FEISHU_PAGE_CHATS;
    s_state.loading=false;s_state.bookmark_dirty=false;xSemaphoreGive(s_state_lock);
    free(w);w=NULL;
    char *result=calloc(1,640);cJSON *args=cJSON_CreateObject();size_t bot_chat=0;bool group_match=true;
#ifdef FEISHU_NATIVE_GROUP_CHAT
    group_match=false;
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    for(size_t i=0;i<s_state.nav.chat_count;++i)if(!strcmp(s_state.chats[i].chat_id,FEISHU_NATIVE_GROUP_CHAT)){bot_chat=i;group_match=true;break;}
    xSemaphoreGive(s_state_lock);
#endif
    if(args){cJSON_AddNumberToObject(args,"chat_index",bot_chat);cJSON_AddStringToObject(args,"name","蓝药丸1号机");}
    bool bot=group_match && result && args && assistant_tool(a,"self.feishu.find_member",args,result,640);
    ESP_LOGI(TAG,"ASSISTANT_BOT_MEMBER group_match=%u pass=%u",(unsigned)group_match,(unsigned)bot);cJSON_Delete(args);
    pass=pass && staged && read==4 && maximum<3000 && bot;
    ESP_LOGI(TAG,"ASSISTANT_DETAIL_PAGES pass=%u pages=%u max_ms=%u",(unsigned)pass,read,maximum);
    a->tool_failed=false;free(result);return pass;
}

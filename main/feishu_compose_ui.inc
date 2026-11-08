/* Worker-only composition tools. No automatic send path exists here. */
static esp_err_t assistant_format_message(const char *chat,const cJSON *args,const char *body,
                                         const char *existing,char *out,size_t capacity)
{
    feishu_mention_t *members=calloc(FEISHU_MENTION_MAX,sizeof(*members));if(!members)return ESP_ERR_NO_MEM;
    size_t count=0;esp_err_t err=ESP_OK;const char *ignored=NULL;
    const cJSON *names=cJSON_GetObjectItemCaseSensitive(args,"mentions");
    if(names){
        for(const cJSON *name=names->child;name;name=name->next){
            if(count>=FEISHU_MENTION_MAX || !cJSON_IsString(name)){err=ESP_ERR_INVALID_ARG;break;}
            const char *query=name->valuestring;if(*query=='@')++query;
            err=feishu_api_resolve_mention(&s_session,chat,query,&members[count],&s_state.cancel_recording);
            if(err!=ESP_OK)break;
            ++count;
        }
    } else if(existing && !feishu_mentions_split(existing,members,&count,&ignored))err=ESP_ERR_INVALID_RESPONSE;
    if(err==ESP_OK && !feishu_mentions_compose(body,members,count,out,capacity))err=ESP_ERR_INVALID_SIZE;
    memset(members,0,FEISHU_MENTION_MAX*sizeof(*members));free(members);return err;
}
static bool assistant_draft_snapshot(assistant_ui_t *a,feishu_compose_snapshot_t *out,
                                      feishu_outbox_t *pending,int *slot)
{
    memset(out,0,sizeof(*out));
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    bool valid=a->epoch==s_owner_epoch && !s_state.cancel_recording && !s_state.closing &&
        !s_state.sending && (s_state.nav.page==FEISHU_PAGE_REVIEW || s_state.nav.page==FEISHU_PAGE_DRAFT_ACTIONS);
    if(valid){
        *pending=s_state.pending;*slot=s_state.pending_slot;
        valid=pending->state==FEISHU_OUTBOX_READY && !pending->offset && pending->length<sizeof(out->text);
        if(valid && *slot==RAM_REPLY_SLOT)feishu_utf8_copy(out->text,sizeof(out->text),s_state.reply);
    }
    xSemaphoreGive(s_state_lock);
    if(!valid)return false;
    if(*slot!=RAM_REPLY_SLOT){
        size_t got=0;
        if(*slot<0 || feishu_cache_text_read(pending->body_name,pending->object_id,false,0,
            out->text,(size_t)pending->length,&got)!=ESP_OK || got!=pending->length)return false;
        out->text[got]=0;
    }
    feishu_utf8_copy(out->chat,sizeof(out->chat),pending->chat);
    feishu_utf8_copy(out->message,sizeof(out->message),pending->message);
    feishu_utf8_copy(out->uuid,sizeof(out->uuid),pending->uuid);
    out->length=pending->length;out->offset=pending->offset;out->state=pending->state;
    return feishu_compose_editable(out);
}
static const char *assistant_page_name(feishu_page_t page)
{
    switch(page){
    case FEISHU_PAGE_CHATS:return "chats";
    case FEISHU_PAGE_MESSAGES:return "messages";
    case FEISHU_PAGE_MESSAGE_DETAIL:return "message";
    case FEISHU_PAGE_REVIEW:case FEISHU_PAGE_DRAFT_ACTIONS:return "draft_editor";
    case FEISHU_PAGE_RECORDING:return "dictation";
    case FEISHU_PAGE_REPLY_READY:return "composer";
    case FEISHU_PAGE_DRAFTS:return "drafts";
    case FEISHU_PAGE_SETTINGS:return "settings";
    case FEISHU_PAGE_SOUND_SETTINGS:return "sound_settings";
    case FEISHU_PAGE_ABOUT:return "about";
    case FEISHU_PAGE_POWER_SETTINGS:return "power_settings";
    case FEISHU_PAGE_ASSISTANT_SETTINGS:return "assistant_settings";
    case FEISHU_PAGE_XIAOZHI_SETTINGS:return "xiaozhi_settings";
    case FEISHU_PAGE_XIAOZHI:return "xiaozhi_pairing";
    case FEISHU_PAGE_ACCOUNT:return "feishu_account";
    case FEISHU_PAGE_NETWORK:return "network";
    case FEISHU_PAGE_NETWORK_SCAN:return "network_scan";
    case FEISHU_PAGE_NETWORK_ACTIONS:return "network_actions";
    case FEISHU_PAGE_NETWORK_PASSWORD:return "network_password";
    case FEISHU_PAGE_IMAGE:case FEISHU_PAGE_IMAGE_ACTIONS:return "image";
    case FEISHU_PAGE_CONFIRM:return "confirmation";
    case FEISHU_PAGE_TRANSCRIBING:return "transcribing";
    default:return "other";
    }
}
static bool assistant_compose_tool(assistant_ui_t *a,const char *name,const cJSON *args,char *text,size_t capacity)
{
    struct work {
        feishu_compose_snapshot_t before,after;
        feishu_outbox_t pending,next;
        feishu_data_writer_t writer;
        char revision[FEISHU_COMPOSE_REV_CAP],uuid[41],key[64],part[129],formatted[512];
        feishu_mention_t mentions[FEISHU_MENTION_MAX];
    } *w=calloc(1,sizeof(*w));
    if(!w){snprintf(text,capacity,"Not enough memory; draft unchanged.");return false;}
    cJSON *result=cJSON_CreateObject();bool ok=false,undo=!strcmp(name,"self.feishu.undo_draft");
    int slot=-1;bool editable=assistant_draft_snapshot(a,&w->before,&w->pending,&slot);
    if(!result)goto done;
    if(!strcmp(name,"self.feishu.get_context")){
        xSemaphoreTake(s_state_lock,portMAX_DELAY);
        cJSON_AddStringToObject(result,"page",assistant_page_name(s_state.nav.page));
        cJSON_AddBoolToObject(result,"assistant_active",s_state.assistant_active);
        cJSON_AddBoolToObject(result,"sending",s_state.sending);
        cJSON_AddNumberToObject(result,"draft_state",s_state.pending.state);
        cJSON_AddNumberToObject(result,"draft_bytes",(double)s_state.pending.length);
        bool draft=s_state.nav.page==FEISHU_PAGE_REVIEW || s_state.nav.page==FEISHU_PAGE_DRAFT_ACTIONS;
        size_t i=s_state.nav.chat_index;
        if(draft)cJSON_AddStringToObject(result,"chat_id",s_state.pending.chat);
        else if(i<s_state.nav.chat_count)cJSON_AddStringToObject(result,"chat_id",s_state.chats[i].chat_id);
        if(!draft && i<s_state.nav.chat_count && !strcmp(s_state.loaded_chat,s_state.chats[i].chat_id) &&
           (s_state.nav.page==FEISHU_PAGE_MESSAGE_DETAIL ||
            (s_state.nav.page==FEISHU_PAGE_MESSAGES && !s_state.nav.composer_selected)) &&
            s_state.nav.message_index<s_state.nav.message_count){
            const feishu_message_t *m=&s_state.messages[s_state.nav.message_index];
            cJSON_AddStringToObject(result,"message_id",m->message_id);
            feishu_utf8_copy(w->part,65,m->text);
            cJSON_AddStringToObject(result,"message_preview",w->part);
            /* The model must explicitly fetch message content, not assume a
             * selected list row is the entire message. */
            cJSON_AddBoolToObject(result,"message_preview_only",true);
        }
        xSemaphoreGive(s_state_lock);
        cJSON_AddBoolToObject(result,"draft_editable",editable);
        cJSON_AddBoolToObject(result,"physical_send_confirmation_available",true);
        cJSON_AddBoolToObject(result,"voice_send_confirmation",true);ok=true;
    } else if(!editable){
        snprintf(text,capacity,"No complete editable draft in view. Open its editor first. Sent/uncertain, partially sent, missing or over-511-byte drafts cannot be replaced from a preview.");goto done;
    } else if(!strcmp(name,"self.feishu.read_draft")){
        const cJSON *offset=cJSON_GetObjectItemCaseSensitive(args,"offset");
        const char *body=NULL;size_t mention_count=0;
        if(!feishu_mentions_split(w->before.text,w->mentions,&mention_count,&body))goto done;
        size_t body_length=strlen(body);
        size_t start=cJSON_IsNumber(offset) ? (size_t)offset->valueint:0;
        if(start>body_length || (start<body_length && ((unsigned char)body[start]&0xc0)==0x80)){
            snprintf(text,capacity,"Invalid offset; use next_offset from the preceding page.");goto done;
        }
        size_t end=start+128;if(end>body_length)end=body_length;
        while(end>start && ((unsigned char)body[end]&0xc0)==0x80)--end;
        memcpy(w->part,body+start,end-start);w->part[end-start]=0;
        feishu_compose_revision(&w->before,w->revision);
        cJSON_AddStringToObject(result,"revision",w->revision);
        cJSON_AddStringToObject(result,"chat_id",w->before.chat);
        cJSON_AddStringToObject(result,"reply_to_message_id",w->before.message);
        cJSON_AddStringToObject(result,"text",w->part);
        cJSON_AddNumberToObject(result,"next_offset",end);
        cJSON_AddBoolToObject(result,"more",end<body_length);
        cJSON_AddNumberToObject(result,"mention_count",mention_count);
        cJSON_AddBoolToObject(result,"undo_available",a->undo!=NULL);ok=true;
    } else if(!strcmp(name,"self.feishu.request_send")){
        char actual[FEISHU_COMPOSE_REV_CAP];
        const char *revision=cJSON_GetObjectItemCaseSensitive(args,"revision")->valuestring;
        if(!w->before.length || !feishu_compose_revision(&w->before,actual) || strcmp(actual,revision)){
            snprintf(text,capacity,"Empty or stale draft. Read current draft and finish editing before requesting confirmation. NOT SENT.");goto done;
        }
        bool confirmed=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(args,"confirmed"));
        xSemaphoreTake(s_state_lock,portMAX_DELAY);
        bool valid=a->epoch==s_owner_epoch && !s_state.cancel_recording && !s_state.closing && !s_state.sending &&
            s_state.nav.page==FEISHU_PAGE_REVIEW && !memcmp(s_state.pending.uuid,w->pending.uuid,sizeof(w->pending.uuid));
        if(confirmed){
            /* A tool cannot confirm its own preview in the same spoken turn.
             * Revision/UUID bind this confirmation to the visible recipient/body. */
            valid=valid && s_state.compose_send_confirm && a->turn!=a->confirmation_turn;
            work_command_t send=WORK_SEND;
            valid=valid && xQueueSend(s_work_queue,&send,0)==pdTRUE;
            if(valid){s_state.compose_send_confirm=false;s_state.sending=s_state.loading=true;s_state.cancel_send=false;
                s_state.reply_from_assistant=true;a->handoff=true;s_state.stop_recording=true;
                feishu_utf8_copy(s_state.status,sizeof(s_state.status),"正在发送消息…");}
        } else if(valid){
            if(!s_state.compose_send_confirm)a->confirmation_turn=a->turn;
            s_state.compose_send_confirm=true;s_state.stop_recording=true;s_state.status_error=false;
            feishu_utf8_copy(s_state.status,sizeof(s_state.status),"请核对目标和正文，说确认发送或按确定");
        }
        xSemaphoreGive(s_state_lock);
        if(!valid){snprintf(text,capacity,"Send not queued: stale editor, missing separate user confirmation, already sending, or queue busy. NOT SENT.");goto done;}
        cJSON_AddBoolToObject(result,"sent",false);cJSON_AddBoolToObject(result,"queued",confirmed);
        cJSON_AddBoolToObject(result,"physical_confirmation_required",false);
        cJSON_AddStringToObject(result,"result",confirmed ? "Send queued for the existing worker. NOT yet confirmed sent; report only sending until the actual result.":
            "Preview ready. Ask the USER to confirm sending in their NEXT spoken turn, or press physical OK. NOT SENT.");ok=true;
    } else {
        const char *revision=cJSON_GetObjectItemCaseSensitive(args,"revision")->valuestring;
        const cJSON *content=cJSON_GetObjectItemCaseSensitive(args,"text");
        if(!undo && assistant_format_message(w->before.chat,args,content->valuestring,w->before.text,
            w->formatted,sizeof(w->formatted))!=ESP_OK){
            snprintf(text,capacity,"Cannot form verified mentions or message exceeds complete draft limit. Use mentions names separately; no literal @Name or raw tags. Draft unchanged.");goto done;
        }
        bool accepted=undo ? feishu_compose_undo(&w->before,revision,a->undo,&w->after):
            feishu_compose_replace(&w->before,revision,w->formatted,&w->after);
        if(!accepted){snprintf(text,capacity,"Stale revision or undo unavailable. Read current draft again. Nothing changed.");goto done;}
        xSemaphoreTake(s_state_lock,portMAX_DELAY);
        bool valid=a->epoch==s_owner_epoch && !s_state.cancel_recording && !s_state.closing &&
            !s_state.sending && s_state.nav.page==FEISHU_PAGE_REVIEW &&
            !memcmp(s_state.pending.uuid,w->pending.uuid,sizeof(w->pending.uuid));
        if(valid)s_state.compose_busy=true;
        xSemaphoreGive(s_state_lock);
        if(!valid){snprintf(text,capacity,"Editor changed; draft unchanged.");goto done;}
        /* Allocate optional undo BEFORE publishing. Failure disables undo,
         * never prevents an otherwise complete message from being edited. */
        feishu_compose_snapshot_t *previous=undo ? NULL:malloc(sizeof(*previous));
        if(previous)*previous=w->before;
        make_reply_uuid(w->uuid);snprintf(w->key,sizeof(w->key),"assistant_%s",w->uuid);
        esp_err_t err=feishu_cache_draft_slot(&slot);
#if defined(FEISHU_DRAFT_RAM_AUTOTEST) || defined(FEISHU_GLOBAL_AUTOTEST)
        err=ESP_FAIL; /* Exercise volatile edits without touching user Flash. */
#endif
        if(err==ESP_OK)err=feishu_cache_draft_begin(&w->writer);
        if(err==ESP_OK)err=feishu_cache_text_append(&w->writer,w->after.text,strlen(w->after.text));
        if(err==ESP_OK && !feishu_outbox_create(&w->next,w->writer.id,w->writer.raw,w->key,
            w->before.chat,w->before.message,w->uuid))err=ESP_ERR_INVALID_RESPONSE;
        if(err==ESP_OK)err=feishu_cache_pending_write((unsigned)slot,&w->next,&w->writer);
        feishu_cache_text_abort(&w->writer);
        bool persisted=err==ESP_OK;
        if(persisted)(void)feishu_cache_prune_drafts(slot,true);
        else {
            memset(&w->next,0,sizeof(w->next));w->next.version=1;w->next.state=FEISHU_OUTBOX_READY;
            w->next.length=strlen(w->after.text);
            feishu_utf8_copy(w->next.chat,sizeof(w->next.chat),w->before.chat);
            feishu_utf8_copy(w->next.message,sizeof(w->next.message),w->before.message);
            feishu_utf8_copy(w->next.uuid,sizeof(w->next.uuid),w->uuid);slot=RAM_REPLY_SLOT;
        }
        xSemaphoreTake(s_state_lock,portMAX_DELAY);
        s_state.pending=w->next;s_state.pending_slot=slot;s_state.reply_from_assistant=true;
        s_state.compose_busy=s_state.compose_send_confirm=false;
        s_state.pending_retry_warning=false;s_state.detail_page=0;s_state.detail_pages=1;
        s_state.detail_scroll=s_state.detail_scroll_max=0;s_state.detail_loaded=true;
        feishu_utf8_copy(s_state.reply,sizeof(s_state.reply),w->after.text);
        feishu_utf8_copy(s_state.reply_uuid,sizeof(s_state.reply_uuid),w->uuid);
        feishu_mentions_display(w->after.text,s_state.detail_text,sizeof(s_state.detail_text));
        feishu_utf8_copy(s_state.status,sizeof(s_state.status),persisted ? "草稿已修改，尚未发送":"仅在内存，断电丢失；尚未发送");
        s_state.status_error=false;
        xSemaphoreGive(s_state_lock);
        free(a->undo);a->undo=previous;
        feishu_utf8_copy(w->after.uuid,sizeof(w->after.uuid),w->next.uuid);
        feishu_compose_revision(&w->after,w->revision);
        cJSON_AddStringToObject(result,"revision",w->revision);
        cJSON_AddBoolToObject(result,"persisted",persisted);
        cJSON_AddBoolToObject(result,"undo_available",a->undo!=NULL);
        cJSON_AddStringToObject(result,"result",undo ? "Last edit undone. NOT SENT.":"Draft updated. NOT SENT. Keep discussing or request user send confirmation.");
        ok=true;
    }
    if(!strcmp(name,"self.feishu.get_context") && !cJSON_PrintPreallocated(result,text,(int)capacity,false))
        cJSON_DeleteItemFromObjectCaseSensitive(result,"message_preview");
    if(!cJSON_PrintPreallocated(result,text,(int)capacity,false)){snprintf(text,capacity,"Result exceeds preview limit; narrow request.");ok=false;}
done:
    cJSON_Delete(result);memset(w,0,sizeof(*w));free(w);return ok;
}

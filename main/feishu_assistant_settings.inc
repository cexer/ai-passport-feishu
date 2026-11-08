/* Existing worker only; no settings task or persistent allocation. */
static bool assistant_settings_tool(assistant_ui_t *a,const cJSON *args,char *text,size_t capacity)
{
    const cJSON *action=cJSON_GetObjectItemCaseSensitive(args,"action");
    feishu_assistant_settings_t before,next;
    feishu_sound_settings_t sounds,next_sounds;
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    bool valid=a->epoch==s_owner_epoch && !s_state.cancel_recording && !s_state.closing && !s_state.assistant_settings_editing;
    before=next=s_state.assistant_settings;sounds=next_sounds=s_state.sounds;
    xSemaphoreGive(s_state_lock);
    if(!valid){snprintf(text,capacity,"Settings changed or an edit is active; retry later.");return false;}
    bool write=!strcmp(action->valuestring,"set"),sound=false;
    if(write){
        const char *key=cJSON_GetObjectItemCaseSensitive(args,"key")->valuestring;
        const cJSON *value=cJSON_GetObjectItemCaseSensitive(args,"value"),*delta=cJSON_GetObjectItemCaseSensitive(args,"delta");
        if(!strcmp(key,"boot"))next.boot=cJSON_IsTrue(value);
        else if(!strcmp(key,"silence_seconds"))next.silence_seconds=(unsigned)value->valueint;
        else if(!strcmp(key,"timeout_minutes"))next.timeout_minutes=(unsigned)value->valueint;
        else if(!strcmp(key,"volume")){
            int v=delta ? (int)before.volume+delta->valueint:value->valueint;
            next.volume=(unsigned)(v<0 ? 0:v>100 ? 100:v);
        } else {
            sound=true;bool enabled=cJSON_IsTrue(value);
            if(!strcmp(key,"success_sound"))next_sounds.success=enabled;
            else if(!strcmp(key,"failure_sound"))next_sounds.failure=enabled;
            else next_sounds.key=enabled;
        }
        esp_err_t err=sound ? feishu_store_save_sounds(&next_sounds):feishu_store_save_assistant(&next);
        if(err!=ESP_OK){snprintf(text,capacity,"Setting could not be saved; unchanged. Retry later.");return false;}
        xSemaphoreTake(s_state_lock,portMAX_DELAY);
        bool unchanged=a->epoch==s_owner_epoch && !s_state.closing &&
            s_state.assistant_settings.boot==before.boot && s_state.assistant_settings.volume==before.volume &&
            s_state.assistant_settings.silence_seconds==before.silence_seconds && s_state.assistant_settings.timeout_minutes==before.timeout_minutes &&
            s_state.sounds.success==sounds.success && s_state.sounds.failure==sounds.failure && s_state.sounds.key==sounds.key;
        if(unchanged){
            if(sound)s_state.sounds=next_sounds;
            else {s_state.assistant_settings=next;atomic_store(&s_assistant_submit_seconds,next.silence_seconds || s_state.nav.page==FEISHU_PAGE_REVIEW ? next.silence_seconds:2);}
            s_state.status_error=false;feishu_utf8_copy(s_state.status,sizeof(s_state.status),"设置已保存");
        }
        xSemaphoreGive(s_state_lock);
        if(!unchanged){enqueue(sound ? WORK_SOUND_SAVE:WORK_ASSISTANT_SAVE);
            snprintf(text,capacity,"A concurrent local edit superseded this request. Read settings again.");return false;}
    }
    int n=snprintf(text,capacity,"{\"saved\":%s,\"boot\":%s,\"silence_seconds\":%u,\"volume\":%u,\"timeout_minutes\":%u,\"success_sound\":%s,\"failure_sound\":%s,\"key_sound\":%s}",
        write ? "true":"false",next.boot ? "true":"false",next.silence_seconds,next.volume,next.timeout_minutes,
        next_sounds.success ? "true":"false",next_sounds.failure ? "true":"false",next_sounds.key ? "true":"false");
    return n>0 && (size_t)n<capacity;
}

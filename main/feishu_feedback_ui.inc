/* One coalesced request; no persistent feedback task or extra audio owner. */
static portMUX_TYPE s_feedback_lock=portMUX_INITIALIZER_UNLOCKED;
static int s_feedback_pending=-1;
static uint32_t s_feedback_at;
static atomic_bool s_feedback_queued;
static void feedback_request(feishu_sound_t kind)
{
#ifdef FEISHU_VOICE_AUTOTEST
    (void)kind;return; /* Voice diagnostics remain silent. */
#endif
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    bool enabled=!s_state.closing && feishu_feedback_enabled(&s_state.sounds,kind);
    bool live=s_state.assistant_active || s_state.assistant_requested;
    xSemaphoreGive(s_state_lock);if(!enabled)return;
    uint32_t now=(uint32_t)(esp_timer_get_time()/1000);
    taskENTER_CRITICAL(&s_feedback_lock);
    if(s_feedback_pending<0 || kind!=FEISHU_SOUND_KEY ||
       !feishu_feedback_fresh((feishu_sound_t)s_feedback_pending,s_feedback_at,now)){
        s_feedback_pending=kind;s_feedback_at=now;
    }
    taskEXIT_CRITICAL(&s_feedback_lock);
    if(!live && !atomic_exchange(&s_feedback_queued,true))enqueue(WORK_FEEDBACK);
}
static void feedback_play(bool live,bool capture)
{
    taskENTER_CRITICAL(&s_feedback_lock);bool pending=s_feedback_pending>=0;taskEXIT_CRITICAL(&s_feedback_lock);
    if(!pending)return;
    /* A key tone is optional; uploading a real microphone frame is not. */
    if(xSemaphoreTake(s_state_lock,0)!=pdTRUE)return;
    feishu_sound_settings_t settings=s_state.sounds;
    bool speaking=s_state.assistant_speech_detected && !s_state.stop_recording;
    bool keep_warm=s_state.nav.page==FEISHU_PAGE_MESSAGES && s_state.nav.composer_selected && !s_state.loading;
    xSemaphoreGive(s_state_lock);
    if(capture && speaking)return;
    taskENTER_CRITICAL(&s_feedback_lock);
    int kind=s_feedback_pending;uint32_t at=s_feedback_at;
    if(capture && kind>=0 && kind!=FEISHU_SOUND_KEY){taskEXIT_CRITICAL(&s_feedback_lock);return;}
    s_feedback_pending=-1;
    taskEXIT_CRITICAL(&s_feedback_lock);
    if(kind<0 || !feishu_feedback_enabled(&settings,(feishu_sound_t)kind) ||
       !feishu_feedback_fresh((feishu_sound_t)kind,at,(uint32_t)(esp_timer_get_time()/1000)))return;
    esp_err_t err=feishu_asr_feedback((feishu_sound_t)kind,live,keep_warm);
    if(live && !capture){
        xSemaphoreTake(s_state_lock,portMAX_DELAY);unsigned volume=s_state.assistant_settings.volume;xSemaphoreGive(s_state_lock);
#ifdef FEISHU_ASSISTANT_AUTOTEST
        volume=0;
#endif
        bsp_audio_set_volume((uint8_t)volume);
    }
    if(err!=ESP_OK){
        ESP_LOGW(TAG,"feedback skipped result=%d",err);
        if(!live && s_focus_audio_reserved){(void)feishu_asr_prepare(false);release_audio_resources();}
    }
}
static void assistant_feedback(void *context,bool capture)
{(void)context;feedback_play(true,capture);}

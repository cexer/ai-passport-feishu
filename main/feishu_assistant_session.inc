/* Persistent assistant WSS. Capture queue and Opus decoder never overlap.
 * Codec stays at 16kHz for the whole visit; response PCM is resampled locally.
 * Microphone input during playback is discarded to avoid self recognition. */
static esp_err_t __attribute__((noinline)) xiao_assistant_wait(esp_transport_handle_t ws,xiao_receive_t *r,
    xiao_assistant_t *a,bool *goodbye)
{
    uint32_t seen=a->received_bytes,progress=(uint32_t)(esp_timer_get_time()/1000);
    bool yielding=false;uint32_t yield_started=0;
    while(!feishu_assistant_reply_stalled((uint32_t)(esp_timer_get_time()/1000),a->received_bytes,&seen,&progress)){
        if(a->cancel && *a->cancel)return FEISHU_HTTP_ERR_CANCELLED;
        if(!yielding && a->hooks->reply_work_pending && a->hooks->reply_work_pending(a->hooks->context,a->player!=NULL)){
            /* Retire reply audio before nested business I/O. Keep draining until
             * the server acknowledges stop, so old audio cannot enter capture. */
            if(xiao_control(ws,a->session,NULL)!=ESP_OK)return ESP_FAIL;
            feishu_xiao_player_close(&a->player);a->can_play=false;
            yielding=true;yield_started=(uint32_t)(esp_timer_get_time()/1000);
            ESP_LOGI(TAG,"ASSISTANT_REPLY_YIELD pending=1 decoder_released=1");
        }
        if(yielding && (uint32_t)((uint32_t)(esp_timer_get_time()/1000)-yield_started)>=3000u){
            ESP_LOGW(TAG,"ASSISTANT_REPLY_YIELD stop_timeout=1");return ESP_ERR_TIMEOUT;
        }
        int kind=xiao_receive(ws,r,100);
        if(kind<0 || kind==XIAO_MESSAGE_ALERT)return ESP_ERR_INVALID_RESPONSE;
        if(!a->player && a->hooks->feedback)a->hooks->feedback(a->hooks->context,false);
        if(kind==XIAO_MESSAGE_STT){
            if(a->hooks->event)a->hooks->event(a->hooks->context,FEISHU_ASSISTANT_HEARD,r->message.text);
            *goodbye=feishu_assistant_goodbye(r->message.text);
#ifdef FEISHU_ASSISTANT_AUTOTEST
            ESP_LOGI(TAG,"ASSISTANT_FIXTURE_STT text=%s farewell=%u",r->message.text,(unsigned)*goodbye);
#endif
            if(*goodbye){(void)xiao_control(ws,a->session,NULL);return ESP_OK;}
        }
        if(a->done){a->can_play=true;return ESP_OK;}
    }
    return ESP_ERR_TIMEOUT;
}
static esp_err_t __attribute__((noinline)) xiao_pcm_upload(esp_transport_handle_t ssl,xiao_capture_t *c,unsigned version,int slot)
{
    /* Actual speech, not keepalive/silent capture, restores interactive power. */
    feishu_power_activity();feishu_power_network_poll();
    if(!feishu_xiao_audio_decode(xiao_capture_packet(c,(unsigned)slot),c->upload_pcm,FEISHU_XIAO_QUEUE_SAMPLES))return ESP_ERR_INVALID_RESPONSE;
    /* The service expects 16k PCM input for recognition and bidirectional visits. Keep
     * the compact 8k local queue, expand its samples at the transport boundary. */
    size_t header=version==2 ? 16:version==3 ? 4:0,n=sizeof(c->upload_pcm)*2;
    uint8_t *wire=c->wire;size_t capacity=sizeof(c->wire);
    memset(wire,0,header);
    if(version==2){wire[1]=2;uint32_t size=htonl((uint32_t)n);memcpy(wire+12,&size,4);}
    if(version==3){uint16_t size=htons((uint16_t)n);memcpy(wire+2,&size,2);}
    for(unsigned i=0;i<FEISHU_XIAO_QUEUE_SAMPLES;++i){
        memcpy(wire+header+i*4,&c->upload_pcm[i],2);
        memcpy(wire+header+i*4+2,&c->upload_pcm[i],2);
    }
    return xiao_audio_write(ssl,wire,capacity,header+n)==(int)(header+n) ? ESP_OK:ESP_FAIL;
}
static esp_err_t __attribute__((noinline)) xiao_assistant_session(volatile bool *stop,volatile bool *cancel,
    volatile unsigned *elapsed,volatile bool *preparing,xiao_assistant_t *a)
{
    xiao_config_t *cfg=calloc(1,sizeof(*cfg));xiao_receive_t *r=calloc(1,sizeof(*r));
    xiao_capture_t *c=NULL;esp_transport_handle_t ssl=NULL,ws=NULL;bool started=false,goodbye=false;
    esp_err_t err=ESP_ERR_NO_MEM;unsigned version=0,turns=0;char *message=NULL;const char *stage="allocate";
    unsigned transport_retries=0;bool greeting_complete=a->hooks->skip_greeting,stable_connection=false;int64_t codec_opened_at=0,ready_since=0;
#ifdef FEISHU_GLOBAL_AUTOTEST
    bool global_idle_done=false,global_resume_tested=false,global_idle_stress_done=false;int64_t idle_after_two=0;
#endif
    if(!cfg || !r)goto done;
 reconnect_transport:
    if(!cfg){cfg=calloc(1,sizeof(*cfg));if(!cfg){err=ESP_ERR_NO_MEM;goto done;}}
    stage="config";r->assistant=a;a->business_receiver=r;err=xiao_config(cfg,cancel,false);if(err!=ESP_OK)goto done;
    version=(unsigned)cfg->version;
    for(unsigned attempt=0;attempt<2;++attempt){
        stage="connect";err=xiao_ws_open(cfg,&ssl,&ws);if(err==ESP_OK)break;
        xiao_ws_close(&ws,&ssl);if(attempt || *cancel)goto done;
        err=xiao_config(cfg,cancel,true);if(err!=ESP_OK)goto done;
    }
    a->business_ws=ws;a->business_ping_at=esp_timer_get_time();
    version=(unsigned)cfg->version;
    memset(cfg,0,sizeof(*cfg));free(cfg);cfg=NULL;
    if(!feishu_xiao_downlink_init(&a->downlink,version)){err=ESP_ERR_NOT_SUPPORTED;goto done;}
    stage="hello";message=calloc(1,256);if(!message){err=ESP_ERR_NO_MEM;goto done;}
    snprintf(message,256,"{\"type\":\"hello\",\"version\":%u,\"transport\":\"websocket\",\"features\":{\"mcp\":true},\"audio_params\":{\"format\":\"pcm\",\"sample_rate\":16000,\"channels\":1,\"frame_duration\":20}}",version);
    if(esp_transport_ws_send_raw(ws,WS_TRANSPORT_OPCODES_TEXT|WS_TRANSPORT_OPCODES_FIN,message,(int)strlen(message),1000)!=(int)strlen(message)){err=ESP_FAIL;goto done;}
    int64_t deadline=esp_timer_get_time()+15000000;
    while(!a->session[0] || !a->initialized || !a->listed){
        if(*cancel){err=FEISHU_HTTP_ERR_CANCELLED;goto done;}
        if(esp_timer_get_time()>=deadline){err=ESP_ERR_TIMEOUT;goto done;}
        if(xiao_receive(ws,r,100)<0){err=ESP_ERR_INVALID_RESPONSE;goto done;}
    }
    ESP_LOGI(TAG,"ASSISTANT_CAPABILITIES_READY complete=1");
    if(!a->audio){
        codec_opened_at=esp_timer_get_time()-(int64_t)(s_audio_primed>ASR_PRIME_MS ? ASR_PRIME_MS:s_audio_primed)*1000;
    stage="audio";err=bsp_audio_init();if(err!=ESP_OK)goto done;a->audio=true;a->fixed_audio=true;
    err=bsp_audio_set_format(16000,16,1);if(err!=ESP_OK)goto done;
    if(a->hooks->activation_chime){
        esp_err_t tone=feishu_asr_feedback(FEISHU_SOUND_SUCCESS,true,false);
        ESP_LOGI(TAG,"ASSISTANT_ACTIVATION_TONE result=%d",tone);
    }
    bsp_audio_set_volume((uint8_t)(a->hooks->get_volume ? a->hooks->get_volume(a->hooks->context):a->hooks->volume));
    ESP_LOGI(TAG,"ASSISTANT_CODEC_OPEN rate=16000");
    }
    a->can_play=true;
    if(!greeting_complete){
    /* Standard XiaoZhi wake-word notification requests the assistant greeting,
     * not a fabricated TTS endpoint or a Feishu message. One per new visit. */
    snprintf(message,256,"{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"detect\",\"text\":\"你好小智\"}",a->session);
    if(esp_transport_ws_send_raw(ws,WS_TRANSPORT_OPCODES_TEXT|WS_TRANSPORT_OPCODES_FIN,message,(int)strlen(message),1000)!=(int)strlen(message)){err=ESP_FAIL;goto done;}
    free(message);message=NULL;
    if(a->hooks->event)a->hooks->event(a->hooks->context,FEISHU_ASSISTANT_THINKING,"正在唤醒小智…");
    /* Mark the greeting request once sent, even if its reply is interrupted. */
    greeting_complete=true;
    if(a->hooks->event)a->hooks->event(a->hooks->context,FEISHU_ASSISTANT_GREETED,"");
    stage="greeting";err=xiao_assistant_wait(ws,r,a,&goodbye);if(err!=ESP_OK)goto done;
    ESP_LOGI(TAG,"ASSISTANT_GREETING done=1 audio_frames=%u",a->audio_frames);
    } else {free(message);message=NULL;ESP_LOGI(TAG,"ASSISTANT_GREETING reused=1");}
#ifdef FEISHU_MEMORY_RECOVERY_AUTOTEST
    static bool memory_injected;
    if(!memory_injected){memory_injected=true;ESP_LOGI(TAG,"ASSISTANT_MEMORY_INJECT once=1");err=ESP_ERR_NO_MEM;goto done;}
#endif
#ifdef FEISHU_SCENARIO_AUTOTEST
    static bool transport_injected;
    if(!transport_injected){transport_injected=true;ESP_LOGI(TAG,"ASSISTANT_TRANSPORT_INJECT once=1");
        esp_transport_close(ws);a->business_failed=true;err=ESP_FAIL;goto done;}
#endif
    while(!*cancel && !goodbye){
        vTaskDelay(pdMS_TO_TICKS(100));feishu_xiao_player_close(&a->player);
        a->can_play=false;a->done=false;
        if(a->hooks->event)a->hooks->event(a->hooks->context,FEISHU_ASSISTANT_PAUSED,"");
        stage="business";if(!xiao_continue_business(a))break;
        while(a->hooks->can_listen && !a->hooks->can_listen(a->hooks->context)){
            stage="confirm";
            a->confirm_wait=true;
            if(!xiao_business_progress(a)){err=ESP_FAIL;goto done;}
            if(*cancel)goto finished;
            if(!xiao_continue_business(a))goto finished;
            if(xiao_receive(ws,r,20)<0){err=ESP_ERR_INVALID_RESPONSE;goto done;}
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        a->confirm_wait=false;a->done=false;
        if(a->hooks->feedback)a->hooks->feedback(a->hooks->context,false);
        stage="capture";c=calloc(1,sizeof(*c));if(!c){err=ESP_ERR_NO_MEM;goto done;}
        atomic_init(&c->extra_packets,NULL);
        if(!xiao_capture_allocate(c,false)){err=ESP_ERR_NO_MEM;goto done;}
        atomic_init(&c->ready_for_voice,false);atomic_init(&c->voice_started,false);
        /* Discard DMA backlog and speaker tail before accepting a new command.
         * No codec reopen or repeated 2-second microphone settling. */
        bsp_audio_set_volume(0);
        for(unsigned i=0;i<12;++i){err=bsp_audio_read(c->pcm,sizeof(c->pcm));if(err!=ESP_OK)goto done;}
        *stop=false;*elapsed=0;if(preparing)*preparing=true;
        c->stop=stop;c->cancel=cancel;c->elapsed=elapsed;c->preparing=preparing;
        c->speech_detected=a->hooks->speech_detected;if(c->speech_detected)*c->speech_detected=false;
        c->continuous=true;c->silence_seconds=a->hooks->silence_seconds;c->get_silence=a->hooks->get_silence;c->silence_context=a->hooks->context;c->stack_free_min=XIAO_STACK;c->started=xTaskGetTickCount();
        c->free=xQueueCreate(XIAO_FRAMES,sizeof(int));c->ready=xQueueCreate(XIAO_FRAMES+1,sizeof(int));c->done=xSemaphoreCreateBinary();
        if(!c->free || !c->ready || !c->done){err=ESP_ERR_NO_MEM;goto done;}
        for(int i=0;i<FEISHU_XIAO_IDLE_FRAMES;++i)xQueueSend(c->free,&i,0);
        c->enabled=true;
        int64_t warmed=(esp_timer_get_time()-codec_opened_at)/1000;
        s_audio_primed=warmed>=ASR_PRIME_MS ? ASR_PRIME_MS:(unsigned)(warmed>0 ? warmed:0);
        bool streaming=false;int64_t last_ping=esp_timer_get_time();
#ifdef FEISHU_ASSISTANT_AUTOTEST
        unsigned fixture=turns==0 ? 0:turns==1 ? 4:5;
#if defined(FEISHU_SCENARIO_AUTOTEST) || defined(FEISHU_GLOBAL_AUTOTEST)
        if(turns==0)fixture=3; /* Real cloud MCP open-chat path, not only direct API calls. */
#endif
        memset(&voice_probe,0,sizeof(voice_probe));voice_probe.active=true;
        voice_probe.pcm=assistant_fixtures[fixture];voice_probe.size=assistant_fixture_sizes[fixture];
        voice_probe.stop_ms=UINT32_MAX;voice_probe.cancel_ms=12000;voice_probe.stop=stop;voice_probe.cancel=cancel;
#ifdef FEISHU_GLOBAL_AUTOTEST
        if(!global_idle_done){voice_probe.size=0;voice_probe.cancel_ms=900000;} /* Bounded whole-suite quiet capture, diagnostic only. */
        if(turns==2){
            if(!idle_after_two)idle_after_two=esp_timer_get_time()+
                75000000;
            int64_t left=(idle_after_two-esp_timer_get_time())/1000;
            voice_probe.prefix_ms=left>0 ? (unsigned)left:0;voice_probe.cancel_ms=voice_probe.prefix_ms+12000;
            ESP_LOGI(TAG,"ASSISTANT_TWO_TURN_IDLE remaining_ms=%u",voice_probe.prefix_ms);
        }
#endif
#endif
        if(xTaskCreate(xiao_capture_task,"assistant_capture",XIAO_STACK,c,6,&c->task)!=pdPASS){err=ESP_ERR_NO_MEM;goto done;}
        started=true;a->capture=c;bool listening_announced=false;
        if(a->hooks->event)a->hooks->event(a->hooks->context,FEISHU_ASSISTANT_PREPARING,"");
 capture_again:
#ifdef FEISHU_GLOBAL_AUTOTEST
        if(!global_idle_stress_done
#ifdef FEISHU_ASSISTANT_TIMEOUT_AUTOTEST
           && false
#endif
        ){
            vTaskDelay(pdMS_TO_TICKS(5000));global_idle_stress_done=true;
            ESP_LOGI(TAG,"ASSISTANT_IDLE_PRESSURE pass=%u delay_ms=5000 held=%u ready=%u free=%u error=%d",
                (unsigned)(c->error==ESP_OK && c->preroll_count==6 && uxQueueMessagesWaiting(c->ready)==0),
                c->preroll_count,(unsigned)uxQueueMessagesWaiting(c->ready),(unsigned)uxQueueMessagesWaiting(c->free),c->error);
        }
#endif
        for(;;){
            if(!listening_announced && atomic_load(&c->ready_for_voice)){
                if(a->hooks->event)a->hooks->event(a->hooks->context,FEISHU_ASSISTANT_LISTENING,"");
                listening_announced=true;if(!ready_since)ready_since=esp_timer_get_time();ESP_LOGI(TAG,"ASSISTANT_LISTEN_READY microphone=1");
            }
            if(!streaming && ready_since && esp_timer_get_time()-ready_since>=30000000)stable_connection=true;
#ifdef FEISHU_GLOBAL_AUTOTEST
            static bool idle_transport_injected;
            if(turns==2 && !idle_transport_injected && !c->vad.heard && idle_after_two && esp_timer_get_time()>=idle_after_two-40000000 && esp_timer_get_time()<idle_after_two){
                idle_transport_injected=true;ESP_LOGI(TAG,"ASSISTANT_IDLE_TRANSPORT_INJECT capture_active=1");
                esp_transport_close(ws);a->business_failed=true;err=ESP_FAIL;goto done;
            }
#endif
            if(*cancel){err=FEISHU_HTTP_ERR_CANCELLED;goto done;}
            if(c->error!=ESP_OK){err=c->error;goto done;}
            if(a->hooks->feedback)a->hooks->feedback(a->hooks->context,true);
            if(a->hooks->work_pending && !c->vad.heard && !c->vad.speech_ms && a->hooks->work_pending(a->hooks->context)){
                if(a->hooks->can_listen && !a->hooks->can_listen(a->hooks->context))*stop=true;
                else {
                    unsigned capture_before=*elapsed;
                    if(!xiao_continue_business(a))goto finished;
#ifdef FEISHU_GLOBAL_AUTOTEST
                    bool retained=c==a->capture && c->error==ESP_OK && *elapsed>=capture_before;
                    ESP_LOGI(TAG,"ASSISTANT_CAPTURE_BUSINESS pass=%u before=%u after=%u held=%u",(unsigned)retained,capture_before,*elapsed,c->preroll_count);
                    if(!global_idle_done && (!a->hooks->diagnostic_ready || a->hooks->diagnostic_ready(a->hooks->context))){
                        global_idle_done=true;voice_probe.prefix_ms=*elapsed+200;
                        voice_probe.size=assistant_fixture_sizes[fixture];voice_probe.cancel_ms=voice_probe.prefix_ms+12000;
                        /* Force a real read to overlap the fixture's speech onset.
                         * Its cancellation must leave the same capture/queued prefix intact. */
                        char *probe_text=calloc(1,640);cJSON *probe_args=cJSON_CreateObject();
                        bool queried=false;
                        if(probe_text && probe_args){
                            cJSON_AddNumberToObject(probe_args,"chat_index",0);
                            a->business_busy=true;feishu_http_set_progress(xiao_business_progress,a);
                            feishu_http_set_read_preemptible(true);
                            queried=a->hooks->execute(a->hooks->context,"self.feishu.get_messages",probe_args,probe_text,640);
                            feishu_http_set_progress(NULL,NULL);a->business_busy=false;
                        }
                        bool preempt=retained && !queried && a->voice_priority && c->error==ESP_OK && atomic_load(&c->voice_started);
                        a->voice_priority=false;cJSON_Delete(probe_args);free(probe_text);
                        ESP_LOGI(TAG,"ASSISTANT_CAPTURE_PREEMPT pass=%u queued=%u elapsed=%u",(unsigned)preempt,(unsigned)uxQueueMessagesWaiting(c->ready),*elapsed);
                        global_resume_tested=preempt;
                        if(!preempt){err=ESP_ERR_INVALID_RESPONSE;goto done;}
                        last_ping=esp_timer_get_time()-12000000; /* Valid idle-to-speech keepalive boundary. */
                        ESP_LOGI(TAG,"ASSISTANT_KEEPALIVE_PHASE_TEST remaining_ms=3000");
                    }
#else
                    (void)capture_before;
#endif
                }
            }
            if(!streaming && esp_timer_get_time()-last_ping>=15000000){
#ifdef FEISHU_GLOBAL_AUTOTEST
                ESP_LOGI(TAG,"ASSISTANT_CAPTURE_PING streaming=%u elapsed=%u",(unsigned)streaming,*elapsed);
#endif
                if(esp_transport_ws_send_raw(ws,WS_TRANSPORT_OPCODES_PING|WS_TRANSPORT_OPCODES_FIN,"",0,1000)<0){err=ESP_FAIL;goto done;}
                last_ping=esp_timer_get_time();
            }
            int slot;
            if(xQueueReceive(c->ready,&slot,pdMS_TO_TICKS(30))==pdTRUE){
                if(slot<0){err=slot==-1 ? ESP_OK:c->error;break;}
                /* Feishu GET has yielded before upload. Expand the same queue
                 * before listen/start or any potentially delayed socket write. */
                if(!xiao_capture_promote(c)){err=ESP_ERR_NO_MEM;goto done;}
                if(!streaming){
                    /* The producer publishes its prefix only after VAD onset.
                     * Receiving a real slot therefore starts exactly one turn. */
                    a->done=false;
                    err=xiao_control(ws,a->session,"start");if(err!=ESP_OK)goto done;
                    ESP_LOGI(TAG,"ASSISTANT_STREAM_START preroll_producer=1");streaming=true;
                }
#ifdef FEISHU_GLOBAL_AUTOTEST
                int64_t upload_at=esp_timer_get_time();
#endif
                stage="upload";err=xiao_pcm_upload(ssl,c,version,slot);xQueueSend(c->free,&slot,0);
#ifdef FEISHU_GLOBAL_AUTOTEST
                ESP_LOGI(TAG,"ASSISTANT_UPLOAD_PROFILE ms=%u ready=%u free=%u result=%d",
                    (unsigned)((esp_timer_get_time()-upload_at)/1000),
                    (unsigned)uxQueueMessagesWaiting(c->ready),(unsigned)uxQueueMessagesWaiting(c->free),err);
#endif
                if(err!=ESP_OK)goto done;
                last_ping=esp_timer_get_time();a->business_ping_at=last_ping;
            }
#ifdef FEISHU_GLOBAL_AUTOTEST
            int64_t receive_at=esp_timer_get_time();
#endif
            stage="listen";int kind=xiao_receive(ws,r,0);
#ifdef FEISHU_GLOBAL_AUTOTEST
            unsigned receive_ms=(unsigned)((esp_timer_get_time()-receive_at)/1000);
            if(receive_ms>10)ESP_LOGI(TAG,"ASSISTANT_RECEIVE_PROFILE ms=%u kind=%d ready=%u",receive_ms,kind,(unsigned)uxQueueMessagesWaiting(c->ready));
#endif
            if(kind<0 || kind==XIAO_MESSAGE_ALERT || kind==XIAO_MESSAGE_STT){err=ESP_ERR_INVALID_RESPONSE;goto done;}
        }
        if(err!=ESP_OK)goto done;
        if(!streaming){
#ifdef FEISHU_GLOBAL_AUTOTEST
            if(!global_resume_tested && c->preroll_count){
                /* Inject the one-frame quiet-stop race without adding a
                 * cloud utterance; the following real fixtures stay intact. */
                c->vad.speech_ms=20;global_resume_tested=true;
            }
#endif
            /* A voice onset can race the quiet-work stop by one DMA frame.
             * Retain that frame and the preroll, then resume the same capture
             * instead of silently discarding the beginning of a command. */
            if(c->vad.speech_ms && (!a->hooks->can_listen || a->hooks->can_listen(a->hooks->context))){
                xSemaphoreTake(c->done,portMAX_DELAY);started=false;
                vTaskDelay(pdMS_TO_TICKS(10));*stop=false;
                listening_announced=false;atomic_store(&c->ready_for_voice,false);
                if(a->hooks->event)a->hooks->event(a->hooks->context,FEISHU_ASSISTANT_PREPARING,"");
                if(xTaskCreate(xiao_capture_task,"assistant_capture",XIAO_STACK,c,6,&c->task)!=pdPASS){err=ESP_ERR_NO_MEM;goto done;}
                started=true;ESP_LOGI(TAG,"ASSISTANT_CAPTURE_RESUME prefix_ms=%u",c->vad.speech_ms);
                goto capture_again;
            }
            a->capture=NULL;xiao_capture_destroy(&c,started);started=false;
#ifdef FEISHU_GLOBAL_AUTOTEST
            global_idle_done=true;
#endif
            continue;
        }
        ESP_LOGI(TAG,"ASSISTANT_SUBMIT speech=%u silence_ms=%u command_ms=%u",(unsigned)c->vad.heard,c->vad.silence_ms,c->command_ms);
        err=xiao_control(ws,a->session,"stop");if(err!=ESP_OK)goto done;
        a->capture=NULL;xiao_capture_destroy(&c,started);started=false;
        a->done=false;a->can_play=true;bsp_audio_set_volume((uint8_t)(a->hooks->get_volume ? a->hooks->get_volume(a->hooks->context):a->hooks->volume));
        if(a->hooks->event)a->hooks->event(a->hooks->context,FEISHU_ASSISTANT_THINKING,"正在处理…");
        stage="reply";err=xiao_assistant_wait(ws,r,a,&goodbye);if(err!=ESP_OK)goto done;
        ++turns;if(ready_since && esp_timer_get_time()-ready_since>=30000000)stable_connection=true;ESP_LOGI(TAG,"ASSISTANT_CONTINUOUS turn=%u goodbye=%u heap=%u",turns,(unsigned)goodbye,(unsigned)esp_get_free_heap_size());
#ifdef FEISHU_ASSISTANT_AUTOTEST
        if(turns>=3 && !goodbye){err=ESP_ERR_INVALID_RESPONSE;goto done;}
#endif
    }
 finished:
    err=*cancel ? FEISHU_HTTP_ERR_CANCELLED:a->business_failed ? ESP_FAIL:ESP_OK;
 done:
    if(ready_since && esp_timer_get_time()-ready_since>=30000000 && a->last_payload_at &&
       esp_timer_get_time()-a->last_payload_at<5000000)stable_connection=true;
    if(err!=ESP_OK && err!=ESP_ERR_NO_MEM && !*cancel && !goodbye &&
       (a->business_failed || ((err==ESP_FAIL || err==ESP_ERR_INVALID_RESPONSE || err==ESP_ERR_TIMEOUT) &&
        (!strcmp(stage,"connect") || !strcmp(stage,"hello") || !strcmp(stage,"greeting") ||
         !strcmp(stage,"listen") || !strcmp(stage,"upload") || !strcmp(stage,"reply")))) &&
       feishu_assistant_transport_retry(&transport_retries,stable_connection)){
        ESP_LOGW(TAG,"ASSISTANT_TRANSPORT_RECONNECT attempt=%u stable=%u codec_retained=%u result=%d stage=%s",transport_retries,(unsigned)stable_connection,(unsigned)a->audio,err,stage);
        feishu_http_set_progress(NULL,NULL);a->capture=NULL;xiao_capture_destroy(&c,started);started=false;
        feishu_xiao_player_close(&a->player);xiao_ws_close(&ws,&ssl);bsp_audio_set_volume(0);
        free(message);message=NULL;if(cfg){memset(cfg,0,sizeof(*cfg));free(cfg);cfg=NULL;}
        free(a->request);a->request=NULL;a->used=a->capacity=0;
        memset(r,0,sizeof(*r));r->assistant=a;
        a->session[0]=0;a->initialized=a->listed=a->done=a->business_failed=a->business_busy=false;
        a->business_ws=NULL;a->business_receiver=r;a->can_play=false;a->last_payload_at=0;stable_connection=false;ready_since=0;
        if(a->hooks->event)a->hooks->event(a->hooks->context,FEISHU_ASSISTANT_RECONNECTING,"");
        vTaskDelay(pdMS_TO_TICKS(500));
        if(*cancel){err=FEISHU_HTTP_ERR_CANCELLED;goto final_cleanup;}
        if(a->hooks->continue_session && !a->hooks->continue_session(a->hooks->context)){err=ESP_OK;goto final_cleanup;}
        goto reconnect_transport;
    }
 final_cleanup:
    feishu_http_set_progress(NULL,NULL);a->business_ws=NULL;a->business_receiver=NULL;a->business_busy=false;
    free(message);a->capture=NULL;xiao_capture_destroy(&c,started);xiao_ws_close(&ws,&ssl);
    if(!a->audio && s_audio_primed)bsp_audio_deinit();
    if(cfg){memset(cfg,0,sizeof(*cfg));free(cfg);}if(r){memset(r,0,sizeof(*r));free(r);}
#ifdef FEISHU_GLOBAL_AUTOTEST
    ESP_LOGI(TAG,"ASSISTANT_IDLE_RESUME_TEST pass=%u",(unsigned)global_resume_tested);
#endif
    ESP_LOGI(TAG,"ASSISTANT_SESSION result=%d turns=%u goodbye=%u stage=%s",err,turns,(unsigned)goodbye,stage);
    return err;
}

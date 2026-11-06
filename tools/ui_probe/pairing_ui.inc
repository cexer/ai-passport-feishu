/* Opt-in LVGL-thread smoke test. Uses the device's existing paired identity;
 * changes only temporary UI state, never unpairs, records, sends or deletes. */
static void pairing_ui_smoke(lv_timer_t *timer)
{
    static unsigned stage;
    static uint32_t started,phase_at,request;
    static feishu_nav_t saved_nav;
    uint32_t now=(uint32_t)(esp_timer_get_time()/1000);
    if(!started)started=now;
    bool done=false,pass=true,enter_network=false,open_network_actions=false,start_network_scan=false,open_network_password=false,select_network_action=false;
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    if(now-started>90000){done=true;pass=false;}
    else if(stage==0) {
        if(!s_state.ready || s_state.loading || !online()){xSemaphoreGive(s_state_lock);return;}
        saved_nav=s_state.nav;s_state.nav.page=FEISHU_PAGE_XIAOZHI;
        memset(&s_state.pairing,0,sizeof(s_state.pairing));
        s_state.cancel_pairing=s_state.pairing_busy=s_state.pairing_auto_blocked=false;
        s_state.pairing_retry_at=now+30000;s_state.status[0]=0;
        phase_at=now;request=s_state.pairing_request;stage=1;
    } else if(stage==1 && now-phase_at>=600) {
        pass=s_state.pairing_request==request && s_pair_qr && lv_image_get_src(s_pair_qr)==&feishu_xiao_console_qr;
        ESP_LOGI(TAG,"PAIR_UI_SMOKE phase=cooldown pass=%u qr=%u",(unsigned)pass,(unsigned)(s_pair_qr!=NULL));
        if(pass){s_state.pairing_retry_at=0;stage=2;}else done=true;
    } else if(stage==2 && !s_state.pairing_busy && s_state.pairing_request!=request) {
        pass=s_state.pairing_request==request+1 && s_state.pairing.paired && !s_state.status_error;
        ESP_LOGI(TAG,"PAIR_UI_SMOKE phase=missing pass=%u",(unsigned)pass);
        if(pass) {
            request=s_state.pairing_request;s_state.pairing.paired=false;
            feishu_utf8_copy(s_state.pairing.code,sizeof(s_state.pairing.code),"123456");
            s_state.pairing.expires_at_ms=now-1;s_state.pairing_retry_at=0;stage=3;
        } else done=true;
    } else if(stage==3 && !s_state.pairing_busy && s_state.pairing_request!=request) {
        pass=s_state.pairing_request==request+1 && s_state.pairing.paired && !s_state.status_error;
        ESP_LOGI(TAG,"PAIR_UI_SMOKE phase=expired pass=%u",(unsigned)pass);
        if(pass) {
            s_state.nav.page=FEISHU_PAGE_DRAFTS;s_state.nav.draft_count=2;s_state.nav.draft_index=0;
            feishu_utf8_copy(s_state.draft_labels[0],sizeof(s_state.draft_labels[0]),"界面测试\n待确认 48字节");
            feishu_utf8_copy(s_state.draft_labels[1],sizeof(s_state.draft_labels[1]),"测试会话\n待确认 36字节");
            s_state.status[0]=0;phase_at=now;stage=4;
        } else done=true;
    } else if(stage==4 && now-phase_at>=600) {
        bool colors=lv_color_to_u32(lv_obj_get_style_text_color(s_row_text[0],0))==lv_color_to_u32(lv_color_hex(FEISHU_TEXT)) &&
            lv_color_to_u32(lv_obj_get_style_text_color(s_row_subtext[0],0))==lv_color_to_u32(lv_color_hex(FEISHU_MUTED));
        lv_mem_monitor_t memory;lv_mem_monitor(&memory);
        pass=colors && !s_pair_qr && !s_pair_caption && !lv_obj_has_flag(s_row_subtext[0],LV_OBJ_FLAG_HIDDEN);
        ESP_LOGI(TAG,"PAIR_UI_SMOKE phase=draft pass=%u colors=%u qr_released=%u lv_free=%u",(unsigned)pass,(unsigned)colors,(unsigned)(s_pair_qr==NULL),(unsigned)memory.free_size);
        done=true;
#ifdef FEISHU_NETWORK_UI_AUTOTEST
        if(pass) {
            s_network_ui=calloc(1,sizeof(*s_network_ui));pass=s_network_ui!=NULL;
            if(pass){s_network_ui->generation=atomic_fetch_add(&s_network_generation,1)+1;strcpy(s_network_ui->ssid,"Very-long-network-name-123456789");memset(s_network_ui->password,'a',64);s_network_ui->password[64]=0;s_state.nav.page=FEISHU_PAGE_NETWORK_PASSWORD;phase_at=now;stage=5;done=false;}
        }
#endif
    } else if(stage==5 && now-phase_at>=600) {
        lv_mem_monitor_t memory;lv_mem_monitor(&memory);
        pass=s_network_ui && s_state.nav.page==FEISHU_PAGE_NETWORK_PASSWORD && lv_obj_get_height(s_content)<=85;
        ESP_LOGI(TAG,"NETWORK_UI_SMOKE phase=keyboard pass=%u lv_free=%u",(unsigned)pass,(unsigned)memory.free_size);
        network_close_locked();done=!pass;
        if(pass){stage=6;phase_at=now;enter_network=true;}
    } else if(stage==6 && now-phase_at>=600 && s_network_ui && !s_network_ui->busy) {
        pass=s_network_ui->count>0 && s_network_ui->selected==s_network_ui->count && !s_state.status_error &&
            lv_obj_get_y(s_rows[4])==218 && lv_obj_get_style_border_width(s_rows[4],0)==2 && lv_obj_get_y(s_rows[5])==243;
        ESP_LOGI(TAG,"NETWORK_UI_SMOKE phase=default_add pass=%u",(unsigned)pass);
        done=!pass;
        if(pass){stage=7;phase_at=now;open_network_actions=true;}
    } else if(stage==7 && now-phase_at>=600) {
        pass=s_state.nav.page==FEISHU_PAGE_NETWORK_ACTIONS && !strcmp(lv_label_get_text(s_row_text[0]),"连接") &&
            !strcmp(lv_label_get_text(s_row_text[1]),"断开") && !strcmp(lv_label_get_text(s_row_text[2]),"忘记") &&
            !strcmp(lv_label_get_text(s_row_text[3]),"设置密码");
        ESP_LOGI(TAG,"NETWORK_UI_SMOKE phase=actions pass=%u",(unsigned)pass);
        done=!pass;
        if(pass){stage=10;phase_at=now;select_network_action=true;}
    } else if(stage==10 && now-phase_at>=600) {
        pass=s_state.nav.page==FEISHU_PAGE_NETWORK_ACTIONS && s_network_ui && s_network_ui->action_index==1 &&
            lv_obj_get_style_border_width(s_rows[1],0)==2 && lv_obj_get_style_border_width(s_rows[0],0)==0;
        ESP_LOGI(TAG,"NETWORK_UI_SMOKE phase=action_focus pass=%u",(unsigned)pass);
        done=!pass;
        if(pass){stage=8;phase_at=now;start_network_scan=true;}
    } else if(stage==8 && now-phase_at>=600 && s_network_ui && !s_network_ui->busy) {
        pass=s_state.nav.page==FEISHU_PAGE_NETWORK_SCAN && s_network_ui->scan_count>0;
        ESP_LOGI(TAG,"NETWORK_UI_SMOKE phase=add_scan pass=%u",(unsigned)pass);
        done=!pass;
        if(pass){stage=9;phase_at=now;open_network_password=true;}
    } else if(stage==9 && now-phase_at>=600) {
        pass=s_state.nav.page==FEISHU_PAGE_NETWORK_PASSWORD && s_network_ui && s_network_ui->ssid[0] && !s_network_ui->password[0] && !s_network_ui->editing_saved;
        ESP_LOGI(TAG,"NETWORK_UI_SMOKE phase=scan_password pass=%u",(unsigned)pass);
        network_close_locked();done=true;
    }
    if(done) {
        if(s_network_ui)network_close_locked();
        s_state.nav=saved_nav;s_state.cancel_pairing=true;++s_state.pairing_request;
        s_state.pairing_busy=false;s_state.status[0]=0;s_state.status_error=false;
        ESP_LOGI(TAG,"PAIR_UI_SMOKE_DONE pass=%u no_sends=1 no_unpair=1",(unsigned)pass);
        s_ui_smoke_timer=NULL;lv_timer_delete(timer);
    }
    xSemaphoreGive(s_state_lock);
    if(enter_network)network_enter();
    if(open_network_actions){network_navigation(FEISHU_NAV_UP);network_navigation(FEISHU_NAV_OK);}
    if(select_network_action)network_navigation(FEISHU_NAV_DOWN);
    if(start_network_scan){network_navigation(FEISHU_NAV_BACK);network_navigation(FEISHU_NAV_OK);}
    if(open_network_password)network_navigation(FEISHU_NAV_OK);
#ifdef FEISHU_NETWORK_UI_AUTOTEST
    if(done && pass)enqueue(WORK_NETWORK_AUTOTEST);
#endif
}

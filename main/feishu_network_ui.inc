/* Page-owned state; workers publish only to the same page generation. */
#include "feishu_wifi_keyboard.h"
#include "esp_wifi.h"
typedef struct {
    uint32_t generation;
    char names[FEISHU_WIFI_SAVED_MAX][33];
    feishu_wifi_ap_t aps[FEISHU_WIFI_SCAN_MAX];
    size_t count,active,scan_count,selected,saved_index,key,list_cursor,action_index;
    unsigned keyboard_page;
    char ssid[33],password[65];
    bool busy,secured,editing_saved;
} network_ui_t;
static network_ui_t *s_network_ui;
static atomic_uint s_network_generation;
static void show_list_rows(bool visible);
static void set_label_text(lv_obj_t *label,const char *text);
static void style_list_row(size_t slot,bool selected);
static void render_choice_rows(const char *const *labels,size_t count,size_t selected,bool multiline);
static bool network_page(feishu_page_t page)
{return page==FEISHU_PAGE_NETWORK || page==FEISHU_PAGE_NETWORK_SCAN || page==FEISHU_PAGE_NETWORK_PASSWORD || page==FEISHU_PAGE_NETWORK_ACTIONS;}
static bool network_cancel(void *context)
{return atomic_load(&s_network_generation)!=(unsigned)(uintptr_t)context;}
static void network_close_locked(void)
{
    atomic_fetch_add(&s_network_generation,1);
    if(s_network_ui){memset(s_network_ui,0,sizeof(*s_network_ui));free(s_network_ui);s_network_ui=NULL;}
    s_state.loading=false;s_state.nav.page=FEISHU_PAGE_SETTINGS;s_state.nav.settings_index=FEISHU_SETTING_NETWORK;
    s_state.status[0]=0;s_state.status_error=false;
}
static void network_enter(void)
{
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    if(s_network_ui){memset(s_network_ui,0,sizeof(*s_network_ui));free(s_network_ui);}
    s_network_ui=calloc(1,sizeof(*s_network_ui));
    if(s_network_ui) {
        s_state.nav.page=FEISHU_PAGE_NETWORK;
        s_network_ui->generation=atomic_fetch_add(&s_network_generation,1)+1;
        s_network_ui->busy=true;s_state.loading=false;
        feishu_utf8_copy(s_state.status,sizeof(s_state.status),"正在读取已记忆网络…");
    } else {s_state.nav.page=FEISHU_PAGE_SETTINGS;feishu_utf8_copy(s_state.status,sizeof(s_state.status),"内存不足，未修改网络");s_state.status_error=true;}
    bool okay=s_network_ui!=NULL;xSemaphoreGive(s_state_lock);if(okay)enqueue(WORK_NETWORK_LIST);
}
static void network_work(work_command_t command)
{
    int64_t started=esp_timer_get_time();
    uint32_t generation;size_t index;char ssid[33]={0},password[65]={0};bool candidate=false;
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    if(!s_network_ui || !network_page(s_state.nav.page)){xSemaphoreGive(s_state_lock);return;}
    generation=s_network_ui->generation;index=s_network_ui->saved_index;
    if(command==WORK_NETWORK_CONNECT && s_state.nav.page==FEISHU_PAGE_NETWORK_PASSWORD && !s_network_ui->editing_saved) {
        candidate=true;memcpy(ssid,s_network_ui->ssid,sizeof(ssid));
    }
    if(candidate || command==WORK_NETWORK_PASSWORD_SAVE)memcpy(password,s_network_ui->password,sizeof(password));
    xSemaphoreGive(s_state_lock);
    esp_err_t err=ESP_OK;bool restored=false;const char *notice=NULL;
    char names[FEISHU_WIFI_SAVED_MAX][33]={{0}};size_t count=0,active=0;
    feishu_wifi_ap_t aps[FEISHU_WIFI_SCAN_MAX]={0};size_t n=0;wifi_config_t config={0};
    if(command==WORK_NETWORK_CONNECT) {
        feishu_asr_prepare(false);
        err=feishu_network_switch(index,candidate ? ssid:NULL,password,network_cancel,(void *)(uintptr_t)generation,&restored);
        if(err==ESP_OK)notice="连接成功";
        else notice=restored ? (feishu_network_user_offline() ? "连接失败，仍保持断开":"连接失败，已恢复原网络"):"连接失败，原配置保留";
    } else if(command==WORK_NETWORK_SCAN)err=feishu_network_scan(aps,FEISHU_WIFI_SCAN_MAX,&n);
    else if(command==WORK_NETWORK_EDIT || command==WORK_NETWORK_PASSWORD_SAVE) {
        err=feishu_store_get_wifi(index,&config,sizeof(config));
        if(err==ESP_OK && command==WORK_NETWORK_PASSWORD_SAVE) {
            memset(config.sta.password,0,sizeof(config.sta.password));
            memcpy(config.sta.password,password,strlen(password));
            err=feishu_store_update_wifi(index,&config,sizeof(config));notice="密码已保存，连接时生效";
        }
    } else if(command==WORK_NETWORK_DISCONNECT || command==WORK_NETWORK_REMOVE) {
        err=feishu_store_list_wifi(names,&count,&active,sizeof(config));
        if(err==ESP_OK && index>=count)err=ESP_ERR_INVALID_ARG;
        if(err==ESP_OK && command==WORK_NETWORK_DISCONNECT) {
            if(index!=active || !feishu_network_connected()){err=ESP_ERR_INVALID_STATE;notice="此网络未连接";}
            else {feishu_asr_prepare(false);err=feishu_network_disconnect();notice="已断开，网络仍记忆";}
        } else if(err==ESP_OK) {
            err=feishu_store_remove_wifi(index,sizeof(config));
            if(err==ESP_OK && index==active){feishu_asr_prepare(false);feishu_network_disconnect();}
            notice="已忘记网络";
        }
    }
    if(err==ESP_OK && command!=WORK_NETWORK_SCAN && command!=WORK_NETWORK_EDIT && command!=WORK_NETWORK_PASSWORD_SAVE)
        err=feishu_store_list_wifi(names,&count,&active,sizeof(config));
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    if(s_network_ui && s_network_ui->generation==generation && !network_cancel((void *)(uintptr_t)generation)) {
        network_ui_t *u=s_network_ui;u->busy=false;s_state.loading=false;s_state.status_error=err!=ESP_OK;
        if(err==ESP_OK && command==WORK_NETWORK_SCAN) {
            memcpy(u->aps,aps,sizeof(aps));u->scan_count=n;u->selected=0;
            notice=n ? "选择网络后输入密码":"未发现网络，可手动配网";
        } else if(err==ESP_OK && command==WORK_NETWORK_EDIT) {
            memcpy(u->ssid,config.sta.ssid,32);u->ssid[32]=0;
            memcpy(u->password,config.sta.password,64);u->password[64]=0;
            u->editing_saved=true;u->secured=false;u->keyboard_page=0;u->key=0;s_state.nav.page=FEISHU_PAGE_NETWORK_PASSWORD;
            notice="修改后选择保存，不自动连接";
        } else if(err==ESP_OK && command==WORK_NETWORK_PASSWORD_SAVE) {
            memset(u->password,0,sizeof(u->password));u->editing_saved=false;
            s_state.nav.page=FEISHU_PAGE_NETWORK_ACTIONS;u->action_index=0;
        } else if(err==ESP_OK) {
            memcpy(u->names,names,sizeof(names));u->count=count;u->active=active;
            if(command!=WORK_NETWORK_DISCONNECT){s_state.nav.page=FEISHU_PAGE_NETWORK;u->selected=count;u->list_cursor=0;}
            memset(u->password,0,sizeof(u->password));u->editing_saved=false;
        }
        if(!notice && err!=ESP_OK)notice=command==WORK_NETWORK_SCAN ? "扫描失败，可重试或手动配网":"操作失败，原配置保留";
        if(notice)feishu_utf8_copy(s_state.status,sizeof(s_state.status),notice);
        else snprintf(s_state.status,sizeof(s_state.status),"%s，已记忆%u/10",feishu_network_connected() ? "网络已连接":"当前离线",(unsigned)u->count);
    }
    if(command==WORK_NETWORK_LIST)ESP_LOGI(TAG,"NETWORK_LIST_READY result=%d count=%u ms=%u",err,(unsigned)count,(unsigned)((esp_timer_get_time()-started)/1000));
    xSemaphoreGive(s_state_lock);memset(password,0,sizeof(password));memset(&config,0,sizeof(config));
}
static bool network_navigation(feishu_nav_event_t event)
{
    work_command_t command=WORK_EXIT;bool portal=false;
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    if(!network_page(s_state.nav.page)){xSemaphoreGive(s_state_lock);return false;}
    network_ui_t *u=s_network_ui;
    if(!u){network_close_locked();xSemaphoreGive(s_state_lock);return true;}
    if(event==FEISHU_NAV_BACK) {
        if(u->busy || s_state.nav.page==FEISHU_PAGE_NETWORK)network_close_locked();
        else {
            bool editing=s_state.nav.page==FEISHU_PAGE_NETWORK_PASSWORD && u->editing_saved;
            memset(u->password,0,sizeof(u->password));u->editing_saved=false;
            s_state.nav.page=editing ? FEISHU_PAGE_NETWORK_ACTIONS:FEISHU_PAGE_NETWORK;
            u->selected=u->count;s_state.status[0]=0;s_state.status_error=false;
        }
        xSemaphoreGive(s_state_lock);return true;
    }
    if(u->busy){xSemaphoreGive(s_state_lock);return true;}
    size_t count=s_state.nav.page==FEISHU_PAGE_NETWORK ? u->count+2:
        s_state.nav.page==FEISHU_PAGE_NETWORK_SCAN ? u->scan_count+2:
        s_state.nav.page==FEISHU_PAGE_NETWORK_ACTIONS ? 4:feishu_wifi_key_count(u->keyboard_page);
    size_t *selection=s_state.nav.page==FEISHU_PAGE_NETWORK_PASSWORD ? &u->key:
        s_state.nav.page==FEISHU_PAGE_NETWORK_ACTIONS ? &u->action_index:&u->selected;
    bool keyboard=s_state.nav.page==FEISHU_PAGE_NETWORK_PASSWORD;
    if(event==FEISHU_NAV_UP)*selection=feishu_focus_step(*selection,count,-1,keyboard);
    if(event==FEISHU_NAV_DOWN)*selection=feishu_focus_step(*selection,count,1,keyboard);
    if(s_state.nav.page==FEISHU_PAGE_NETWORK || s_state.nav.page==FEISHU_PAGE_NETWORK_SCAN) {
        size_t networks=s_state.nav.page==FEISHU_PAGE_NETWORK ? u->count:u->scan_count;
        if(event==FEISHU_NAV_COMPOSE){if(u->selected<networks)u->list_cursor=u->selected;u->selected=networks;}
        else if(event==FEISHU_NAV_BROWSE && networks)u->selected=u->list_cursor<networks ? u->list_cursor:0;
        if(u->selected<networks)u->list_cursor=u->selected;
    }
    if(event==FEISHU_NAV_OK) {
        s_state.status_error=false;s_state.status[0]=0;
        if(s_state.nav.page==FEISHU_PAGE_NETWORK) {
            if(u->selected<u->count){u->saved_index=u->selected;u->action_index=0;s_state.nav.page=FEISHU_PAGE_NETWORK_ACTIONS;}
            else if(u->selected==u->count){s_state.nav.page=FEISHU_PAGE_NETWORK_SCAN;u->selected=0;command=WORK_NETWORK_SCAN;}
            else portal=true;
        } else if(s_state.nav.page==FEISHU_PAGE_NETWORK_SCAN) {
            if(u->selected==u->scan_count)command=WORK_NETWORK_SCAN;
            else if(u->selected==u->scan_count+1)portal=true;
            else {
                memcpy(u->ssid,u->aps[u->selected].ssid,sizeof(u->ssid));memset(u->password,0,sizeof(u->password));
                u->secured=u->aps[u->selected].secured;u->editing_saved=false;
                u->keyboard_page=0;u->key=0;s_state.nav.page=FEISHU_PAGE_NETWORK_PASSWORD;
            }
        } else if(s_state.nav.page==FEISHU_PAGE_NETWORK_ACTIONS) {
            static const work_command_t actions[]={WORK_NETWORK_CONNECT,WORK_NETWORK_DISCONNECT,WORK_NETWORK_REMOVE,WORK_NETWORK_EDIT};
            command=actions[u->action_index];
        } else {
            int result=feishu_wifi_key_apply(&u->keyboard_page,&u->key,u->password);
            if(result==1) {
                if(!feishu_wifi_password_valid(u->password) || (!u->editing_saved && u->secured && !u->password[0])) {
                    s_state.status_error=true;feishu_utf8_copy(s_state.status,sizeof(s_state.status),"密码需8-63位或64位十六进制");
                } else if(u->editing_saved)command=WORK_NETWORK_PASSWORD_SAVE;
                else {u->saved_index=SIZE_MAX;command=WORK_NETWORK_CONNECT;}
            }
            if(result<0){s_state.status_error=true;feishu_utf8_copy(s_state.status,sizeof(s_state.status),"密码最多64位");}
        }
        if(command!=WORK_EXIT){u->busy=true;feishu_utf8_copy(s_state.status,sizeof(s_state.status),"正在处理，长按确定可返回");}
        if(portal){network_close_locked();s_state.loading=true;feishu_utf8_copy(s_state.status,sizeof(s_state.status),"正在进入手动配网…");}
    }
    xSemaphoreGive(s_state_lock);
    if(command!=WORK_EXIT)enqueue(command);
    if(portal)enqueue(WORK_RESET_WIFI);
    return true;
}
static bool network_long_key(bsp_btn_t btn)
{
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    bool handled=network_page(s_state.nav.page);
    if(handled && s_network_ui && !s_network_ui->busy && s_state.nav.page==FEISHU_PAGE_NETWORK_PASSWORD) {
        network_ui_t *u=s_network_ui;
        if(btn==BSP_BTN_UP){u->keyboard_page=(u->keyboard_page+1)%4;u->key=0;}
        if(btn==BSP_BTN_DOWN){size_t n=strlen(u->password);if(n)u->password[n-1]=0;}
    }
    xSemaphoreGive(s_state_lock);return handled;
}
static void network_fixed_row(size_t slot,int y,const char *label,bool selected)
{
    lv_obj_remove_flag(s_rows[slot],LV_OBJ_FLAG_HIDDEN);lv_obj_set_pos(s_rows[slot],10,y);lv_obj_set_size(s_rows[slot],220,23);
    style_list_row(slot,selected);lv_obj_set_style_radius(s_rows[slot],6,0);
    lv_obj_add_flag(s_row_avatar[slot],LV_OBJ_FLAG_HIDDEN);lv_obj_add_flag(s_row_subtext[slot],LV_OBJ_FLAG_HIDDEN);
    set_label_text(s_row_mark[slot],"");lv_obj_set_style_bg_opa(s_row_mark[slot],LV_OPA_TRANSP,0);
    lv_obj_set_style_pad_all(s_row_text[slot],0,0);lv_obj_set_style_bg_opa(s_row_text[slot],LV_OPA_TRANSP,0);
    lv_obj_set_style_border_width(s_row_text[slot],0,0);lv_obj_set_style_text_align(s_row_text[slot],LV_TEXT_ALIGN_LEFT,0);
    lv_obj_set_style_text_line_space(s_row_text[slot],1,0);lv_label_set_long_mode(s_row_text[slot],LV_LABEL_LONG_DOT);
    lv_obj_set_size(s_row_text[slot],196,18);set_label_text(s_row_text[slot],label);
    lv_obj_align(s_row_text[slot],LV_ALIGN_LEFT_MID,10,0);
}
static void network_render(char *body,size_t capacity,char *hint,size_t hint_size)
{
    network_ui_t *u=s_network_ui;set_label_text(s_title,"网络连接");
    if(!u){show_list_rows(false);snprintf(body,capacity,"正在打开网络设置…");return;}
    if(s_state.nav.page==FEISHU_PAGE_NETWORK_PASSWORD) {
        show_list_rows(false);char name[96];size_t n=strlen(u->password);
        feishu_text_preview(name,sizeof(name),u->ssid,false,26,1);
        snprintf(body,capacity,"%s\n密码 %u位  %s",name,(unsigned)n,u->password);
        if(!s_state.status[0])feishu_utf8_copy(s_state.status,sizeof(s_state.status),"ABC/123换键盘，DEL删除");
        lv_obj_invalidate(s_scr);
    } else if(s_state.nav.page==FEISHU_PAGE_NETWORK_ACTIONS) {
        set_label_text(s_title,"网络操作");const char *labels[]={"连接","断开","忘记","设置密码"};
        render_choice_rows(labels,4,u->action_index,false);
        for(size_t i=0;i<4;++i)lv_obj_set_y(s_rows[i],80+(int)i*34);
        lv_obj_remove_flag(s_content,LV_OBJ_FLAG_HIDDEN);
        if(u->saved_index<u->count)feishu_text_preview(body,capacity,u->names[u->saved_index],false,26,1);
    } else {
        const char *labels[FEISHU_WIFI_SAVED_MAX];char current[64],lines[FEISHU_WIFI_SCAN_MAX][48];
        size_t n=s_state.nav.page==FEISHU_PAGE_NETWORK ? u->count:u->scan_count;
        for(size_t i=0;i<n;++i) {
            if(s_state.nav.page==FEISHU_PAGE_NETWORK) {
                labels[i]=u->names[i];
                if(i==u->active && feishu_network_connected()){snprintf(current,sizeof(current),"已连接 %s",u->names[i]);labels[i]=current;}
            } else {snprintf(lines[i],sizeof(lines[i]),"%s  %ddB",u->aps[i].ssid,u->aps[i].rssi);labels[i]=lines[i];}
        }
        if(u->selected<n)u->list_cursor=u->selected;
        if(u->list_cursor>=n)u->list_cursor=0;
        size_t start=u->list_cursor>=4 ? u->list_cursor-3:0,visible=n-start;if(visible>4)visible=4;
        render_choice_rows(labels+start,visible,u->selected<n ? u->selected-start:visible,false);
        network_fixed_row(4,218,s_state.nav.page==FEISHU_PAGE_NETWORK ? "添加网络":"重新扫描",u->selected==n);
        network_fixed_row(5,243,"手动配网",u->selected==n+1);
        if(!n){lv_obj_remove_flag(s_content,LV_OBJ_FLAG_HIDDEN);snprintf(body,capacity,"%s",s_state.nav.page==FEISHU_PAGE_NETWORK ? (u->busy ? "正在读取已记忆网络…":"暂无已记忆网络"):(u->busy ? "正在扫描附近网络…":"未发现附近网络"));}
        lv_obj_invalidate(s_scr);
    }
    snprintf(hint,hint_size,"上下选择  确定打开  长按返回");
}
static void network_keyboard_draw(lv_event_t *event)
{
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    lv_layer_t *layer=lv_event_get_layer(event);
    if(s_network_ui && s_state.nav.page==FEISHU_PAGE_NETWORK_PASSWORD) {
        network_ui_t *u=s_network_ui;const lv_font_t *font=lv_obj_get_style_text_font(s_content,0);
        for(size_t i=0;i<feishu_wifi_key_count(u->keyboard_page);++i) {
            int x=11+(int)(i%7)*31,y=135+(int)(i/7)*25;lv_area_t area={x,y,x+28,y+22};
            lv_draw_rect_dsc_t rect;lv_draw_rect_dsc_init(&rect);rect.bg_color=lv_color_hex(FEISHU_SURFACE);rect.bg_opa=LV_OPA_COVER;rect.radius=4;
            rect.border_width=i==u->key ? 2:0;rect.border_color=lv_color_hex(FEISHU_FOCUS);rect.border_opa=LV_OPA_COVER;lv_draw_rect(layer,&rect,&area);
            lv_draw_label_dsc_t label;lv_draw_label_dsc_init(&label);label.font=font;label.color=lv_color_hex(FEISHU_TEXT);label.align=LV_TEXT_ALIGN_CENTER;
            const char *key=feishu_wifi_keys[u->keyboard_page][i];label.text=!strcmp(key," ") ? "空":key;
            if(!strcmp(key,"GO"))label.text=u->editing_saved ? "保存":"连接";
            if(!strcmp(key,"DEL"))label.text="删";
            area.y1+=3;lv_draw_label(layer,&label,&area);
        }
    }
    xSemaphoreGive(s_state_lock);
}

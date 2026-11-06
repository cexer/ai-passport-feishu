/* Worker context, opt-in only. Never prints SSIDs/passwords, sends messages,
 * unpairs or changes firmware layout. The original single-network record is
 * retained so a failed candidate can roll back to the previous firmware. */
static bool network_test_cancel(void *unused){(void)unused;return true;}
static void network_ui_autotest(void)
{
    wifi_config_t before={0},after={0};char names[FEISHU_WIFI_SAVED_MAX][33];size_t count=0,active=0;
    esp_err_t err=feishu_store_load_wifi(&before,sizeof(before));
    bool pass=err==ESP_OK && before.sta.ssid[0] && feishu_network_connected();
    if(pass)pass=feishu_store_save_wifi(&before,sizeof(before))==ESP_OK;
    if(pass)pass=feishu_store_list_wifi(names,&count,&active,sizeof(before))==ESP_OK && count>0 && active<count;
    ESP_LOGI(TAG,"NETWORK_UI_SMOKE phase=book pass=%u count=%u",(unsigned)pass,(unsigned)count);
    bool restored=false;
    if(pass) {
        err=feishu_store_update_wifi(active,&before,sizeof(before));
        size_t observed=SIZE_MAX,n=0;
        pass=err==ESP_OK && feishu_store_list_wifi(names,&n,&observed,sizeof(before))==ESP_OK && n==count && observed==active;
        ESP_LOGI(TAG,"NETWORK_UI_SMOKE phase=password_preserves_active pass=%u",(unsigned)pass);
    }
    if(pass) {
        err=feishu_network_switch(active,NULL,NULL,network_test_cancel,NULL,&restored);
        pass=err!=ESP_OK && feishu_network_connected() && feishu_store_load_wifi(&after,sizeof(after))==ESP_OK && !memcmp(&before,&after,sizeof(before));
        ESP_LOGI(TAG,"NETWORK_UI_SMOKE phase=cancel pass=%u",(unsigned)pass);
    }
    if(pass) {
        wifi_config_t edited=before;memset(edited.sta.password,0,sizeof(edited.sta.password));
        const char *wrong="wrong-edited-password-49e3";
        if(!strncmp((const char *)before.sta.password,wrong,64))wrong="other-edited-password-319c";
        memcpy(edited.sta.password,wrong,strlen(wrong));
        err=feishu_store_update_wifi(active,&edited,sizeof(edited));bool checked=err==ESP_OK && feishu_network_connected();
        if(checked){err=feishu_network_switch(active,NULL,NULL,NULL,NULL,&restored);checked=err!=ESP_OK && restored && feishu_network_connected();}
        esp_err_t restore=feishu_store_update_wifi(active,&before,sizeof(before));
        pass=checked && restore==ESP_OK;memset(&edited,0,sizeof(edited));
        ESP_LOGI(TAG,"NETWORK_UI_SMOKE phase=edited_password_rollback pass=%u",(unsigned)pass);
    }
    if(pass) {
        err=feishu_network_disconnect();vTaskDelay(pdMS_TO_TICKS(500));
        pass=err==ESP_OK && !feishu_network_connected() && feishu_network_user_offline() && feishu_network_start(1)==ESP_ERR_INVALID_STATE;
        ESP_LOGI(TAG,"NETWORK_UI_SMOKE phase=disconnect pass=%u",(unsigned)pass);
    }
    if(pass) {
        err=feishu_network_switch(active,NULL,NULL,NULL,NULL,&restored);
        pass=err==ESP_OK && feishu_network_connected() && !feishu_network_user_offline();
        ESP_LOGI(TAG,"NETWORK_UI_SMOKE phase=reconnect pass=%u",(unsigned)pass);
    }
    if(pass && count==1) {
        err=feishu_store_remove_wifi(active,sizeof(before));
        bool removed=err==ESP_OK;
        if(removed) {
            feishu_network_disconnect();
            size_t n=99,a=99;
            removed=feishu_store_load_wifi(&after,sizeof(after))==FEISHU_STORE_ERR_WIFI_EMPTY &&
                feishu_store_list_wifi(names,&n,&a,sizeof(before))==ESP_OK && n==0;
        }
        /* Restore from the RAM snapshot even when the empty-book check fails. */
        esp_err_t saved=feishu_store_save_wifi(&before,sizeof(before));
        bool recovered=false;
        if(saved==ESP_OK)recovered=feishu_network_switch(0,NULL,NULL,NULL,NULL,&restored)==ESP_OK;
        pass=removed && saved==ESP_OK && recovered;
        active=0;
        ESP_LOGI(TAG,"NETWORK_UI_SMOKE phase=forget_empty_readd pass=%u",(unsigned)pass);
    }
    if(pass) {
        char ssid[33]={0};memcpy(ssid,before.sta.ssid,32);
        const char *wrong="invalid-test-password-7b83a9";
        /* Avoid accidentally matching even an unusual pre-existing password. */
        if(!strncmp((const char *)before.sta.password,wrong,64))wrong="different-test-password-2c49";
        err=feishu_network_switch(SIZE_MAX,ssid,wrong,NULL,NULL,&restored);
        pass=err!=ESP_OK && restored && feishu_network_connected() &&
            feishu_store_load_wifi(&after,sizeof(after))==ESP_OK && !memcmp(&before,&after,sizeof(before));
        ESP_LOGI(TAG,"NETWORK_UI_SMOKE phase=wrong_password pass=%u restored=%u",(unsigned)pass,(unsigned)restored);
        memset(ssid,0,sizeof(ssid));
    }
    if(pass) {
        feishu_wifi_ap_t aps[FEISHU_WIFI_SCAN_MAX];size_t n=0;
        err=feishu_network_scan(aps,FEISHU_WIFI_SCAN_MAX,&n);pass=err==ESP_OK && n<=FEISHU_WIFI_SCAN_MAX;
        ESP_LOGI(TAG,"NETWORK_UI_SMOKE phase=scan pass=%u count=%u",(unsigned)pass,(unsigned)n);
        memset(aps,0,sizeof(aps));
    }
    if(pass) {
        err=refresh_conversations();pass=err==ESP_OK;
        ESP_LOGI(TAG,"NETWORK_UI_SMOKE phase=message_recovery pass=%u",(unsigned)pass);
    }
    if(!feishu_network_connected() && before.sta.ssid[0]) {
        bool repair=false;esp_err_t retry=feishu_network_switch(active,NULL,NULL,NULL,NULL,&repair);
        ESP_LOGI(TAG,"NETWORK_UI_SMOKE phase=cleanup_reconnect pass=%u",(unsigned)(retry==ESP_OK && feishu_network_connected()));
    }
    memset(&before,0,sizeof(before));memset(&after,0,sizeof(after));memset(names,0,sizeof(names));
    ESP_LOGI(TAG,"NETWORK_UI_SMOKE_DONE pass=%u no_sends=1 silent=1",(unsigned)pass);
}

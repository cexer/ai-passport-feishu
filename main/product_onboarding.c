#include "product_onboarding.h"
#include "feishu_qr_retry.h"

#include "feishu_binding.h"
#include "feishu_registration.h"
#include "esp_timer.h"
#include "nvs.h"
#include "feishu_network.h"
#include "feishu_provision.h"
#include "feishu_store.h"
#include "demo_radio.h"
#include "setup_portal.h"
#include "ui_pixel.h"
#include "feishu_theme.h"
#include "feishu_ui_chrome.h"
#include "bsp_battery.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    ONBOARD_CONNECTING = 0,
    ONBOARD_WIFI_SETUP,
    ONBOARD_APP_SETUP,
    ONBOARD_BINDING,
    ONBOARD_SUCCESS,
    ONBOARD_ERROR,
    ONBOARD_AUTH_LOADING,
} onboarding_page_t;

typedef struct {
    onboarding_page_t page;
    setup_portal_info_t portal;
    char qr_data[FEISHU_BIND_URL_MAX];
    char status[128];
    bool closing;
    bool cancel_auth;
    bool worker_finished;
    bool return_to_wifi;
    uint32_t revision;
    int battery_percent;
} onboarding_state_t;

static onboarding_state_t s_state;
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_worker_done;
static TaskHandle_t s_worker;
static lv_obj_t *s_screen;
static lv_obj_t *s_title;
static lv_obj_t *s_body;
static lv_obj_t *s_qr;
static lv_obj_t *s_step;
static lv_obj_t *s_footer_actions[3];
static lv_obj_t *s_notice;
static lv_obj_t *s_battery;
static lv_timer_t *s_timer;
static product_onboarding_complete_cb_t s_complete;
static bool s_completion_delivered;
static onboarding_page_t s_rendered_page = (onboarding_page_t)-1;
static uint32_t s_rendered_revision;
static char s_rendered_qr[FEISHU_BIND_URL_MAX];
static feishu_qr_retry_t s_qr_retry;
static const char *TAG="onboarding";

static void update_state(onboarding_page_t page, const char *status,
                         const char *qr_data)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool changed=s_state.page!=page;
    bool redraw=changed || strcmp(s_state.status,status ? status:"") ||
        strcmp(s_state.qr_data,qr_data ? qr_data:"");
    s_state.page = page;
    feishu_utf8_copy(s_state.status, sizeof(s_state.status), status);
    feishu_utf8_copy(s_state.qr_data, sizeof(s_state.qr_data),
                     qr_data == NULL ? "" : qr_data);
    if(redraw)++s_state.revision;
    xSemaphoreGive(s_lock);
    if(changed)ESP_LOGI(TAG,"phase=%u",(unsigned)page);
}

static bool closing(void)
{
    bool value;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    value = s_state.closing;
    xSemaphoreGive(s_lock);
    return value;
}

static bool cancelled(void)
{
    xSemaphoreTake(s_lock,portMAX_DELAY);bool value=s_state.cancel_auth;
    xSemaphoreGive(s_lock);return value;
}
static bool returning_to_wifi(void)
{
    xSemaphoreTake(s_lock,portMAX_DELAY);bool value=s_state.return_to_wifi;
    xSemaphoreGive(s_lock);return value;
}
static bool auth_wait(unsigned milliseconds)
{
    static TickType_t sampled;
    for(unsigned elapsed=0;elapsed<milliseconds;elapsed+=100) {
        if(xTaskGetTickCount()-sampled>=pdMS_TO_TICKS(15000)) {
            int percent=bsp_battery_soc();sampled=xTaskGetTickCount();
            xSemaphoreTake(s_lock,portMAX_DELAY);s_state.battery_percent=percent;xSemaphoreGive(s_lock);
        }
        if(closing() || cancelled())return false;
        feishu_provision_commit_pending();
        vTaskDelay(pdMS_TO_TICKS(milliseconds-elapsed<100 ? milliseconds-elapsed:100));
    }
    return !closing() && !cancelled();
}
static bool auth_cancel(void *context) { (void)context;return closing() || cancelled(); }
static esp_err_t select_owner_app(feishu_credentials_t *credentials,const char *preferred)
{
    feishu_register_result_t *request=calloc(1,sizeof(*request));
    feishu_register_result_t *result=calloc(1,sizeof(*result));
    esp_err_t err=ESP_ERR_NO_MEM;
    bool usb_started=feishu_provision_start_candidate(credentials)==ESP_OK;
    if(!request || !result)goto done;
    while(!closing() && !cancelled()) {
        if(usb_started && feishu_provision_credentials_received()) {err=ESP_OK;goto done;}
        update_state(ONBOARD_AUTH_LOADING,"正在获取飞书BOT入口...\n长按确定返回",NULL);
        err=feishu_registration_begin_cancelable(request,preferred,auth_cancel,NULL);
        if(err!=ESP_OK) {
            if(cancelled() || closing())break;
            update_state(ONBOARD_AUTH_LOADING,"BOT入口暂时不可用，正在重试\n长按确定返回",NULL);
            if(!auth_wait(5000))break;
            if(usb_started && feishu_provision_credentials_received()) {err=ESP_OK;goto done;}
            continue;
        }
        update_state(ONBOARD_APP_SETUP,"飞书扫码选择或新建BOT\n长按确定取消",request->verification_url);
        printf("FAP-SETUP/1 APP_AUTH %s\n",request->verification_url);
        int64_t deadline=esp_timer_get_time()+(int64_t)request->expires_in*1000000;
        unsigned interval=request->interval;
        while(esp_timer_get_time()<deadline && auth_wait(interval*1000)) {
            if(usb_started && feishu_provision_credentials_received()) {err=ESP_OK;goto done;}
            err=feishu_registration_poll_cancelable(request,result,auth_cancel,NULL);
            if(closing() || cancelled())break;
            if(err!=ESP_OK)continue; // Keep original config and the current QR.
            if(result->status==FEISHU_REGISTER_COMPLETE) {
                memset(credentials,0,sizeof(*credentials));
                memcpy(credentials->app_id,result->app_id,sizeof(credentials->app_id));
                memcpy(credentials->app_secret,result->app_secret,sizeof(credentials->app_secret));
                err=ESP_OK;goto done;
            }
            if(result->status==FEISHU_REGISTER_DENIED) {err=ESP_ERR_INVALID_STATE;goto done;}
            if(result->status==FEISHU_REGISTER_UNSUPPORTED) {err=ESP_ERR_NOT_SUPPORTED;goto done;}
            if(result->status==FEISHU_REGISTER_EXPIRED)break;
            if(result->status==FEISHU_REGISTER_FAILED) {err=ESP_ERR_INVALID_RESPONSE;goto done;}
            if(result->status==FEISHU_REGISTER_SLOW && interval<60)interval=interval>55?60:interval+5;
        }
    }
    err=ESP_ERR_INVALID_STATE;
done:
    if(usb_started)feishu_provision_stop();
    if(request) {memset(request,0,sizeof(*request));free(request);}
    if(result) {memset(result,0,sizeof(*result));free(result);}
    return err;
}
static void worker_task(void *argument)
{
    (void)argument;
    ESP_LOGI(TAG,"worker started");
    esp_err_t err;
    feishu_credentials_t *credentials=NULL;
    bool bound=false,force_wifi=false;
    const char *wifi_notice="";
    unsigned auth_request=FEISHU_AUTH_NONE;
    err=demo_radio_nvs_prepare();
    if(err!=ESP_OK) {update_state(ONBOARD_ERROR,"无法读取配置，未清除已有数据",NULL);goto finished;}
    err=feishu_store_get_auth_request(&auth_request);
    if(err!=ESP_OK) {update_state(ONBOARD_ERROR,"无法读取授权状态，原配置保留",NULL);goto finished;}
    force_wifi=feishu_store_wifi_setup_required();
    credentials=calloc(1,sizeof(*credentials));
    if(!credentials) {update_state(ONBOARD_ERROR,"内存不足，请重试",NULL);goto finished;}
    err=feishu_store_load_credentials(credentials);
    bound=err==ESP_OK;
    if(err!=ESP_OK && err!=ESP_ERR_NVS_NOT_FOUND) {
        update_state(ONBOARD_ERROR,"无法读取绑定，原配置保留",NULL);goto finished;
    }
    ESP_LOGI(TAG,"configuration checked: bound=%u wifi_setup=%u auth_request=%u",(unsigned)bound,(unsigned)force_wifi,auth_request);
    memset(credentials,0,sizeof(*credentials));free(credentials);credentials=NULL;
    if(bound && !force_wifi && auth_request==FEISHU_AUTH_NONE) {
        update_state(ONBOARD_SUCCESS,"正在打开飞书",NULL);goto finished;
    }
network_setup:
    for (;;) {
        update_state(ONBOARD_CONNECTING, "正在连接 Wi-Fi...\n长按确定返回配网", NULL);
        err = force_wifi ? ESP_ERR_INVALID_STATE : feishu_network_start_cancelable(12000,auth_cancel,NULL);
        ESP_LOGI(TAG,"network result=%s",esp_err_to_name(err));
        if(cancelled())goto cancelled;
        if (err == ESP_OK) break;
        if (closing()) goto finished;
        if(cancelled())goto cancelled;

        update_state(ONBOARD_CONNECTING, "正在启动配网热点...", NULL);
        err = setup_portal_start(&s_state.portal);
        if (err != ESP_OK) {
            ESP_LOGE(TAG,"portal start failed: %s",esp_err_to_name(err));
            update_state(ONBOARD_ERROR, "无法启动配网，请重启设备", NULL);
            goto finished;
        }
        if(!force_wifi)wifi_notice=err==ESP_ERR_TIMEOUT ? "连接超时，请重新配网":"连接未成功，请重新配网";
        update_state(ONBOARD_WIFI_SETUP, wifi_notice, NULL);
        bool wifi_usb_started = feishu_provision_start_wifi() == ESP_OK;
        while (!closing() && !cancelled() && !setup_portal_credentials_received() &&
               !(wifi_usb_started && feishu_provision_credentials_received())) {
            feishu_provision_commit_pending();
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        feishu_provision_stop();
        if (closing()) {
            setup_portal_stop();
            goto finished;
        }
        vTaskDelay(pdMS_TO_TICKS(800));
        setup_portal_stop();
        if(cancelled())goto cancelled;
        force_wifi=false;
        wifi_notice="";
        err=feishu_store_require_wifi_setup(false);
        if(err!=ESP_OK){update_state(ONBOARD_ERROR,"无法保存配网状态，请重试",NULL);goto finished;}
    }

    if(bound && auth_request==FEISHU_AUTH_NONE) {
        update_state(ONBOARD_SUCCESS,"飞书已绑定",NULL);goto finished;
    }
    credentials=calloc(1,sizeof(*credentials));
    if(!credentials) {update_state(ONBOARD_ERROR,"内存不足，请重试",NULL);goto finished;}
configure_owner_app:
    err=feishu_store_load_app_credentials(credentials);
    if(err!=ESP_OK && err!=ESP_ERR_NVS_NOT_FOUND) {
        update_state(ONBOARD_ERROR,"无法读取BOT配置，未覆盖原数据",NULL);goto finished;
    }
    if(err==ESP_ERR_NVS_NOT_FOUND || auth_request==FEISHU_AUTH_CHANGE_APP || auth_request==FEISHU_AUTH_REPAIR_APP) {
        char preferred[FEISHU_APP_ID_MAX]={0};
        if(auth_request==FEISHU_AUTH_REPAIR_APP && err==ESP_OK)
            memcpy(preferred,credentials->app_id,sizeof(preferred));
        err=select_owner_app(credentials,preferred);
        if(err==ESP_ERR_INVALID_STATE)goto cancelled;
        if(err!=ESP_OK) {update_state(ONBOARD_ERROR,"无法完成BOT选择，原配置保留",NULL);goto finished;}
    }
    while(!closing() && !cancelled()) {
        feishu_binding_request_t request={0};
        err=feishu_binding_begin_with_app(&request,credentials);
        if(err==ESP_ERR_INVALID_ARG) {
            // Explicit invalid_client only: repair without clearing the old app.
            auth_request=FEISHU_AUTH_REPAIR_APP;
            err=feishu_store_request_auth(auth_request);
            if(err!=ESP_OK) {update_state(ONBOARD_ERROR,"无法保存修复请求，原配置保留",NULL);goto finished;}
            goto configure_owner_app;
        }
        if(err!=ESP_OK) {
            update_state(ONBOARD_AUTH_LOADING,"暂时无法发起授权，正在重试\n长按确定取消",NULL);
            if(!auth_wait(5000))break;
            continue;
        }
        update_state(ONBOARD_BINDING,"飞书扫码确认用户数据授权\n长按确定取消",request.verification_url);
        printf("FAP-SETUP/1 AUTH %s\n",request.verification_url);
        int64_t deadline=esp_timer_get_time()+(int64_t)request.expires_in*1000000;
        unsigned interval=request.interval>60?60:request.interval;
        while(esp_timer_get_time()<deadline && auth_wait(interval*1000)) {
            feishu_binding_status_t status;
            err=feishu_binding_poll_candidate(&request,&status,credentials,auth_cancel,NULL);
            // An atomic save already in progress wins a simultaneous cancel.
            if(err==ESP_OK && status==FEISHU_BINDING_COMPLETE) {
                err=feishu_store_request_auth(FEISHU_AUTH_NONE);
                if(err!=ESP_OK) {update_state(ONBOARD_ERROR,"授权已保存，结束向导失败，请重试",NULL);goto finished;}
                update_state(ONBOARD_SUCCESS,"绑定成功",NULL);goto finished;
            }
            if(closing() || cancelled())break;
            if(err==ESP_ERR_INVALID_ARG) {
                auth_request=FEISHU_AUTH_REPAIR_APP;
                if(feishu_store_request_auth(auth_request)!=ESP_OK) {
                    update_state(ONBOARD_ERROR,"无法保存修复请求，原配置保留",NULL);goto finished;
                }
                goto configure_owner_app;
            }
            if(err!=ESP_OK)continue;
            if(status==FEISHU_BINDING_DENIED)goto cancelled;
            if(status==FEISHU_BINDING_EXPIRED)break;
            if(status==FEISHU_BINDING_SLOW_DOWN && interval<60)interval=interval>58?60:interval+2;
        }
    }
    if(cancelled())goto cancelled;
    goto finished;
cancelled:
    if(closing())goto finished;
    if(!bound || returning_to_wifi()) {
        if(credentials){memset(credentials,0,sizeof(*credentials));free(credentials);credentials=NULL;}
        feishu_network_stop();
        err=feishu_store_require_wifi_setup(true);
        if(err==ESP_OK)err=feishu_store_request_auth(FEISHU_AUTH_NONE);
        if(err!=ESP_OK){update_state(ONBOARD_ERROR,"无法返回配网，原配置保留",NULL);goto finished;}
        auth_request=FEISHU_AUTH_NONE;force_wifi=true;wifi_notice="已取消连接，重新配网";
        xSemaphoreTake(s_lock,portMAX_DELAY);s_state.cancel_auth=false;s_state.return_to_wifi=false;xSemaphoreGive(s_lock);
        goto network_setup;
    }
    // A candidate never replaced the old binding before user authorization.
    err=feishu_store_request_auth(FEISHU_AUTH_NONE);
    if(err!=ESP_OK) {update_state(ONBOARD_ERROR,"取消未保存，原配置保留，请重试",NULL);goto finished;}
    if(bound)update_state(ONBOARD_SUCCESS,"已取消，返回原账号",NULL);
    else update_state(ONBOARD_ERROR,"授权已取消，按确定重试",NULL);
finished:
    if(credentials) {memset(credentials,0,sizeof(*credentials));free(credentials);}
    xSemaphoreTake(s_lock,portMAX_DELAY);s_state.worker_finished=true;xSemaphoreGive(s_lock);
    xSemaphoreGive(s_worker_done);vTaskDelete(NULL);
}

static void render(lv_timer_t *timer)
{
    (void)timer;
    onboarding_state_t snapshot;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(&snapshot, &s_state, sizeof(snapshot));
    xSemaphoreGive(s_lock);

    feishu_ui_battery(s_battery,snapshot.battery_percent);
    if (snapshot.revision != s_rendered_revision) {
        s_rendered_revision=snapshot.revision;s_rendered_page=snapshot.page;
        s_rendered_qr[0]='\0';feishu_qr_retry_reset(&s_qr_retry);
        lv_obj_add_flag(s_qr,LV_OBJ_FLAG_HIDDEN);
        bool app=snapshot.page==ONBOARD_APP_SETUP;
        bool auth=snapshot.page==ONBOARD_BINDING;
        bool qr=app || auth;
        lv_obj_set_style_text_color(s_body,lv_color_hex(FEISHU_TEXT),0);
        lv_obj_set_style_text_color(s_notice,lv_color_hex(snapshot.page==ONBOARD_ERROR ? FEISHU_ERROR:FEISHU_MUTED),0);
        lv_label_set_text(s_notice,qr ? snapshot.status:snapshot.page==ONBOARD_ERROR ? "操作未完成":"");
        feishu_ui_actions(s_footer_actions,snapshot.page==ONBOARD_ERROR ? "确定重试  长按返回":"长按确定返回");
        lv_obj_set_pos(s_body,12,qr ? 230:46);
        if(qr) {
            lv_obj_remove_flag(s_step,LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(s_step,app ? "第2/3步 | 选择BOT":"第3/3步 | BOT授权");
            lv_obj_set_style_text_color(s_step,lv_color_hex(app ? 0x91B4DF:FEISHU_ACCENT),0);
            lv_qrcode_set_dark_color(s_qr,lv_color_hex(app ? 0x245C96:0x146341));
        } else lv_obj_add_flag(s_step,LV_OBJ_FLAG_HIDDEN);
        if(snapshot.page==ONBOARD_CONNECTING) {
            lv_label_set_text(s_title,"连接 Wi-Fi");lv_label_set_text(s_body,snapshot.status);
        } else if(snapshot.page==ONBOARD_AUTH_LOADING) {
            lv_label_set_text(s_title,"连接飞书服务");lv_label_set_text(s_body,snapshot.status);
        } else if(snapshot.page==ONBOARD_WIFI_SETUP) {
            lv_label_set_text(s_title,"连接设备热点");
            lv_label_set_text_fmt(s_body,"手机打开 Wi-Fi 设置\n\n%s\n无需密码，直接连接\n\n连接后打开配网页\nhttp://%s\n\n提示无互联网时请保持连接",snapshot.portal.ssid,snapshot.portal.address);
            lv_label_set_text(s_notice,snapshot.status);
        } else if(app) {
            lv_label_set_text(s_title,"选择飞书BOT");
            lv_label_set_text(s_body,"飞书扫一扫，选择或新建BOT\n完成后进入BOT授权");
        } else if(auth) {
            lv_label_set_text(s_title,"BOT授权");
            lv_label_set_text(s_body,"飞书扫一扫，授权BOT\n完成后以你的身份收发消息");
        } else if(snapshot.page==ONBOARD_SUCCESS) {
            lv_label_set_text(s_title,"设置完成");lv_label_set_text(s_body,"飞书绑定成功\n\n正在打开会话…");
        } else {
            lv_label_set_text(s_title,"连接暂不可用");lv_label_set_text(s_body,snapshot.status);
        }
    }
    if ((snapshot.page == ONBOARD_APP_SETUP ||
         snapshot.page == ONBOARD_BINDING) && snapshot.qr_data[0] != '\0' &&
        strcmp(snapshot.qr_data, s_rendered_qr) != 0 &&
        feishu_qr_retry_due(&s_qr_retry,lv_tick_get())) {
        bool recovering=s_qr_retry.failed;
        if(!lv_canvas_get_draw_buf(s_qr))lv_qrcode_set_size(s_qr,156);
        bool okay=lv_qrcode_update(s_qr,snapshot.qr_data,strlen(snapshot.qr_data))==LV_RESULT_OK;
        feishu_qr_retry_result(&s_qr_retry,lv_tick_get(),okay);
        if(okay) {
            feishu_utf8_copy(s_rendered_qr,sizeof(s_rendered_qr),snapshot.qr_data);
            lv_obj_remove_flag(s_qr,LV_OBJ_FLAG_HIDDEN);
            if(recovering) {
                lv_obj_set_style_text_color(s_body,lv_color_hex(FEISHU_TEXT),0);
                lv_label_set_text(s_body,snapshot.page==ONBOARD_APP_SETUP ?
                    "飞书扫一扫，选择或新建BOT\n完成后进入BOT授权":
                    "飞书扫一扫，授权BOT\n完成后以你的身份收发消息");
            }
        } else {
            lv_obj_add_flag(s_qr,LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_text_color(s_body,lv_color_hex(FEISHU_ERROR),0);
            lv_label_set_text(s_body,"二维码绘制失败，正在重试\n长按确定可返回");
            ESP_LOGW(TAG,"QR draw failed: phase=%u bytes=%u",(unsigned)snapshot.page,(unsigned)strlen(snapshot.qr_data));
        }
    }
    if (snapshot.page == ONBOARD_SUCCESS && snapshot.worker_finished &&
        !s_completion_delivered && s_complete != NULL) {
        s_completion_delivered = true;
        s_complete();
    }
}

void product_onboarding_enter(product_onboarding_complete_cb_t callback,int battery_percent)
{
    memset(&s_state, 0, sizeof(s_state));
    s_state.page = ONBOARD_CONNECTING;
    s_state.battery_percent=battery_percent;
    s_complete = callback;
    s_completion_delivered = false;
    s_rendered_page = (onboarding_page_t)-1;
    s_rendered_revision=UINT32_MAX;
    s_rendered_qr[0] = '\0';
    feishu_qr_retry_reset(&s_qr_retry);
    s_lock = xSemaphoreCreateMutex();
    s_worker_done = xSemaphoreCreateBinary();

    s_screen = lv_obj_create(NULL);
    lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(FEISHU_BG), 0);
    lv_obj_set_style_bg_opa(s_screen,LV_OPA_COVER,0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    feishu_ui_header(s_screen,&s_title,&s_battery);
    s_qr = lv_qrcode_create(s_screen);
    lv_qrcode_set_size(s_qr, 156);
    lv_qrcode_set_dark_color(s_qr, lv_color_hex(FEISHU_BG));
    lv_qrcode_set_light_color(s_qr, lv_color_hex(FEISHU_QR_LIGHT));
    lv_qrcode_set_quiet_zone(s_qr, true);
    lv_obj_set_pos(s_qr,42,68);
    lv_obj_add_flag(s_qr, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_border_width(s_qr,0,0);
    lv_obj_set_style_border_color(s_qr,lv_color_hex(FEISHU_QR_LIGHT),0);
    lv_obj_set_style_radius(s_qr,8,0);
    s_step=lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_step,&lv_font_ai_passport_14,0);
    lv_obj_align(s_step,LV_ALIGN_TOP_MID,0,46);
    feishu_ui_footer(s_screen,&s_notice,s_footer_actions);
    s_body = lv_label_create(s_screen);
    lv_obj_set_width(s_body, 216);
    lv_obj_set_pos(s_body,12,46);
    lv_obj_set_style_text_font(s_body, &lv_font_ai_passport_14, 0);
    lv_obj_set_style_text_align(s_body, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_body, LV_LABEL_LONG_WRAP);
    lv_screen_load(s_screen);

    if (s_lock == NULL || s_worker_done == NULL) {
        lv_label_set_text(s_body, "内存不足，请重启设备");
        return;
    }
    s_timer = lv_timer_create(render, 100, NULL);
    if (xTaskCreate(worker_task, "onboarding", 10240, NULL, 4,
                    &s_worker) != pdPASS) {
        update_state(ONBOARD_ERROR, "内存不足，请重启设备", NULL);
    }
}

void product_onboarding_exit(void)
{
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_state.closing = true;
        xSemaphoreGive(s_lock);
    }
    if (s_worker != NULL && s_worker_done != NULL) {
        /* A bounded network call may outlive three seconds. Never release
         * its lock/state or stop its portal while the worker still uses it. */
        xSemaphoreTake(s_worker_done, portMAX_DELAY);
        s_worker = NULL;
    }
    setup_portal_stop();
    feishu_provision_stop();
    if (s_timer != NULL) { lv_timer_delete(s_timer); s_timer = NULL; }
    if (s_screen != NULL) { lv_obj_delete(s_screen); s_screen = NULL; }
    if (s_worker_done != NULL) {
        vSemaphoreDelete(s_worker_done);
        s_worker_done = NULL;
    }
    if (s_lock != NULL) { vSemaphoreDelete(s_lock); s_lock = NULL; }
    s_complete = NULL;
}

void product_onboarding_key(bsp_btn_t button,bsp_btn_ev_t event)
{
    if(button!=BSP_BTN_OK || s_lock==NULL)return;
    xSemaphoreTake(s_lock,portMAX_DELAY);
    onboarding_page_t page=s_state.page;
    if(event==BSP_BTN_LONG && !s_state.worker_finished) {
        s_state.cancel_auth=true;
        if(page==ONBOARD_CONNECTING)s_state.return_to_wifi=true;
        feishu_utf8_copy(s_state.status,sizeof(s_state.status),"正在取消，请稍候...");
        ++s_state.revision;
    }
    xSemaphoreGive(s_lock);
    if(event==BSP_BTN_CLICK && page==ONBOARD_ERROR)esp_restart();
}

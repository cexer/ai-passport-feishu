// Product entry point: onboarding and the Feishu terminal share one key router.
#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "demo.h"
#include "product_onboarding.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "lvgl.h"
#include "ui_pixel.h"
#include "feishu_theme.h"
#include <stdatomic.h>

static const char *TAG = "main";
static bool s_onboarding = true;
typedef struct {uint8_t button,event;} key_event_t;
static StaticQueue_t s_key_storage;
static uint8_t s_key_bytes[16*sizeof(key_event_t)];
static QueueHandle_t s_keys;
static atomic_bool s_key_overflow;

static void onboarding_complete(void)
{
    product_onboarding_exit();
#ifdef FEISHU_NETWORK_DIAGNOSTIC
    extern void feishu_network_probe_start(void);
    feishu_network_probe_start();
#else
    demo_feishu_enter();
#endif
    s_onboarding = false;
    xQueueReset(s_keys);
}

/* A scanline draw can hold LVGL longer than the old 500 ms key timeout.
 * Keep button sampling non-blocking and deliver queued events from LVGL,
 * where page creation/deletion and routing have the same lifetime. */
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    if(ev!=BSP_BTN_CLICK && ev!=BSP_BTN_LONG && !((btn==BSP_BTN_DOWN || btn==BSP_BTN_UP) && ev==BSP_BTN_DOUBLE))return;
    key_event_t key={(uint8_t)btn,(uint8_t)ev};
    if(s_keys && xQueueSend(s_keys,&key,0)!=pdTRUE)atomic_store(&s_key_overflow,true);
}
static void dispatch_key(lv_timer_t *timer)
{
    (void)timer;
    if(atomic_exchange(&s_key_overflow,false)) {
        /* Overflow must not replay an old confirmation in a later context. */
        xQueueReset(s_keys);ESP_LOGW(TAG,"input queue overflow; pending buttons cleared");return;
    }
    key_event_t key;if(xQueueReceive(s_keys,&key,0)!=pdTRUE)return;
    bsp_btn_t btn=(bsp_btn_t)key.button;bsp_btn_ev_t ev=(bsp_btn_ev_t)key.event;
#ifdef FEISHU_NETWORK_DIAGNOSTIC
    if(!s_onboarding)return;
#endif
    if (s_onboarding) {
        product_onboarding_key(btn, ev);
    } else if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
        demo_feishu_back();
    } else {
        demo_feishu_key(btn, ev);
    }
    /* Confirm/back/long-press can change the visible interaction context. */
    if(btn==BSP_BTN_OK || ev==BSP_BTN_LONG)xQueueReset(s_keys);
}

esp_err_t feishu_font_init(void);

static void log_font_grayscale(void)
{
    lv_font_glyph_dsc_t glyph={0};
    if(!lv_font_get_glyph_dsc(&lv_font_ai_passport_14,&glyph,0x6587,0))return;
    lv_draw_buf_t *buf=lv_draw_buf_create(glyph.box_w,glyph.box_h,LV_COLOR_FORMAT_A8,LV_STRIDE_AUTO);
    if(!buf)return;
    if(lv_font_get_glyph_bitmap(&glyph,buf)) {
        bool seen[256]={false};unsigned levels=0;
        for(unsigned y=0;y<glyph.box_h;++y)for(unsigned x=0;x<glyph.box_w;++x)seen[buf->data[y*buf->header.stride+x]]=true;
        for(unsigned i=0;i<256;++i)if(seen[i])++levels;
        ESP_LOGI(TAG,"FONT_GRAY levels=%u antialias=%u color_depth=%u",levels,
            (unsigned)lv_display_get_antialiasing(lv_display_get_default()),(unsigned)LV_COLOR_DEPTH);
    }
    lv_draw_buf_destroy(buf);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Feishu terminal starting");
    bsp_i2c_init();
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "Display initialization failed");
        return;
    }
    if(bsp_lvgl_lock(1000)) {
        lv_obj_set_style_bg_color(lv_screen_active(),lv_color_hex(FEISHU_BG),0);
        lv_obj_set_style_bg_opa(lv_screen_active(),LV_OPA_COVER,0);
        lv_obj_set_style_text_color(lv_screen_active(),lv_color_hex(FEISHU_TEXT),0);
        bsp_lvgl_unlock();
    }
    bsp_display_backlight(70);
    s_keys=xQueueCreateStatic(16,sizeof(key_event_t),s_key_bytes,&s_key_storage);
    bool buttons_ok = bsp_button_init(on_key, NULL) == ESP_OK;
    // Audio DMA is acquired only while recording, not while browsing or TLS.
    bool battery_ok = bsp_battery_init() == ESP_OK;
    int battery_percent=battery_ok ? bsp_battery_soc():-1;
    if (bsp_lvgl_lock(1000)) {
        if(!lv_timer_create(dispatch_key,20,NULL)) {
            buttons_ok=false;ESP_LOGE(TAG,"Input dispatch initialization failed");
        }
        esp_err_t font_error=feishu_font_init();
        if(font_error!=ESP_OK) {
            ESP_LOGE(TAG,"Font asset unavailable: %s; settings preserved",esp_err_to_name(font_error));
            lv_obj_t *error=lv_label_create(lv_screen_active());
            lv_obj_set_style_text_font(error,&lv_font_montserrat_14,0);
            lv_label_set_text(error,"Font asset missing/corrupt\nReinstall matching firmware\nSettings are preserved");
            lv_obj_center(error);bsp_lvgl_unlock();return;
        }
        log_font_grayscale();
        product_onboarding_enter(onboarding_complete,battery_percent);
        bsp_lvgl_unlock();
    }
    ESP_LOGI(TAG, "Ready: Button=%d Audio=on-demand Battery=%d",
             buttons_ok, battery_ok);
#ifdef FEISHU_STORAGE_AUTOTEST
    extern void feishu_storage_autotest_start(void);
    feishu_storage_autotest_start();
#endif
}

/* Silent physical-device benchmark: production codec/cache, synthetic data,
 * no Wi-Fi, display, microphone or speaker initialization. */
#include "feishu_image.h"
#include "feishu_cache.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "mbedtls/sha256.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>

extern const unsigned char fixture_start[] asm("_binary_media_fixture_jpg_start");
extern const unsigned char fixture_end[] asm("_binary_media_fixture_jpg_end");

static bool source(const char *resource,const void *data,size_t size)
{
    const char message[]="local-probe";uint8_t digest[32];char name[64];
    mbedtls_sha256_context hash;mbedtls_sha256_init(&hash);
    int rc=mbedtls_sha256_starts(&hash,0);
    if(!rc)rc=mbedtls_sha256_update(&hash,(const uint8_t *)message,sizeof(message));
    if(!rc)rc=mbedtls_sha256_update(&hash,(const uint8_t *)resource,strlen(resource)+1);
    if(!rc)rc=mbedtls_sha256_finish(&hash,digest);
    mbedtls_sha256_free(&hash);if(rc)return false;
    memcpy(name,"image_src/",10);
    for(unsigned i=0;i<16;++i)snprintf(name+10+i*2,3,"%02x",digest[i]);
    feishu_data_writer_t writer={0};
    esp_err_t err=feishu_cache_blob_begin(FEISHU_DATA_IMAGE,&writer);
    if(err!=ESP_OK)printf("MEDIA_PROBE source_begin_failed rc=%d\n",(int)err);
    const uint8_t *bytes=data;
    for(size_t at=0;err==ESP_OK && at<size;) {
        size_t n=size-at;if(n>1024)n=1024;
        err=feishu_cache_blob_append(&writer,bytes+at,n,false);at+=n;
        if(err!=ESP_OK)printf("MEDIA_PROBE source_append_failed rc=%d\n",(int)err);
    }
    int64_t pin=0;
    if(err==ESP_OK)err=feishu_cache_blob_commit(&writer,name,NULL,NULL,&pin);
    if(err!=ESP_OK)printf("MEDIA_PROBE source_failed rc=%d\n",(int)err);
    feishu_cache_text_abort(&writer);feishu_cache_unpin(&pin);
    return err==ESP_OK;
}

static bool preview(const char *step,const char *resource,feishu_image_view_t view,
                    bool expected_success,unsigned x,unsigned y,unsigned channel)
{
    feishu_prepared_image_t image={0};int status=0;
    size_t before=heap_caps_get_free_size(MALLOC_CAP_8BIT);
    bool monitoring=heap_caps_monitor_local_minimum_free_size_start()==ESP_OK;
    int64_t started=esp_timer_get_time();
    esp_err_t err=feishu_image_prepare("unused","","local-probe",resource,&view,false,NULL,NULL,&image,&status);
    bool pass=expected_success ? err==ESP_OK:err!=ESP_OK;
    if(pass && expected_success) {
        uint8_t pixel[2];size_t got=0;
        uint64_t offset=sizeof(feishu_image_record_t)+sizeof(lv_image_header_t)+((uint64_t)y*image.info.width+x)*2;
        pass=x<image.info.width && y<image.info.height &&
            feishu_cache_blob_read(image.name,image.id,offset,pixel,sizeof(pixel),&got)==ESP_OK && got==2;
        if(pass) {
            unsigned rgb565=pixel[0]|(unsigned)pixel[1]<<8;
            unsigned rgb[3]={((rgb565>>11)&31)*8,((rgb565>>5)&63)*4,(rgb565&31)*8};
            pass=rgb[channel]>180;
            for(unsigned i=0;i<3;++i)if(i!=channel && rgb[i]>70)pass=false;
        }
    }
    size_t minimum=heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    if(monitoring)heap_caps_monitor_local_minimum_free_size_stop();
    pass=pass && monitoring;
    printf("MEDIA_PROBE step=%s pass=%u rc=%d us=%lld cached=%u before=%u free=%u minimum=%u largest=%u workspace=%u\n",
        step,(unsigned)pass,(int)err,(long long)(esp_timer_get_time()-started),(unsigned)image.cached,
        (unsigned)before,(unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),(unsigned)minimum,
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),(unsigned)image.info.workspace_peak);
    feishu_cache_unpin(&image.pin);return pass;
}

void app_main(void)
{
    printf("MEDIA_PROBE silent=1 network=0 display=0 audio=0\n");
    bool pass=feishu_cache_init()==ESP_OK;
    esp_err_t mount=pass ? feishu_cache_mount():ESP_ERR_NO_MEM;
    printf("MEDIA_PROBE mount_rc=%d\n",(int)mount);pass=pass && mount==ESP_OK;
    /* A fresh synthetic scope prevents a reset from turning cold-decode
     * measurements into hits on a previous run's previews. */
    char owner[32];snprintf(owner,sizeof(owner),"local_%08lx",(unsigned long)esp_random());
    feishu_cache_set_scope("media_probe",owner);
    if(pass)pass=source("quadrants",fixture_start,(size_t)(fixture_end-fixture_start));
    if(pass)pass=preview("overview","quadrants",(feishu_image_view_t){0,-1,-1},true,53,40,0);
    if(pass)pass=preview("zoom","quadrants",(feishu_image_view_t){1,512,384},true,54,132,2);
    if(pass)pass=preview("pan","quadrants",(feishu_image_view_t){1,512,267},true,54,132,0);
    if(pass)pass=preview("cache_hit","quadrants",(feishu_image_view_t){1,512,267},true,54,132,0);
    if(pass)pass=source("invalid","not a jpeg",10);
    if(pass)pass=preview("invalid","invalid",(feishu_image_view_t){0,-1,-1},false,0,0,0);
    if(pass)pass=preview("recovery","quadrants",(feishu_image_view_t){1,256,267},true,162,72,0);
    feishu_cache_close();feishu_cache_unmount();
    printf("MEDIA_PROBE DONE pass=%u\n",(unsigned)pass);
}

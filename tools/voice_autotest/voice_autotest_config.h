#pragma once
/* Opt-in diagnostic firmware only. The ordinary product excludes this file,
 * fixture data and driver. Real I2S capture still runs before fixture overlay. */
#include "voice_autotest_fixture.h"
#include "feishu_cache.h"
#include "feishu_http.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
static struct {
    bool active,repeat;
    const uint8_t *pcm;
    size_t size;
    unsigned stop_ms,cancel_ms,fail_ms,delay_ms,reads,prefix_ms;
    int peak;
    unsigned samples,zeros,clipped;
    int64_t sum;
    uint64_t power;
    volatile bool *stop,*cancel;
} voice_probe;
static esp_err_t voice_probe_capture(int16_t *pcm,size_t bytes,unsigned elapsed)
{
    if(!voice_probe.active)return ESP_OK;
    ++voice_probe.reads;
    for(size_t i=0;i<bytes/2;++i) {
        int n=pcm[i];++voice_probe.samples;voice_probe.sum+=n;voice_probe.power+=(int64_t)n*n;
        if(!n)++voice_probe.zeros;
        if(n==32767 || n==-32768)++voice_probe.clipped;
        if(n<0)n=-n;
        if(n>voice_probe.peak)voice_probe.peak=n;
    }
    if(voice_probe.fail_ms && elapsed>=voice_probe.fail_ms)return ESP_FAIL;
    if(voice_probe.pcm) {
        size_t offset=elapsed>=voice_probe.prefix_ms ? (size_t)(elapsed-voice_probe.prefix_ms)*32:SIZE_MAX;
        for(size_t i=0;i<bytes;++i) {
            size_t p=offset+i;
            ((uint8_t *)pcm)[i]=voice_probe.repeat ? voice_probe.pcm[p%voice_probe.size]:
                (elapsed>=voice_probe.prefix_ms && p<voice_probe.size ? voice_probe.pcm[p]:0);
        }
    }
    unsigned duration=(unsigned)(bytes/32);
    if(voice_probe.cancel_ms && elapsed+duration>=voice_probe.cancel_ms)*voice_probe.cancel=true;
    if(elapsed+duration>=voice_probe.stop_ms)*voice_probe.stop=true;
    return ESP_OK;
}
static void voice_probe_upload_delay(void)
{
    if(voice_probe.active && voice_probe.delay_ms) {
        unsigned delay=voice_probe.delay_ms;voice_probe.delay_ms=0;
        vTaskDelay(pdMS_TO_TICKS(delay));
    }
}

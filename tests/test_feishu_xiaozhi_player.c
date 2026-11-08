#include "feishu_xiaozhi_player.h"
#include "esp_opus_dec.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned opened,closed,decoded,played;
static int fault;
static volatile bool cancel;
esp_audio_err_t esp_opus_dec_open(void *cfg,uint32_t size,void **handle)
{
    esp_opus_dec_cfg_t *c=cfg;assert(size==sizeof(*c) && c->sample_rate==24000 && c->channel==1 && !c->self_delimited);
    if(fault==1)return ESP_AUDIO_ERR_MEM_LACK;
    ++opened;*handle=&opened;return ESP_AUDIO_ERR_OK;
}
esp_audio_err_t esp_opus_dec_close(void *handle)
{assert(handle==&opened);++closed;return ESP_AUDIO_ERR_OK;}
esp_audio_err_t esp_opus_dec_decode(void *handle,esp_audio_dec_in_raw_t *raw,esp_audio_dec_out_frame_t *frame,esp_audio_dec_info_t *info)
{
    assert(handle==&opened && frame->len==960);++decoded;
    raw->consumed=raw->len;frame->decoded_size=960;memset(frame->buffer,0,960);
    info->sample_rate=24000;info->channel=1;info->bits_per_sample=16;
    if(fault==2)return ESP_AUDIO_ERR_FAIL;
    if(fault==3)frame->decoded_size=frame->len+1;
    if(fault==4)raw->consumed=0;
    if(fault==5)info->sample_rate=16000;
    if(fault==6)cancel=true;
    return ESP_AUDIO_ERR_OK;
}
static esp_err_t sink(void *context,const int16_t *pcm,size_t samples)
{
    assert(context==&played && pcm && samples==480);++played;return fault==7 ? ESP_FAIL:ESP_OK;
}
int main(void)
{
    feishu_xiao_player_t *p=NULL;uint8_t packet[3]={0};
    assert(feishu_xiao_player_open(&p,16000,1,20,sink,&played,&cancel)==ESP_ERR_NOT_SUPPORTED && !p);
    assert(feishu_xiao_player_open(&p,24000,2,20,sink,&played,&cancel)==ESP_ERR_NOT_SUPPORTED);
    fault=1;assert(feishu_xiao_player_open(&p,24000,1,20,sink,&played,&cancel)==ESP_ERR_NO_MEM && !p);
    for(fault=0;fault<=7;++fault) {
        if(fault==1)continue;
        cancel=false;unsigned before=played;
        assert(feishu_xiao_player_open(&p,24000,1,20,sink,&played,&cancel)==ESP_OK);
        esp_err_t err=feishu_xiao_player_packet(p,packet,sizeof(packet));
        if(!fault)assert(err==ESP_OK && played==before+1);
        else {
            assert(err!=ESP_OK);
            assert(played==before+(fault==7));
            unsigned calls=decoded;assert(feishu_xiao_player_packet(p,packet,sizeof(packet))==err && calls==decoded);
        }
        feishu_xiao_player_close(&p);assert(!p && opened==closed);feishu_xiao_player_close(&p);
    }
    fault=0;cancel=true;assert(feishu_xiao_player_open(&p,24000,1,20,sink,&played,&cancel)==ESP_ERR_INVALID_STATE && !p);
    cancel=false;
    for(unsigned i=0;i<1000;++i) {
        assert(feishu_xiao_player_open(&p,24000,1,20,sink,&played,&cancel)==ESP_OK);
        assert(feishu_xiao_player_packet(p,packet,sizeof(packet))==ESP_OK);
        feishu_xiao_player_close(&p);assert(!p && opened==closed);
    }
    puts("XiaoZhi player adapter: unsupported formats, decoder OOM/error/oversize/partial output, cancellation, sink failure and 1000 lifecycles PASS (fake decoder, no real Opus or hardware)");
}

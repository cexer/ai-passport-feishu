#include "feishu_xiaozhi_player.h"
#include "feishu_xiaozhi_downlink.h"
#include "esp_opus_dec.h"
#include "esp_log.h"
#include <stdlib.h>

struct feishu_xiao_player {
    void *decoder;
    int16_t pcm[480];
    feishu_xiao_pcm_sink_t sink;
    void *context;
    const volatile bool *cancel;
    esp_err_t error;
};
void feishu_xiao_player_close(feishu_xiao_player_t **owner)
{
    if(!owner || !*owner)return;
    feishu_xiao_player_t *p=*owner;*owner=NULL;
    if(p->decoder)esp_opus_dec_close(p->decoder);
    free(p);
}
esp_err_t feishu_xiao_player_open(feishu_xiao_player_t **out,unsigned rate,unsigned channels,
                                unsigned frame_ms,feishu_xiao_pcm_sink_t sink,void *context,
                                const volatile bool *cancel)
{
    if(!out || *out || !sink)return ESP_ERR_INVALID_ARG;
    if(rate!=24000 || channels!=1 || frame_ms!=20)return ESP_ERR_NOT_SUPPORTED;
    if(cancel && *cancel)return ESP_ERR_INVALID_STATE;
    feishu_xiao_player_t *p=calloc(1,sizeof(*p));if(!p)return ESP_ERR_NO_MEM;
    p->sink=sink;p->context=context;p->cancel=cancel;
    esp_opus_dec_cfg_t cfg={.sample_rate=rate,.channel=1,
        .frame_duration=ESP_OPUS_DEC_FRAME_DURATION_20_MS,.self_delimited=false};
    esp_audio_err_t err=esp_opus_dec_open(&cfg,sizeof(cfg),&p->decoder);
    if(err!=ESP_AUDIO_ERR_OK) {
        feishu_xiao_player_close(&p);
        return err==ESP_AUDIO_ERR_MEM_LACK ? ESP_ERR_NO_MEM:ESP_FAIL;
    }
    *out=p;return ESP_OK;
}
esp_err_t feishu_xiao_player_packet(feishu_xiao_player_t *p,uint8_t *packet,size_t size)
{
    if(!p)return ESP_ERR_INVALID_ARG;
    if(p->error!=ESP_OK)return p->error;
    if(p->cancel && *p->cancel)return p->error=ESP_ERR_INVALID_STATE;
    if(!packet || !size || size>FEISHU_XIAO_OPUS_CAP)return p->error=ESP_ERR_INVALID_SIZE;
    esp_audio_dec_in_raw_t raw={.buffer=packet,.len=(uint32_t)size};
    esp_audio_dec_out_frame_t frame={.buffer=(uint8_t *)p->pcm,.len=sizeof(p->pcm)};
    esp_audio_dec_info_t info={0};
    esp_audio_err_t err=esp_opus_dec_decode(p->decoder,&raw,&frame,&info);
    if(err!=ESP_AUDIO_ERR_OK || raw.consumed!=size || !frame.decoded_size ||
       frame.decoded_size>sizeof(p->pcm) || frame.decoded_size%2 ||
       info.sample_rate!=24000 || info.channel!=1 || info.bits_per_sample!=16) {
        ESP_LOGW("feishu_player","ASSISTANT_DECODE err=%d input=%u consumed=%u decoded=%u needed=%u rate=%u channels=%u bits=%u",
            (int)err,(unsigned)size,(unsigned)raw.consumed,(unsigned)frame.decoded_size,(unsigned)frame.needed_size,
            (unsigned)info.sample_rate,(unsigned)info.channel,(unsigned)info.bits_per_sample);
        return p->error=ESP_ERR_INVALID_RESPONSE;
    }
    if(p->cancel && *p->cancel)return p->error=ESP_ERR_INVALID_STATE;
    return p->error=p->sink(p->context,p->pcm,frame.decoded_size/sizeof(int16_t));
}

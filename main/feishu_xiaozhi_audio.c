#include "feishu_xiaozhi_audio.h"
size_t feishu_xiao_audio_encode(int16_t *pcm,size_t samples,feishu_xiao_packet_t *packet)
{
    if(!packet)return 0;
    packet->size=0;
    if(!pcm || samples!=FEISHU_XIAO_CAPTURE_SAMPLES)return 0;
    unsigned peak=0;
    for(unsigned i=0;i<FEISHU_XIAO_QUEUE_SAMPLES;++i) {
        int value=((int32_t)pcm[i*2]+pcm[i*2+1])/2;pcm[i]=(int16_t)value;
        unsigned magnitude=(unsigned)(value<0 ? -value:value);
        if(magnitude>peak)peak=magnitude;
    }
    packet->bytes[0]=(uint8_t)peak;packet->bytes[1]=(uint8_t)(peak>>8);
    uint32_t bits=0;unsigned count=0;size_t at=2;
    for(unsigned i=0;i<FEISHU_XIAO_QUEUE_SAMPLES;++i) {
        int scaled=(int32_t)pcm[i]*31;
        int quant=peak ? (scaled+(scaled<0 ? -(int)(peak/2):(int)(peak/2)))/(int)peak:0;
        bits|=(uint32_t)(quant+31)<<count;count+=6;
        while(count>=8){packet->bytes[at++]=(uint8_t)bits;bits>>=8;count-=8;}
    }
    if(count || at!=FEISHU_XIAO_PACKET_CAP)return 0;
    packet->size=(uint16_t)at;return at;
}
bool feishu_xiao_audio_decode(const feishu_xiao_packet_t *packet,int16_t *pcm,size_t capacity)
{
    if(!packet || !pcm || packet->size!=FEISHU_XIAO_PACKET_CAP || capacity<FEISHU_XIAO_QUEUE_SAMPLES)return false;
    unsigned peak=(unsigned)packet->bytes[0]|(unsigned)packet->bytes[1]<<8;
    if(peak>32768)return false;
    uint32_t bits=0;unsigned count=0;size_t at=2;
    for(unsigned i=0;i<FEISHU_XIAO_QUEUE_SAMPLES;++i) {
        while(count<6){bits|=(uint32_t)packet->bytes[at++]<<count;count+=8;}
        unsigned code=bits&63u;bits>>=6;count-=6;
        if(code==63)return false;
        int scaled=((int)code-31)*(int)peak;
        int value=(scaled+(scaled<0 ? -15:15))/31;
        if(value>32767)value=32767;
        if(value< -32768)value= -32768;
        pcm[i]=(int16_t)value;
    }
    return true;
}

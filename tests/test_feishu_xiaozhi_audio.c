#include "feishu_xiaozhi_audio.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

_Static_assert(FEISHU_XIAO_CAPTURE_SAMPLES*1000==16000*FEISHU_XIAO_FRAME_MS,"capture duration");
_Static_assert(FEISHU_XIAO_QUEUE_SAMPLES*1000==8000*FEISHU_XIAO_FRAME_MS,"wire duration");
_Static_assert(FEISHU_XIAO_QUEUE_FRAMES*FEISHU_XIAO_FRAME_MS==2400,"backlog duration");
_Static_assert(sizeof(feishu_xiao_packet_t)==124,"queue packet footprint");
int main(void)
{
    int16_t pcm[320]={32767,-32768,-32768,-32768,32767,32767,-3,0,0,3},out[161];
    feishu_xiao_packet_t packet={0};
    assert(feishu_xiao_audio_encode(pcm,320,&packet)==122 && packet.size==122);
    assert(pcm[0]==0 && pcm[1]==-32768 && pcm[2]==32767 && pcm[3]==-1 && pcm[4]==1);
    out[160]=1234;assert(feishu_xiao_audio_decode(&packet,out,160) && out[160]==1234);
    assert(!feishu_xiao_audio_decode(&packet,out,159));
    packet.size=123;assert(!feishu_xiao_audio_decode(&packet,out,160));
    packet.size=122;packet.bytes[1]=255;assert(!feishu_xiao_audio_decode(&packet,out,160));
    assert(!feishu_xiao_audio_encode(pcm,319,&packet) && !packet.size);
    assert(!feishu_xiao_audio_encode(NULL,320,&packet));
    assert(!feishu_xiao_audio_decode(NULL,out,160));
    for(unsigned frame=0;frame<2000;++frame) {
        for(unsigned i=0;i<320;++i) {
            int p=(int)((frame*320+i)%320);pcm[i]=(int16_t)(p<160 ? p*200-16000:48000-p*200);
        }
        assert(feishu_xiao_audio_encode(pcm,320,&packet)==122);
        assert(feishu_xiao_audio_decode(&packet,out,160));
        unsigned error=0;for(unsigned i=0;i<160;++i)error+=(unsigned)abs((int)pcm[i]-out[i]);
        assert(error/160<2000 && out[160]==1234);
    }
    /* Exhaust the signed input domain against an independent absolute-error
     * bound with a fixed full-scale peak, rather than duplicating the codec. */
    for(int base=-32768;base<=32767;base+=159) {
        pcm[0]=pcm[1]=-32768;
        for(unsigned i=1;i<160;++i) {
            int value=base+(int)i-1;if(value>32767)value=32767;
            pcm[i*2]=pcm[i*2+1]=(int16_t)value;
        }
        assert(feishu_xiao_audio_encode(pcm,320,&packet)==122);
        assert(feishu_xiao_audio_decode(&packet,out,160));
        for(unsigned i=0;i<160;++i)assert(abs((int)pcm[i]-out[i])<=530);
    }
    packet.bytes[2]|=63;assert(!feishu_xiao_audio_decode(&packet,out,160));
    puts("XiaoZhi queue: 16k pair-average -> 8k PCM6 -> 320-byte PCM20ms, 2400ms capacity, 40s reuse and malformed recovery PASS");
}

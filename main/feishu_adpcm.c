#include "feishu_adpcm.h"
#include <string.h>
static const int steps[89]={7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767};
static const int adjust[8]={-1,-1,-1,-1,2,4,6,8};
static void update(feishu_adpcm_t *s,unsigned code)
{
    int step=steps[s->index],delta=step>>3;
    if(code&4)delta+=step;
    if(code&2)delta+=step>>1;
    if(code&1)delta+=step>>2;
    s->predictor+=(code&8) ? -delta:delta;
    if(s->predictor>32767)s->predictor=32767;
    if(s->predictor<-32768)s->predictor=-32768;
    int index=(int)s->index+adjust[code&7];
    s->index=(unsigned)(index<0 ? 0:index>88 ? 88:index);
}
size_t feishu_adpcm_encode(feishu_adpcm_t *state,const int16_t *pcm,size_t samples,uint8_t *out,size_t cap)
{
    if(!state || !pcm || !out || !samples || samples>FEISHU_ADPCM_SAMPLES ||
       cap<4+(samples+1)/2 || state->index>88 || state->predictor<-32768 || state->predictor>32767)return 0;
    feishu_adpcm_t s=*state;uint16_t predictor=(uint16_t)(int16_t)s.predictor;
    out[0]=(uint8_t)predictor;out[1]=(uint8_t)(predictor>>8);out[2]=(uint8_t)s.index;out[3]=0;
    memset(out+4,0,(samples+1)/2);
    for(size_t i=0;i<samples;++i) {
        int diff=(int)pcm[i]-s.predictor,step=steps[s.index];unsigned code=0;
        if(diff<0){code=8;diff=-diff;}
        if(diff>=step){code|=4;diff-=step;}
        step>>=1;if(diff>=step){code|=2;diff-=step;}
        step>>=1;if(diff>=step)code|=1;
        update(&s,code);out[4+i/2]|=(uint8_t)(code<<((i&1)?0:4));
    }
    *state=s;return 4+(samples+1)/2;
}
bool feishu_adpcm_decode(const uint8_t *block,size_t bytes,size_t samples,int16_t *pcm,size_t capacity)
{
    if(!block || !pcm || !samples || samples>FEISHU_ADPCM_SAMPLES || capacity<samples ||
       bytes!=4+(samples+1)/2 || block[2]>88 || block[3] || ((samples&1)&&(block[bytes-1]&15)))return false;
    feishu_adpcm_t s={(int16_t)((uint16_t)block[0]|(uint16_t)block[1]<<8),block[2]};
    for(size_t i=0;i<samples;++i) {
        unsigned code=(block[4+i/2]>>((i&1)?0:4))&15;update(&s,code);pcm[i]=(int16_t)s.predictor;
    }
    return true;
}

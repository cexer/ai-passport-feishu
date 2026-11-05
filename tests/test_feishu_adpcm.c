#include "feishu_adpcm.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif
static int pipe_codec(bool encode)
{
#ifdef _WIN32
    _setmode(_fileno(stdin),_O_BINARY);_setmode(_fileno(stdout),_O_BINARY);
#endif
    int16_t pcm[FEISHU_ADPCM_SAMPLES];uint8_t packed[FEISHU_ADPCM_BYTES];feishu_adpcm_t state={0};
    for(;;) {
        size_t n,bytes;
        if(encode) {
            n=fread(pcm,sizeof(int16_t),FEISHU_ADPCM_SAMPLES,stdin);if(!n)break;
            bytes=feishu_adpcm_encode(&state,pcm,n,packed,sizeof(packed));assert(bytes);
            uint8_t size[2]={(uint8_t)n,(uint8_t)(n>>8)};
            assert(fwrite(size,1,2,stdout)==2 && fwrite(packed,1,bytes,stdout)==bytes);
        } else {
            uint8_t size[2];size_t got=fread(size,1,2,stdin);if(!got)break;assert(got==2);
            n=(size_t)size[0]|(size_t)size[1]<<8;bytes=4+(n+1)/2;assert(n && n<=FEISHU_ADPCM_SAMPLES);
            assert(fread(packed,1,bytes,stdin)==bytes && feishu_adpcm_decode(packed,bytes,n,pcm,FEISHU_ADPCM_SAMPLES));
            assert(fwrite(pcm,sizeof(int16_t),n,stdout)==n);
        }
    }
    return ferror(stdin)||ferror(stdout);
}
int main(int argc,char **argv)
{
    if(argc==2)return pipe_codec(!strcmp(argv[1],"encode"));
    int16_t pcm[FEISHU_ADPCM_SAMPLES]={0},out[FEISHU_ADPCM_SAMPLES+1];uint8_t packed[FEISHU_ADPCM_BYTES];
    feishu_adpcm_t state={0};size_t bytes=feishu_adpcm_encode(&state,pcm,FEISHU_ADPCM_SAMPLES,packed,sizeof(packed));
    assert(bytes==FEISHU_ADPCM_BYTES && !state.predictor && !state.index);
    for(size_t i=0;i<bytes;++i)assert(!packed[i]);
    assert(feishu_adpcm_decode(packed,bytes,FEISHU_ADPCM_SAMPLES,out,FEISHU_ADPCM_SAMPLES));
    assert(!memcmp(pcm,out,sizeof(pcm)));
    assert(!feishu_adpcm_encode(&state,pcm,FEISHU_ADPCM_SAMPLES,packed,sizeof(packed)-1));
    assert(!feishu_adpcm_decode(packed,bytes-1,FEISHU_ADPCM_SAMPLES,out,FEISHU_ADPCM_SAMPLES));
    packed[2]=89;assert(!feishu_adpcm_decode(packed,bytes,FEISHU_ADPCM_SAMPLES,out,FEISHU_ADPCM_SAMPLES));
    packed[2]=0;packed[3]=1;assert(!feishu_adpcm_decode(packed,bytes,FEISHU_ADPCM_SAMPLES,out,FEISHU_ADPCM_SAMPLES));
    packed[3]=0;
    for(unsigned frame=0;frame<200;++frame) {
        for(unsigned i=0;i<FEISHU_ADPCM_SAMPLES;++i)pcm[i]=(int16_t)((frame*37+i*191)%65536-32768);
        bytes=feishu_adpcm_encode(&state,pcm,FEISHU_ADPCM_SAMPLES,packed,sizeof(packed));assert(bytes==sizeof(packed));
        out[FEISHU_ADPCM_SAMPLES]=1234;
        assert(feishu_adpcm_decode(packed,bytes,FEISHU_ADPCM_SAMPLES,out,FEISHU_ADPCM_SAMPLES));
        assert(out[FEISHU_ADPCM_SAMPLES]==1234 && state.index<=88 && state.predictor>=-32768 && state.predictor<=32767);
    }
    state=(feishu_adpcm_t){0};pcm[0]=2000;
    bytes=feishu_adpcm_encode(&state,pcm,1,packed,sizeof(packed));assert(bytes==5);
    assert(feishu_adpcm_decode(packed,bytes,1,out,1));packed[4]|=1;assert(!feishu_adpcm_decode(packed,bytes,1,out,1));
    puts("ADPCM: bounded 804-byte blocks, silence, malformed headers, odd tails, saturation and 20s reuse PASS");
    return 0;
}

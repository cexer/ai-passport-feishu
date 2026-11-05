#include "feishu_asr_batch.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct {unsigned count,next,released,writes,fail_write;int source_failure;bool held[2];uint8_t pcm[2][6400],output[19200];size_t used,block_bytes;} fixture_t;
static int next(void *context,const void **data)
{
    fixture_t *f=context;
    if((int)f->next==f->source_failure)return -2;
    if(f->next==f->count)return -1;
    unsigned slot=f->next%2;assert(!f->held[slot]);f->held[slot]=true;
    memset(f->pcm[slot],++f->next,sizeof(f->pcm[slot]));*data=f->pcm[slot];return (int)slot;
}
static void release(void *context,int slot)
{fixture_t *f=context;assert(f->held[slot]);f->held[slot]=false;++f->released;memset(f->pcm[slot],0xfe,sizeof(f->pcm[slot]));}
static bool write(void *context,const void *data,size_t size)
{
    fixture_t *f=context;assert(size==f->block_bytes && f->used+size<=sizeof(f->output));
    if(++f->writes==f->fail_write)return false;
    memcpy(f->output+f->used,data,size);f->used+=size;return true;
}
static void reset(fixture_t *f,unsigned count)
{memset(f,0,sizeof(*f));f->count=count;f->source_failure=-1;f->block_bytes=6400;}
int main(void)
{
    fixture_t f;bool done=false;
    for(unsigned n=0;n<=3;++n) {
        reset(&f,n);assert(feishu_asr_batch(3,6400,f.pcm[0],next,release,&f,write,&f,&done));
        assert(f.used==19200 && f.released==n && done==(n<3));
        for(unsigned i=0;i<19200;++i)assert(f.output[i]==(i/6400<n ? i/6400+1:0));
    }
    for(unsigned failure=1;failure<=3;++failure) {
        reset(&f,3);f.fail_write=failure;
        assert(!feishu_asr_batch(3,6400,f.pcm[0],next,release,&f,write,&f,&done));
        assert(f.released==failure && !f.held[0] && !f.held[1]);
    }
    for(int failure=0;failure<3;++failure) {
        reset(&f,3);f.source_failure=failure;
        assert(!feishu_asr_batch(3,6400,f.pcm[0],next,release,&f,write,&f,&done));
        assert(f.released==(unsigned)failure && !f.held[0] && !f.held[1]);
    }
    /* Reuse the same two buffers for 30 seconds and for recovery after faults. */
    reset(&f,150);
    for(unsigned i=0;i<50;++i){f.used=0;assert(feishu_asr_batch(3,6400,f.pcm[0],next,release,&f,write,&f,&done));assert(!done);}
    assert(f.released==150 && !f.held[0] && !f.held[1]);
    f.used=0;assert(feishu_asr_batch(3,6400,f.pcm[0],next,release,&f,write,&f,&done)&&done);
    assert(!feishu_asr_batch(5,6400,f.pcm[0],next,release,&f,write,&f,&done));
    reset(&f,6);f.block_bytes=3200;
    assert(feishu_asr_batch(6,3200,f.pcm[0],next,release,&f,write,&f,&done) && f.released==6 && f.used==19200);
    for(unsigned i=0;i<19200;++i)assert(f.output[i]==i/3200+1);
    puts("ASR batch: two reused buffers, early release, tail padding, source/write faults and 30s stress PASS");
    return 0;
}

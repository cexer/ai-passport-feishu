#include "feishu_fdb.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define SIZE (64u*1024u)
static unsigned char flash[SIZE],saved[SIZE];
static unsigned erases,cut;
static int rd(void *ctx,uint32_t at,void *out,size_t n)
{(void)ctx;assert(at<=SIZE && n<=SIZE-at);memcpy(out,flash+at,n);return (int)n;}
static int wr(void *ctx,uint32_t at,const void *in,size_t n)
{
    (void)ctx;assert(at<=SIZE && n<=SIZE-at);const unsigned char *p=in;
    for(size_t i=0;i<n;++i){assert((flash[at+i]&p[i])==p[i]);flash[at+i]&=p[i];}
    return (int)n;
}
static int er(void *ctx,uint32_t at,size_t n)
{
    (void)ctx;assert(at<=SIZE && n<=SIZE-at && at%4096==0 && n%4096==0);
    for(size_t i=0;i<n;i+=4096) {
        ++erases;memset(flash+at+i,255,4096);
        if(cut && --cut==0)return -1; /* Reset after this physical erase. */
    }
    return (int)n;
}
static void check(const unsigned char *data,size_t size)
{
    unsigned char out[32];size_t n=0;
    assert(feishu_fdb_get("required",out,sizeof(out),&n)==FEISHU_FDB_OK);
    assert(n==size && !memcmp(data,out,n));
}
int main(void)
{
    const feishu_fdb_io_t io={NULL,SIZE,rd,wr,er};
    memset(flash,255,sizeof(flash));assert(feishu_fdb_open(&io,true)==FEISHU_FDB_OK);
    const unsigned char required[]={7,19,42};
    assert(feishu_fdb_put("required",required,sizeof(required))==FEISHU_FDB_OK);
    unsigned char value[1000];unsigned trigger=0;
    for(unsigned i=1;i<200;++i) {
        memcpy(saved,flash,SIZE);unsigned before=erases;memset(value,(int)i,sizeof(value));
        assert(feishu_fdb_put("hot",value,sizeof(value))==FEISHU_FDB_OK);
        if(erases>before){trigger=i;break;}
    }
    assert(trigger);feishu_fdb_close();
    for(unsigned stop=1;stop<=4;++stop) {
        memcpy(flash,saved,SIZE);cut=0;
        assert(feishu_fdb_open(&io,false)==FEISHU_FDB_OK);
        cut=stop;memset(value,(int)trigger,sizeof(value));
        assert(feishu_fdb_put("hot",value,sizeof(value))==FEISHU_FDB_IO);
        assert(cut==0);feishu_fdb_close();
        assert(feishu_fdb_open(&io,false)==FEISHU_FDB_OK);check(required,sizeof(required));
        assert(feishu_fdb_put("after","recovered",10)==FEISHU_FDB_OK);
        feishu_fdb_close();assert(feishu_fdb_open(&io,false)==FEISHU_FDB_OK);check(required,sizeof(required));
        feishu_fdb_close();
    }
    puts("FlashDB erase: reset after each 4 KiB unit preserves GC header, protected value and normal writes PASS");
    return 0;
}

#include "feishu_fdb_host.h"
#include <assert.h>
#include <string.h>
#define SIZE (640u*1024u)
static unsigned char flash[SIZE],checkpoint[SIZE];
static int read_left=-1,write_left=-1;
static bool partial,tripped;
static unsigned reads,writes;
static bool fail(int *left)
{if(*left==0){tripped=true;*left=-1;return true;}if(*left>0)--*left;return false;}
static int rd(void *ctx,uint32_t at,void *out,size_t n)
{
    (void)ctx;assert(at<=SIZE && n<=SIZE-at);++reads;
    if(fail(&read_left)) {if(partial && n)memcpy(out,flash+at,n-1);return partial?(int)n-1:-1;}
    memcpy(out,flash+at,n);return (int)n;
}
static int wr(void *ctx,uint32_t at,const void *in,size_t n)
{
    (void)ctx;assert(at<=SIZE && n<=SIZE-at);++writes;
    bool failed=fail(&write_left);size_t actual=failed?(partial?n/2:0):n;
    for(size_t i=0;i<actual;++i){assert((flash[at+i]&((const unsigned char *)in)[i])==((const unsigned char *)in)[i]);flash[at+i]&=((const unsigned char *)in)[i];}
    return failed?-1:(int)n;
}
static int er(void *ctx,uint32_t at,size_t n)
{
    (void)ctx;assert(at<=SIZE && n<=SIZE-at && at%4096==0 && n%4096==0);++writes;
    if(fail(&write_left))return -1;
    memset(flash+at,0xff,n);return (int)n;
}
static const feishu_fdb_io_t io={NULL,SIZE,rd,wr,er};
void feishu_fdb_host_fault(int r,int w,bool p)
{read_left=r;write_left=w;partial=p;tripped=false;reads=writes=0;}
const feishu_fdb_io_t *feishu_fdb_host_reset(void)
{memset(flash,0xff,sizeof(flash));feishu_fdb_host_fault(-1,-1,false);return &io;}
void feishu_fdb_host_checkpoint(void) {memcpy(checkpoint,flash,SIZE);}
void feishu_fdb_host_restore(void) {memcpy(flash,checkpoint,SIZE);feishu_fdb_host_fault(-1,-1,false);}
bool feishu_fdb_host_tripped(void) {return tripped;}
unsigned feishu_fdb_host_reads(void) {return reads;}
unsigned feishu_fdb_host_writes(void) {return writes;}

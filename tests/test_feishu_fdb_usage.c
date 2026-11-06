#include "feishu_fdb_host.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void grouped_read(void)
{
    const feishu_fdb_io_t *io=feishu_fdb_host_reset();
    assert(feishu_fdb_open(io,true)==FEISHU_FDB_OK);
    char key[32];unsigned char data[512]={9},out[512];size_t size;
    for(unsigned i=0;i<180;++i) {
        snprintf(key,sizeof(key),"before%03u",i);assert(feishu_fdb_put(key,data,8)==FEISHU_FDB_OK);
    }
    for(unsigned i=0;i<16;++i) {
        snprintf(key,sizeof(key),"sample/%02u",i);assert(feishu_fdb_put(key,data,sizeof(data))==FEISHU_FDB_OK);
    }
    for(unsigned i=0;i<180;++i) {
        snprintf(key,sizeof(key),"after%03u",i);assert(feishu_fdb_put(key,data,8)==FEISHU_FDB_OK);
    }
    feishu_fdb_close();assert(feishu_fdb_open(io,false)==FEISHU_FDB_OK);
    feishu_fdb_host_fault(-1,-1,false);
    for(unsigned i=0;i<16;++i) {
        snprintf(key,sizeof(key),"sample/%02u",i);assert(feishu_fdb_get(key,out,sizeof(out),&size)==FEISHU_FDB_OK);
        assert(size==sizeof(data) && !memcmp(out,data,size));
    }
    unsigned before=feishu_fdb_host_reads();
    feishu_fdb_close();assert(feishu_fdb_open(io,false)==FEISHU_FDB_OK);
    feishu_fdb_host_fault(-1,-1,false);
    assert(feishu_fdb_prefetch("sample/","sample/00","sample/15",16)==FEISHU_FDB_OK);
    for(unsigned i=0;i<16;++i) {
        snprintf(key,sizeof(key),"sample/%02u",i);assert(feishu_fdb_get(key,out,sizeof(out),&size)==FEISHU_FDB_OK);
        assert(size==sizeof(data) && !memcmp(out,data,size));
    }
    unsigned after=feishu_fdb_host_reads();assert(after<before/2);
    feishu_fdb_host_fault(-1,-1,false);
    assert(feishu_fdb_prefetch("sample/","sample/00","sample/15",16)==FEISHU_FDB_OK);
    assert(feishu_fdb_host_reads()<32);
    data[0]=42;assert(feishu_fdb_put("sample/15",data,sizeof(data))==FEISHU_FDB_OK);
    assert(feishu_fdb_get("sample/15",out,sizeof(out),&size)==FEISHU_FDB_OK && !memcmp(data,out,size));
    feishu_fdb_host_fault(0,-1,false);
    assert(feishu_fdb_prefetch("sample/","sample/00","sample/15",16)==FEISHU_FDB_IO);
    feishu_fdb_host_fault(-1,-1,false);assert(feishu_fdb_put("blocked",data,8)==FEISHU_FDB_IO);
    feishu_fdb_close();assert(feishu_fdb_open(io,false)==FEISHU_FDB_OK);
    assert(feishu_fdb_get("sample/15",out,sizeof(out),&size)==FEISHU_FDB_OK && !memcmp(data,out,size));
    feishu_fdb_close();printf("Grouped KV read: %u -> %u reads; cached repeat, update revalidation and IO recovery PASS\n",before,after);
}
int main(void)
{
    const feishu_fdb_io_t *io=feishu_fdb_host_reset();
    assert(feishu_fdb_open(io,true)==FEISHU_FDB_OK);
    const char small[4]={1},large[8]={2};
    assert(feishu_fdb_put("object",small,sizeof(small))==FEISHU_FDB_OK);
    assert(feishu_fdb_put("object",large,sizeof(large))==FEISHU_FDB_OK);
    size_t margin=io->size-32768-(6+sizeof(large)+64);
    feishu_fdb_host_fault(-1,-1,false);
    assert(feishu_fdb_room(margin) && feishu_fdb_host_reads());
    feishu_fdb_host_fault(-1,-1,false);
    assert(!feishu_fdb_room(io->size) && !feishu_fdb_host_reads());
    assert(feishu_fdb_delete("object")==FEISHU_FDB_OK);
    feishu_fdb_host_fault(-1,-1,false);
    assert(!feishu_fdb_room(io->size) && !feishu_fdb_host_reads());
    feishu_fdb_close();grouped_read();puts("FlashDB usage: overwrite recount, exact deletion and no repeated pressure scan PASS");return 0;
}

#include "feishu_fdb.h"
#include "flashdb.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define CAPACITY (128u*1024u)
/* Host NOR equipment; these arrays are never linked into device firmware. */
static unsigned char flash[CAPACITY], baseline[CAPACITY], snapshot[CAPACITY];
static int read_cut=-1, write_cut=-1, erase_cut=-1;
static bool short_io;
static unsigned reads,writes,erases,blocked_writes;
static size_t read_bytes_total;
static unsigned cooperation_count,visited_count;
static void cooperative_tick(void) {++cooperation_count;}
static bool visited(const char *key,size_t size,void *context)
{(void)key;(void)size;(void)context;++visited_count;return false;}
static bool tripped;
static const char *phase="basic";
static int fault_position=-1;
static int rd(void *ctx,uint32_t at,void *out,size_t size)
{
    (void)ctx;
    assert(at<=CAPACITY && size<=CAPACITY-at);
    ++reads;
    read_bytes_total+=size;
    if(read_cut==0) {
        read_cut=-1; tripped=true;
        if(short_io && size) memcpy(out,flash+at,size-1);
        return short_io ? (int)size-1 : -1;
    }
    if(read_cut>0) --read_cut;
    memcpy(out,flash+at,size);
    return (int)size;
}
static int wr(void *ctx,uint32_t at,const void *in,size_t size)
{
    (void)ctx;
    assert(at<=CAPACITY && size<=CAPACITY-at);
    if(tripped) ++blocked_writes;
    ++writes;
    bool fail=write_cut==0;
    if(write_cut>0) --write_cut;
    size_t count=fail ? (short_io ? size/2 : 0) : size;
    for(size_t i=0;i<count;++i) {
        assert((flash[at+i] & ((const unsigned char *)in)[i])==((const unsigned char *)in)[i]);
        flash[at+i]&=((const unsigned char *)in)[i];
    }
    if(fail) {tripped=true;return -1;}
    return (int)size;
}
static int er(void *ctx,uint32_t at,size_t size)
{
    (void)ctx;
    assert(at%4096==0 && size%4096==0 && at<=CAPACITY && size<=CAPACITY-at);
    if(tripped) ++blocked_writes;
    ++erases;
    bool fail=erase_cut==0;
    if(erase_cut>0) --erase_cut;
    if(!fail) memset(flash+at,0xff,size);
    if(fail) {tripped=true;return -1;}
    return (int)size;
}
static const feishu_fdb_io_t io={NULL,CAPACITY,rd,wr,er};
static void reset_faults(void)
{
    read_cut=write_cut=erase_cut=-1;
    reads=writes=erases=blocked_writes=0;
    read_bytes_total=0;
    tripped=false;
}
static void check_value(const char *key,const char *value)
{
    char buf[64]={0};size_t size=0;
    assert(feishu_fdb_get(key,buf,sizeof(buf),&size)==FEISHU_FDB_OK);
    assert(size==strlen(value)+1 && !memcmp(buf,value,size));
}
static void reopen(void)
{
    feishu_fdb_close();reset_faults();
    int rc=feishu_fdb_open(&io,false);
    if(rc!=FEISHU_FDB_OK)fprintf(stderr,"raw reopen failed phase=%s cut=%d rc=%d\n",phase,fault_position,rc);
    assert(rc==FEISHU_FDB_OK);
    check_value("owner","protected");
}
int main(void)
{
    feishu_fdb_set_cooperate(cooperative_tick);
    memset(flash,0xff,sizeof(flash));
    assert(feishu_fdb_open(&io,false)!=FEISHU_FDB_OK);
    assert(!writes && !erases);
    assert(feishu_fdb_open(&io,true)==FEISHU_FDB_OK);
    assert(feishu_fdb_open(&io,false)==FEISHU_FDB_BUSY);
    assert(feishu_fdb_put("owner","protected",10)==FEISHU_FDB_OK);
    assert(feishu_fdb_put("body","old",4)==FEISHU_FDB_OK);
    /* Find a real CRC16 cache collision where one key is another's prefix.
     * A cache hit must compare both the complete name and its length. */
    const char *short_key="prefix-key";char long_key[64]={0};bool collision=false;
    uint16_t target=(uint16_t)(fdb_calc_crc32(0,short_key,strlen(short_key))>>16);
    for(unsigned i=0;i<2000000;++i) {
        snprintf(long_key,sizeof(long_key),"%s/%u",short_key,i);
        if((uint16_t)(fdb_calc_crc32(0,long_key,strlen(long_key))>>16)==target){collision=true;break;}
    }
    assert(collision);
    assert(feishu_fdb_put(short_key,"short",6)==FEISHU_FDB_OK);
    assert(feishu_fdb_put(long_key,"long",5)==FEISHU_FDB_OK);
    check_value(long_key,"long");check_value(short_key,"short");check_value(long_key,"long");
    check_value("body","old");
    size_t needed=0;char tiny[2];
    assert(feishu_fdb_get("body",tiny,sizeof(tiny),&needed)==FEISHU_FDB_TOOBIG && needed==4);
    feishu_fdb_close();memcpy(baseline,flash,sizeof(flash));

    /* Unknown nonblank bytes, even with explicit blank initialization enabled. */
    memset(flash,0xff,sizeof(flash));flash[CAPACITY-1]=0;
    memcpy(snapshot,flash,sizeof(flash));reset_faults();
    assert(feishu_fdb_open(&io,true)!=FEISHU_FDB_OK);
    assert(!writes && !erases && !memcmp(snapshot,flash,sizeof(flash)));
    memcpy(flash,baseline,sizeof(flash));reset_faults();
    assert(feishu_fdb_open(&io,false)==FEISHU_FDB_OK);
    unsigned open_reads=reads;
    feishu_fdb_close();
    unsigned open_cases=0,get_cases=0;
    for(int short_case=0;short_case<2;++short_case) {
        short_io=short_case!=0;
        for(unsigned cut=0;cut<open_reads;++cut) {
            memcpy(flash,baseline,sizeof(flash));reset_faults();read_cut=(int)cut;
            assert(feishu_fdb_open(&io,false)==FEISHU_FDB_IO);
            assert(tripped && !writes && !erases && !blocked_writes);
            assert(!memcmp(flash,baseline,sizeof(flash)));
            reopen();check_value("body","old");feishu_fdb_close();++open_cases;
        }
        memcpy(flash,baseline,sizeof(flash));reopen();reads=0;
        check_value("body","old");unsigned get_reads=reads;feishu_fdb_close();
        for(unsigned cut=0;cut<get_reads;++cut) {
            memcpy(flash,baseline,sizeof(flash));reopen();
            reset_faults();read_cut=(int)cut;char buf[64];size_t size;
            assert(feishu_fdb_get("body",buf,sizeof(buf),&size)==FEISHU_FDB_IO);
            assert(feishu_fdb_put("owner","overwrite",10)==FEISHU_FDB_IO);
            assert(feishu_fdb_delete("owner")==FEISHU_FDB_IO);
            assert(!writes && !erases && !blocked_writes && !memcmp(flash,baseline,sizeof(flash)));
            reopen();check_value("body","old");feishu_fdb_close();++get_cases;
        }
    }
    /* Interrupted replacement: recover a complete old or new value, never a mix. */
    unsigned operation_read_cases=0;
    for(int op=0;op<3;++op) {
        memcpy(flash,baseline,sizeof(flash));reopen();reset_faults();
        if(op==0) assert(feishu_fdb_put("body","new",4)==FEISHU_FDB_OK);
        if(op==1) assert(feishu_fdb_put("new-key","new",4)==FEISHU_FDB_OK);
        if(op==2) assert(feishu_fdb_delete("body")==FEISHU_FDB_OK);
        unsigned operation_reads=reads;feishu_fdb_close();
        for(int partial=0;partial<2;++partial) for(unsigned cut=0;cut<operation_reads;++cut) {
            memcpy(flash,baseline,sizeof(flash));reopen();reset_faults();
            short_io=partial!=0;read_cut=(int)cut;
            int rc=op==2 ? feishu_fdb_delete("body") :
                   feishu_fdb_put(op ? "new-key" : "body","new",4);
            assert(tripped && rc==FEISHU_FDB_IO && !blocked_writes);
            unsigned mutations=writes+erases;
            assert(feishu_fdb_put("owner","overwrite",10)==FEISHU_FDB_IO);
            assert(mutations==writes+erases);
            reopen();feishu_fdb_close();++operation_read_cases;
        }
    }
    unsigned write_cases=0;
    for(int partial=0;partial<2;++partial) for(int cut=0;cut<16;++cut) {
        memcpy(flash,baseline,sizeof(flash));reopen();reset_faults();
        short_io=partial!=0;write_cut=cut;
        int rc=feishu_fdb_put("body","new",4);
        if(tripped) {
            assert(rc==FEISHU_FDB_IO && !blocked_writes);
            assert(feishu_fdb_put("owner","overwrite",10)==FEISHU_FDB_IO);
            ++write_cases;
        } else assert(rc==FEISHU_FDB_OK);
        reopen();char value[64]={0};size_t size;
        assert(feishu_fdb_get("body",value,sizeof(value),&size)==FEISHU_FDB_OK);
        assert(size==4 && (!memcmp(value,"old",4) || !memcmp(value,"new",4)));
        feishu_fdb_close();
    }
    /* A failed new-key append must not leave a false-negative membership
     * result after reopen, nor prevent subsequent normal insertion. */
    for(int partial=0;partial<2;++partial) for(int cut=0;cut<16;++cut) {
        memcpy(flash,baseline,sizeof(flash));reopen();reset_faults();
        short_io=partial!=0;write_cut=cut;
        int rc=feishu_fdb_put("brand-new","new",4);
        if(tripped)assert(rc==FEISHU_FDB_IO);else assert(rc==FEISHU_FDB_OK);
        reopen();char value[32];size_t n=0;
        rc=feishu_fdb_get("brand-new",value,sizeof(value),&n);
        assert(rc==FEISHU_FDB_NOTFOUND || (rc==FEISHU_FDB_OK && n==4 && !memcmp(value,"new",4)));
        assert(feishu_fdb_put("brand-new","retry",6)==FEISHU_FDB_OK);
        check_value("brand-new","retry");feishu_fdb_close();
    }
    /* A read failure during the blank preflight cannot trigger formatting. */
    memset(flash,0xff,sizeof(flash));reset_faults();read_cut=10;
    assert(feishu_fdb_open(&io,true)==FEISHU_FDB_IO && !writes && !erases);
    reset_faults();assert(feishu_fdb_open(&io,true)==FEISHU_FDB_OK);
    assert(feishu_fdb_put("owner","protected",10)==FEISHU_FDB_OK);
    char data[2048];memset(data,0x5a,sizeof(data));int count=0,rc=0;
    static unsigned char wide_owner[6144];memset(wide_owner,0x37,sizeof(wide_owner));
    assert(feishu_fdb_put("wide-owner",wide_owner,sizeof(wide_owner))==FEISHU_FDB_OK);
    for(;count<100;++count) {char key[24];snprintf(key,sizeof(key),"chunk-%d",count);
        rc=feishu_fdb_put(key,data,sizeof(data));if(rc)break;}
    assert(rc==FEISHU_FDB_FULL && count>0 && count<100);
    check_value("owner","protected");
    memcpy(baseline,flash,sizeof(flash));
    unsigned gc_faults=0;
    phase="full/GC";
    for(int partial=0;partial<2;++partial)for(int cut=0;cut<32;++cut) {
        fault_position=cut;
        feishu_fdb_close();memcpy(flash,baseline,sizeof(flash));reset_faults();
        assert(feishu_fdb_open(&io,false)==FEISHU_FDB_OK);
        for(int i=0;i<8;++i){char key[24];snprintf(key,sizeof(key),"chunk-%d",i);assert(feishu_fdb_delete(key)==FEISHU_FDB_OK);}
        static unsigned char big[8192];memset(big,0x6c,sizeof(big));
        reset_faults();short_io=partial!=0;write_cut=cut;
        int saved=feishu_fdb_put("after-gc",big,sizeof(big));
        if(tripped){assert(saved==FEISHU_FDB_IO && !blocked_writes);++gc_faults;}
        else assert(saved==FEISHU_FDB_OK || saved==FEISHU_FDB_FULL);
        reopen();size_t n=0;unsigned char preserved[2048];
        assert(feishu_fdb_get("chunk-8",preserved,sizeof(preserved),&n)==FEISHU_FDB_OK && n==sizeof(preserved));
        for(size_t i=0;i<n;++i)assert(preserved[i]==0x5a);
        static unsigned char wide_check[6144];
        assert(feishu_fdb_get("wide-owner",wide_check,sizeof(wide_check),&n)==FEISHU_FDB_OK && n==sizeof(wide_check));
        assert(!memcmp(wide_check,wide_owner,sizeof(wide_check)));
    }
    feishu_fdb_close();memcpy(flash,baseline,sizeof(flash));reset_faults();
    assert(feishu_fdb_open(&io,false)==FEISHU_FDB_OK);
    assert(feishu_fdb_delete("chunk-0")==FEISHU_FDB_OK);
    reopen();feishu_fdb_close();
    /* Erase error during blank preparation is terminal and not hidden. */
    memset(flash,0xff,sizeof(flash));reset_faults();erase_cut=0;
    assert(feishu_fdb_open(&io,true)==FEISHU_FDB_IO && tripped && !blocked_writes);
    reset_faults();assert(feishu_fdb_open(&io,true)==FEISHU_FDB_OK);
    feishu_fdb_close();
    /* Cross the former four-sector-cache boundary without turning every
     * allocation/status query into another full-payload scan. */
    memset(flash,0xff,sizeof(flash));reset_faults();
    assert(feishu_fdb_open(&io,true)==FEISHU_FDB_OK);read_bytes_total=0;
    for(unsigned i=0;i<140;++i) {
        char key[32];snprintf(key,sizeof(key),"perf_%u",i);
        assert(feishu_fdb_put(key,data,512)==FEISHU_FDB_OK);
    }
    size_t admission_reads=read_bytes_total;
    assert(admission_reads<512u*1024u);
    unsigned physical_before=reads;cooperation_count=visited_count=0;
    assert(feishu_fdb_visit(visited,NULL)==FEISHU_FDB_OK && visited_count==140);
#ifndef FDB_READ_CACHE_MALLOC
    assert(cooperation_count>reads-physical_before); /* Cache hits also cooperate. */
#else
    assert(cooperation_count>0); /* Deliberately denied cache: direct reads cooperate. */
#endif
    printf("FlashDB cooperative cache scan: logical=%u physical=%u records=%u PASS\n",cooperation_count,reads-physical_before,visited_count);
    feishu_fdb_close();feishu_fdb_set_cooperate(NULL);
    printf("FlashDB raw guard: %u open read faults, %u get read faults, %u mutation read faults, %u interrupted writes, %u full-store/GC write faults, full=%d, 140 admissions read=%zu bytes, reopen/protection PASS\n",open_cases,get_cases,operation_read_cases,write_cases,gc_faults,count,admission_reads);
    return 0;
}

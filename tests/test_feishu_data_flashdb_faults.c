#include "feishu_data.h"
#include "feishu_data_flashdb.h"
#include "feishu_fdb_host.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static feishu_data_t store;
static void stage(const char *key,const char *text,feishu_data_writer_t *w)
{
    assert(feishu_data_begin(&store,key,FEISHU_DATA_REQUIRED,true,w)==FEISHU_DATA_OK);
    assert(feishu_data_append(&store,w,(const uint8_t *)text,strlen(text)+1,true)==FEISHU_DATA_OK);
}
static void value(const char *key,char *out)
{
    feishu_data_object_t o;size_t n=0;
    assert(feishu_data_find(&store,key,&o)==FEISHU_DATA_OK && o.protected_data);
    assert(feishu_data_read(&store,o.id,0,(uint8_t *)out,64,&n)==FEISHU_DATA_OK && n<64 && out[n-1]==0);
}
static void old_pair(void)
{
    char a[64],b[64];value("draft/body",a);value("draft/state",b);
    assert(!strcmp(a,"old body") && !strcmp(b,"old state"));
}
static void consistent_pair(void)
{
    char a[64],b[64];value("draft/body",a);value("draft/state",b);
    assert((!strcmp(a,"old body") && !strcmp(b,"old state")) ||
           (!strcmp(a,"new body") && !strcmp(b,"new state")));
    value("owner",a);assert(!strcmp(a,"retained owner"));
}
static void restore(void)
{
    assert(feishu_data_close(&store)==FEISHU_DATA_OK);
    feishu_fdb_host_restore();
    assert(feishu_data_open(&store,"pool",NULL)==FEISHU_DATA_OK);
    old_pair();
}
int main(void)
{
    const feishu_fdb_io_t *io=feishu_fdb_host_reset();
    assert(feishu_fdb_open(io,true)==FEISHU_FDB_OK);feishu_fdb_close();
    assert(feishu_data_flashdb_configure(io)==FEISHU_DATA_OK);
    assert(feishu_data_flashdb_initialize()==FEISHU_DATA_OK);
    assert(feishu_data_open(&store,"pool",NULL)==FEISHU_DATA_OK);
    feishu_data_writer_t a,b;
    stage("owner","retained owner",&a);assert(feishu_data_commit(&store,&a)==FEISHU_DATA_OK);
    stage("draft/body","old body",&a);stage("draft/state","old state",&b);
    assert(feishu_data_commit_pair(&store,&a,NULL,&b,NULL,0)==FEISHU_DATA_OK);
    feishu_fdb_host_checkpoint();
    restore();
    stage("stage/body","new body",&a);stage("stage/state","new state",&b);
    feishu_fdb_host_fault(-1,-1,false);
    assert(feishu_data_commit_pair(&store,&a,"draft/body",&b,"draft/state",0)==FEISHU_DATA_OK);
    unsigned reads=feishu_fdb_host_reads(),writes=feishu_fdb_host_writes();
    unsigned read_cases=0,write_cases=0;
    for(int mode=0;mode<2;++mode)for(int partial=0;partial<2;++partial) {
        unsigned total=mode?writes:reads;
        for(unsigned cut=0;cut<total;++cut) {
            restore();stage("stage/body","new body",&a);stage("stage/state","new state",&b);
            feishu_data_writer_t before_a=a,before_b=b;
            feishu_fdb_host_fault(mode?-1:(int)cut,mode?(int)cut:-1,partial!=0);
            int rc=feishu_data_commit_pair(&store,&a,"draft/body",&b,"draft/state",0);
            assert(feishu_fdb_host_tripped() && rc==FEISHU_DATA_IOERR);
            assert(!memcmp(&a,&before_a,sizeof(a)) && !memcmp(&b,&before_b,sizeof(b)));
            unsigned after=feishu_fdb_host_writes();
            assert(feishu_data_commit_pair(&store,&a,"draft/body",&b,"draft/state",0)==FEISHU_DATA_IOERR);
            assert(feishu_fdb_host_writes()==after);
            assert(feishu_data_close(&store)==FEISHU_DATA_OK);
            feishu_fdb_host_fault(-1,-1,false);
            assert(feishu_data_open(&store,"pool",NULL)==FEISHU_DATA_OK);
            consistent_pair();
            if(mode)++write_cases;else ++read_cases;
        }
    }
    restore();stage("unfinished","must not be visible",&a);
    assert(feishu_data_close(&store)==FEISHU_DATA_OK);
    assert(feishu_data_open(&store,"pool",NULL)==FEISHU_DATA_OK);
    feishu_data_object_t obj;
    assert(feishu_data_find(&store,"unfinished",&obj)==FEISHU_DATA_NOTFOUND);
    for(int i=0;i<40;++i){size_t n;int rc=feishu_data_reclaim(&store,&n);if(rc==FEISHU_DATA_FULL)break;assert(rc==FEISHU_DATA_OK);}
    old_pair();consistent_pair();
    assert(feishu_data_close(&store)==FEISHU_DATA_OK);
    printf("FlashDB object publication: %u read failures, %u interrupted writes, atomic old/new pairs, reboot garbage and protected owner PASS\n",read_cases,write_cases);
    return 0;
}

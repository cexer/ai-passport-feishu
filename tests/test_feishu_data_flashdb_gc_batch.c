#include "feishu_data.h"
#include "feishu_data_flashdb.h"
#include "feishu_fdb_host.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static feishu_data_t store;
static int64_t ids[20],owner_id;
static void reopen(void)
{
    assert(feishu_data_close(&store)==FEISHU_DATA_OK);
    feishu_fdb_host_fault(-1,-1,false);
    assert(feishu_data_open(&store,"pool",NULL)==FEISHU_DATA_OK);
    assert(feishu_data_pin(&store,ids[0],true)==FEISHU_DATA_OK);
    assert(feishu_data_pin(&store,ids[19],true)==FEISHU_DATA_OK);
}
static void protected_ok(void)
{
    feishu_data_object_t o;uint8_t b[16];size_t n=0;
    assert(feishu_data_find(&store,"owner",&o)==FEISHU_DATA_OK && o.id==owner_id && o.protected_data);
    assert(feishu_data_read(&store,o.id,0,b,sizeof(b),&n)==FEISHU_DATA_OK && n==6 && !memcmp(b,"owner",6));
    assert(feishu_data_read(&store,ids[0],0,b,sizeof(b),&n)==FEISHU_DATA_OK && n==2 && b[0]==0);
    assert(feishu_data_find(&store,"cache",&o)==FEISHU_DATA_OK && o.id==ids[19]);
    assert(feishu_data_read(&store,o.id,0,b,sizeof(b),&n)==FEISHU_DATA_OK && n==2 && b[0]==19);
}
static unsigned missing(void)
{
    unsigned n=0;feishu_data_object_t o;
    for(unsigned i=1;i<=16;++i)if(feishu_data_stat(&store,ids[i],"cache",&o)==FEISHU_DATA_NOTFOUND)++n;
    return n;
}
static void restore(void)
{
    assert(feishu_data_close(&store)==FEISHU_DATA_OK);feishu_fdb_host_restore();reopen();protected_ok();
}
int main(void)
{
    const feishu_fdb_io_t *io=feishu_fdb_host_reset();
    assert(feishu_fdb_open(io,true)==FEISHU_FDB_OK);feishu_fdb_close();
    assert(feishu_data_flashdb_configure(io)==FEISHU_DATA_OK);
    assert(feishu_data_flashdb_initialize()==FEISHU_DATA_OK);
    assert(feishu_data_open(&store,"pool",NULL)==FEISHU_DATA_OK);
    feishu_data_writer_t w={0};
    assert(feishu_data_begin(&store,"owner",FEISHU_DATA_REQUIRED,true,&w)==FEISHU_DATA_OK);
    assert(feishu_data_append(&store,&w,(const uint8_t *)"owner",6,false)==FEISHU_DATA_OK);owner_id=w.id;
    assert(feishu_data_commit(&store,&w)==FEISHU_DATA_OK);
    for(unsigned i=0;i<20;++i) {
        uint8_t b[2]={(uint8_t)i,0};
        assert(feishu_data_begin(&store,"cache",FEISHU_DATA_TEXT,false,&w)==FEISHU_DATA_OK);
        assert(feishu_data_append(&store,&w,b,sizeof(b),false)==FEISHU_DATA_OK);ids[i]=w.id;
        assert(feishu_data_commit(&store,&w)==FEISHU_DATA_OK);
    }
    /* Remove only obsolete staging names so the first reclaim exercises the
     * atomic catalog batch rather than pre-existing raw orphans. */
    char key[40];snprintf(key,sizeof(key),"k%016llx.00000000",(unsigned long long)owner_id);
    assert(feishu_fdb_delete(key)==FEISHU_FDB_OK);
    for(unsigned i=0;i<20;++i){snprintf(key,sizeof(key),"k%016llx.00000000",(unsigned long long)ids[i]);assert(feishu_fdb_delete(key)==FEISHU_FDB_OK);}
    reopen();feishu_fdb_host_checkpoint();
    size_t bytes=0;feishu_fdb_host_fault(-1,-1,false);
    assert(feishu_data_reclaim(&store,&bytes)==FEISHU_DATA_OK && !bytes && missing()==16);
    unsigned catalog_writes=feishu_fdb_host_writes();protected_ok();
    for(unsigned cut=0;cut<catalog_writes;++cut) {
        restore();feishu_fdb_host_fault(-1,(int)cut,true);
        assert(feishu_data_reclaim(&store,&bytes)==FEISHU_DATA_IOERR);
        reopen();protected_ok();unsigned n=missing();assert(n==0 || n==16);
    }
    restore();assert(feishu_data_reclaim(&store,&bytes)==FEISHU_DATA_OK && !bytes && missing()==16);
    feishu_fdb_host_checkpoint();feishu_fdb_host_fault(-1,-1,false);
    assert(feishu_data_reclaim(&store,&bytes)==FEISHU_DATA_OK && bytes>=40);
    unsigned deletion_writes=feishu_fdb_host_writes();protected_ok();
    for(unsigned cut=0;cut<deletion_writes;++cut) {
        restore();feishu_fdb_host_fault(-1,(int)cut,true);
        assert(feishu_data_reclaim(&store,&bytes)==FEISHU_DATA_IOERR);
        unsigned writes=feishu_fdb_host_writes();
        assert(feishu_data_reclaim(&store,&bytes)==FEISHU_DATA_IOERR && feishu_fdb_host_writes()==writes);
        reopen();protected_ok();
    }
    restore();feishu_fdb_host_fault(0,-1,false);
    assert(feishu_data_reclaim(&store,&bytes)==FEISHU_DATA_IOERR && !feishu_fdb_host_writes());reopen();protected_ok();
    assert(feishu_data_close(&store)==FEISHU_DATA_OK);
    printf("FlashDB bounded GC: atomic masked eviction, multi-orphan cleanup, %u interrupted writes, read fault and protected-reader recovery PASS\n",catalog_writes+deletion_writes);
    return 0;
}

#include "feishu_data.h"
#include "feishu_data_flashdb.h"
#include "feishu_fdb_host.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static feishu_data_t store;
static void latest(int64_t expected)
{
    feishu_data_object_t o;uint8_t text[8];size_t n=0;
    assert(feishu_data_find(&store,"protected-latest",&o)==FEISHU_DATA_OK);
    assert(o.id==expected && o.protected_data);
    assert(feishu_data_read(&store,o.id,0,text,sizeof(text),&n)==FEISHU_DATA_OK && n==2 && text[0]==127);
}
int main(void)
{
    const feishu_fdb_io_t *io=feishu_fdb_host_reset();
    assert(feishu_fdb_open(io,true)==FEISHU_FDB_OK);feishu_fdb_close();
    assert(feishu_data_flashdb_configure(io)==FEISHU_DATA_OK);
    assert(feishu_data_flashdb_initialize()==FEISHU_DATA_OK);
    assert(feishu_data_open(&store,"pool",NULL)==FEISHU_DATA_OK);
    int64_t ids[128];
    for(unsigned i=0;i<128;++i) {
        feishu_data_writer_t w={0};uint8_t text[2]={(uint8_t)i,0};
        assert(feishu_data_begin(&store,"protected-latest",FEISHU_DATA_REQUIRED,true,&w)==FEISHU_DATA_OK);
        assert(feishu_data_append(&store,&w,text,sizeof(text),true)==FEISHU_DATA_OK);
        ids[i]=w.id;assert(feishu_data_commit(&store,&w)==FEISHU_DATA_OK);
        assert(feishu_data_pin(&store,ids[i],true)==FEISHU_DATA_OK);
    }
    feishu_data_writer_t w={0};
    feishu_fdb_host_fault(-1,-1,false);
    assert(feishu_data_begin(&store,"next",FEISHU_DATA_REQUIRED,true,&w)==FEISHU_DATA_FULL);
    assert(!feishu_fdb_host_writes()); /* Every historical version is in use. */
    assert(feishu_data_pin(&store,ids[1],false)==FEISHU_DATA_OK);
    /* A failed begin mutation cannot make the current protected version disappear. */
    feishu_fdb_host_fault(-1,0,false);
    assert(feishu_data_begin(&store,"next",FEISHU_DATA_REQUIRED,true,&w)==FEISHU_DATA_IOERR);
    assert(feishu_data_close(&store)==FEISHU_DATA_OK);
    feishu_fdb_host_fault(-1,-1,false);
    assert(feishu_data_open(&store,"pool",NULL)==FEISHU_DATA_OK);latest(ids[127]);
    for(unsigned i=0;i<128;++i)if(i!=1)assert(feishu_data_pin(&store,ids[i],true)==FEISHU_DATA_OK);
    assert(feishu_data_begin(&store,"next",FEISHU_DATA_REQUIRED,true,&w)==FEISHU_DATA_OK);
    assert(w.id>ids[127]);latest(ids[127]);
    uint8_t old[8];size_t n=0;
    assert(feishu_data_read(&store,ids[0],0,old,sizeof(old),&n)==FEISHU_DATA_OK && n==2 && old[0]==0);
    assert(feishu_data_append(&store,&w,(const uint8_t *)"new",4,true)==FEISHU_DATA_OK);
    /* Reset before publication: staging bytes never replace the latest body. */
    assert(feishu_data_close(&store)==FEISHU_DATA_OK);
    assert(feishu_data_open(&store,"pool",NULL)==FEISHU_DATA_OK);latest(ids[127]);
    assert(feishu_data_close(&store)==FEISHU_DATA_OK);
    puts("FlashDB catalog slots: full/pinned rejection, retired reuse, begin-write fault, protected latest and pre-publication reset PASS");
    return 0;
}

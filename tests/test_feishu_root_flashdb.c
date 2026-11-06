#include "feishu_root_flashdb.h"
#include "feishu_fdb_host.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static const feishu_root_t root={.directory="raw"};
static uint8_t old_data[1800],new_data[900],large[FEISHU_ROOT_MAX],out[FEISHU_ROOT_MAX];
static const feishu_fdb_io_t *io;
static void reopen(void)
{
    feishu_fdb_close();feishu_fdb_host_fault(-1,-1,false);
    assert(feishu_fdb_open(io,false)==FEISHU_FDB_OK);
}
static void expect(unsigned key,const uint8_t *data,size_t size)
{
    size_t n=0;memset(out,0,sizeof(out));
    assert(feishu_root_read(&root,key,0,out,size,&n)==FEISHU_ROOT_OK && n==size && !memcmp(out,data,size));
}
int main(void)
{
    io=feishu_fdb_host_reset();assert(feishu_fdb_open(io,true)==FEISHU_FDB_OK);
    for(size_t i=0;i<sizeof(old_data);++i)old_data[i]=(uint8_t)(i*17+9);
    for(size_t i=0;i<sizeof(new_data);++i)new_data[i]=(uint8_t)(i*7+3);
    memset(old_data+1000,0,500);
    feishu_root_part_t parts[]={{old_data,1000},{NULL,500},{old_data+1500,300}};
    assert(feishu_root_write(&root,7,parts,3)==FEISHU_ROOT_OK);expect(7,old_data,sizeof(old_data));
    size_t n=0;
    assert(feishu_root_read(&root,7,777,out,700,&n)==FEISHU_ROOT_OK && n==1800 && !memcmp(out,old_data+777,700));
    feishu_fdb_host_fault(-1,-1,false);
    assert(feishu_root_write(&root,7,parts,3)==FEISHU_ROOT_OK && !feishu_fdb_host_writes());
    /* Independent payload copies: corrupting A still leaves B readable. */
    uint8_t block[512];memset(block,0,sizeof(block));
    assert(feishu_fdb_put("r07a/0000000000000001/00",block,sizeof(block))==FEISHU_FDB_OK);
    expect(7,old_data,sizeof(old_data));
    assert(feishu_root_write(&root,7,parts,3)==FEISHU_ROOT_OK);expect(7,old_data,sizeof(old_data));
    feishu_fdb_host_checkpoint();
    feishu_fdb_host_fault(-1,-1,false);expect(7,old_data,sizeof(old_data));
    unsigned read_count=feishu_fdb_host_reads(),read_faults=0;
    for(int partial=0;partial<2;++partial)for(unsigned cut=0;cut<read_count;++cut) {
        feishu_fdb_close();feishu_fdb_host_restore();assert(feishu_fdb_open(io,false)==FEISHU_FDB_OK);
        feishu_fdb_host_fault((int)cut,-1,partial!=0);size_t total=0;
        assert(feishu_root_read(&root,7,0,out,sizeof(old_data),&total)==FEISHU_ROOT_IO);
        assert(feishu_fdb_host_tripped() && !feishu_fdb_host_writes());
        reopen();expect(7,old_data,sizeof(old_data));++read_faults;
    }
    feishu_root_part_t replacement={new_data,sizeof(new_data)};
    feishu_fdb_host_fault(-1,-1,false);
    assert(feishu_root_write(&root,7,&replacement,1)==FEISHU_ROOT_OK);
    unsigned writes=feishu_fdb_host_writes(),faults=0,verified_successes=0;
    for(int partial=0;partial<2;++partial)for(unsigned cut=0;cut<writes;++cut) {
        feishu_fdb_close();feishu_fdb_host_restore();assert(feishu_fdb_open(io,false)==FEISHU_FDB_OK);
        feishu_fdb_host_fault(-1,(int)cut,partial!=0);
        int rc=feishu_root_write(&root,7,&replacement,1);
        assert(feishu_fdb_host_tripped());
        assert(rc==FEISHU_ROOT_IO || rc==FEISHU_ROOT_OK);
        if(!rc)++verified_successes;
        reopen();n=0;
        assert(feishu_root_read(&root,7,0,NULL,0,&n)==FEISHU_ROOT_OK);
        assert(n==sizeof(old_data) || n==sizeof(new_data));
        expect(7,n==sizeof(old_data)?old_data:new_data,n);++faults;
        if(!rc)assert(n==sizeof(new_data));
    }
    reopen();
    for(size_t i=0;i<sizeof(large);++i)large[i]=(uint8_t)(i*19);
    feishu_root_part_t max_part={large,sizeof(large)};
    assert(feishu_root_write(&root,9,&max_part,1)==FEISHU_ROOT_OK);expect(9,large,sizeof(large));
    /* Total generated payload exceeds the complete pool: automatic cleanup
     * must retain both the active write and the other live configuration. */
    for(unsigned version=0;version<24;++version) {
        large[0]=(uint8_t)version;
        assert(feishu_root_write(&root,9,&max_part,1)==FEISHU_ROOT_OK);
        expect(9,large,sizeof(large));
    }
    /* Chunk collection shares KV storage but cannot remove a live generation. */
    for(unsigned version=0;version<12;++version) {
        new_data[0]=(uint8_t)version;
        assert(feishu_root_write(&root,7,&replacement,1)==FEISHU_ROOT_OK);
    }
    unsigned collected=0;
    for(unsigned i=0;i<4096;++i){size_t bytes=0;int rc=feishu_root_flashdb_collect(&bytes);
        if(rc==FEISHU_ROOT_MISSING)break;assert(rc==FEISHU_ROOT_OK && bytes);++collected;}
    assert(collected);expect(7,new_data,sizeof(new_data));expect(9,large,sizeof(large));
    size_t none=0;feishu_fdb_host_fault(-1,-1,false);
    assert(feishu_root_flashdb_collect(&none)==FEISHU_ROOT_MISSING && !feishu_fdb_host_reads());
    assert(feishu_fdb_put("object-test",new_data,32)==FEISHU_FDB_OK);
    feishu_fdb_host_fault(-1,-1,false);
    assert(feishu_root_flashdb_collect(&none)==FEISHU_ROOT_MISSING && !feishu_fdb_host_reads());
    reopen();feishu_fdb_host_fault(-1,-1,false);
    assert(feishu_root_flashdb_collect(&none)==FEISHU_ROOT_MISSING && feishu_fdb_host_reads());
    new_data[0]^=1;assert(feishu_root_write(&root,7,&replacement,1)==FEISHU_ROOT_OK);
    assert(feishu_root_flashdb_collect(&none)==FEISHU_ROOT_OK && none);expect(7,new_data,sizeof(new_data));
    assert(feishu_root_write(&root,11,NULL,0)==FEISHU_ROOT_OK);
    assert(feishu_root_read(&root,11,0,NULL,0,&n)==FEISHU_ROOT_OK && !n);
    /* Two damaged logical manifests cannot authorize an implicit reset. */
    memset(block,0,32);
    assert(feishu_fdb_put("r07a",block,32)==FEISHU_FDB_OK);
    assert(feishu_fdb_put("r07b",block,32)==FEISHU_FDB_OK);
    feishu_fdb_host_fault(-1,-1,false);
    assert(feishu_root_write(&root,7,&replacement,1)==FEISHU_ROOT_CORRUPT && !feishu_fdb_host_writes());
    feishu_fdb_close();
    printf("FlashDB root: independent mirrors, %u read failures, zero parts, slices, no-op saves, %u interrupted writes (%u verified commits), 16 KiB record, %u obsolete chunks and recovery PASS\n",read_faults,faults,verified_successes,collected);
    return 0;
}

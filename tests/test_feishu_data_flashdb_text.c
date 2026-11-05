#include "feishu_data.h"
#include "feishu_data_flashdb.h"
#include "feishu_fdb_host.h"
#include "feishu_text_map.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static feishu_data_t store;
static feishu_data_writer_t body,map;
static int64_t body_id,map_id;
static char raw[20000],expected[40000];
static feishu_data_text_point_t points[8];
static size_t count;
static bool building;
static bool read_raw(void *ctx,uint64_t offset,void *out,size_t capacity,size_t *size)
{
    (void)ctx;assert(offset%1024==0);
    return feishu_data_read(&store,body_id,(unsigned)(offset/1024),out,capacity,size)==FEISHU_DATA_OK;
}
static int lookup(void *ctx,const char *key,void *out,size_t capacity,size_t *size)
{
    (void)ctx;int rc=feishu_data_mention_get(&store,map_id,key,out,capacity,size);
    if(rc==FEISHU_DATA_OK)return 1;
    if(rc==FEISHU_DATA_DONE)return 0;
    if(rc==FEISHU_DATA_NOTFOUND && building && feishu_data_mention_put(&store,&map,key,NULL)==FEISHU_DATA_OK)return 0;
    return -1;
}
static bool flush(void)
{
    if(!count)return true;
    int rc=feishu_data_text_index(&store,&body,&map,points,count);
    count=0;return rc==FEISHU_DATA_OK;
}
static bool checkpoint(void *ctx,uint64_t r,uint64_t l)
{(void)ctx;points[count++]=(feishu_data_text_point_t){r,l};return count<8 || flush();}
int main(void)
{
    const feishu_fdb_io_t *media=feishu_fdb_host_reset();
    assert(feishu_fdb_open(media,true)==FEISHU_FDB_OK);feishu_fdb_close();
    assert(feishu_data_flashdb_configure(media)==FEISHU_DATA_OK);
    assert(feishu_data_flashdb_initialize()==FEISHU_DATA_OK);
    assert(feishu_data_open(&store,"pool",NULL)==FEISHU_DATA_OK);
    assert(feishu_data_begin(&store,"names",FEISHU_DATA_METADATA,false,&map)==FEISHU_DATA_OK);
    for(unsigned i=1;i<=32;++i) {
        char key[32],name[128];snprintf(key,sizeof(key),"@_user_%u",i);snprintf(name,sizeof(name),"人员%02u😀",i);
        assert(feishu_data_mention_put(&store,&map,key,name)==FEISHU_DATA_OK);
        assert(feishu_data_mention_put(&store,&map,key,name)==FEISHU_DATA_OK);
        assert(feishu_data_mention_put(&store,&map,key,"conflict")==FEISHU_DATA_CONSTRAINT);
    }
    assert(feishu_data_mention_put(&store,&map,"@number","00123")==FEISHU_DATA_OK);
    size_t nr=1022,ne=1022;memset(raw,'x',nr);memset(expected,'x',ne);
    for(unsigned round=0;round<12;++round) {
        for(unsigned i=1;i<=32;++i) {
            nr+=(size_t)snprintf(raw+nr,sizeof(raw)-nr,"@_user_%u ",i);
            ne+=(size_t)snprintf(expected+ne,sizeof(expected)-ne,"@人员%02u😀 ",i);
        }
        nr+=(size_t)snprintf(raw+nr,sizeof(raw)-nr,"@_user_1000 literal\n");
        ne+=(size_t)snprintf(expected+ne,sizeof(expected)-ne,"@_user_1000 literal\n");
    }
    assert(feishu_data_begin(&store,"body-stage",FEISHU_DATA_TEXT,false,&body)==FEISHU_DATA_OK);
    for(size_t at=0;at<nr;at+=1024) {
        size_t n=nr-at;if(n>1024)n=1024;
        assert(feishu_data_append(&store,&body,(uint8_t *)raw+at,n,true)==FEISHU_DATA_OK);
    }
    body_id=body.id;map_id=map.id;building=true;
    feishu_text_map_io_t io={read_raw,lookup,checkpoint,NULL,NULL};
    char preview[256];uint8_t workspace[1024];uint64_t logical=0;
    assert(feishu_text_map_scan(&io,nr,workspace,sizeof(workspace),preview,sizeof(preview),&logical));
    assert(flush() && logical==ne && !memcmp(preview,expected,strlen(preview)));
    assert(feishu_data_text_publish(&store,&body,&map,logical,"message")==FEISHU_DATA_OK);
    building=false;
    feishu_data_text_view_t view;
    for(int reboot=0;reboot<2;++reboot) {
        assert(feishu_data_text_info(&store,body_id,nr,&view)==FEISHU_DATA_OK && view.map==map_id && view.logical==ne);
        for(uint64_t offset=0;offset<ne;offset+=317) {
            feishu_data_text_point_t point;uint8_t out[600];size_t n=0;
            assert(feishu_data_text_seek(&store,body_id,&view,offset,&point)==FEISHU_DATA_OK);
            assert(feishu_text_map_read(&io,nr,point.raw,point.logical,offset,out,sizeof(out),workspace,sizeof(workspace),&n));
            assert(n<=ne-offset && !memcmp(out,expected+offset,n));
        }
        assert(feishu_data_pin(&store,body_id,true)==FEISHU_DATA_OK);
        for(unsigned i=0;i<80;++i){size_t n;int rc=feishu_data_reclaim(&store,&n);if(rc==FEISHU_DATA_FULL)break;assert(rc==FEISHU_DATA_OK);}
        assert(feishu_data_text_info(&store,body_id,nr,&view)==FEISHU_DATA_OK);
        assert(feishu_data_close(&store)==FEISHU_DATA_OK);
        assert(feishu_data_open(&store,"pool",NULL)==FEISHU_DATA_OK);
    }
    /* A corrupted view cannot redirect reads or collection into another map. */
    char key[64];snprintf(key,sizeof(key),"v%016llx.%016llx",(unsigned long long)body_id,(unsigned long long)map_id);
    uint8_t saved[64],bad[64];size_t saved_size=0;
    assert(feishu_fdb_get(key,saved,sizeof(saved),&saved_size)==FEISHU_FDB_OK);
    memcpy(bad,saved,saved_size);bad[12]^=1;
    assert(feishu_fdb_put(key,bad,saved_size)==FEISHU_FDB_OK);
    assert(feishu_data_text_info(&store,body_id,nr,&view)==FEISHU_DATA_CORRUPT && !feishu_data_broken(&store));
    assert(feishu_fdb_put(key,saved,saved_size)==FEISHU_FDB_OK);
    assert(feishu_data_text_info(&store,body_id,nr,&view)==FEISHU_DATA_OK);
    assert(feishu_data_close(&store)==FEISHU_DATA_OK);
    puts("FlashDB text: 32 names, numeric/literal markers, block boundaries, sparse seeks, pinned metadata, reopen and corrupt-view recovery PASS");
    return 0;
}

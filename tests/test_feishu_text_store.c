#include "feishu_data.h"
#include "feishu_text_map.h"
#include "sqlite3.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifndef FEISHU_TEXT_STORE_YIELD
#define FEISHU_TEXT_STORE_YIELD() ((void)0)
#endif
#ifndef FEISHU_TEXT_STORE_STAGE
#define FEISHU_TEXT_STORE_STAGE(name) ((void)0)
#endif

typedef struct {
    feishu_data_t *store;
    feishu_data_writer_t *body,*map;
    int64_t body_id,map_id;
    bool build;
    feishu_data_text_point_t points[8];size_t count;
} context_t;
static bool read_raw(void *context,uint64_t offset,void *data,size_t capacity,size_t *size)
{
    context_t *c=context;assert(!(offset%1024));
    return feishu_data_read(c->store,c->body_id,(unsigned)(offset/1024),data,capacity,size)==SQLITE_OK;
}
static int lookup(void *context,const char *key,void *value,size_t capacity,size_t *size)
{
    context_t *c=context;int rc=feishu_data_mention_get(c->store,c->map_id,key,value,capacity,size);
    if(rc==SQLITE_OK)return 1;
    if(rc==SQLITE_DONE)return 0;
    if(rc==SQLITE_NOTFOUND && c->build && feishu_data_mention_put(c->store,c->map,key,NULL)==SQLITE_OK)return 0;
    return -1;
}
static bool flush(context_t *c)
{
    if(!c->count)return true;
    int rc=feishu_data_text_index(c->store,c->body,c->map,c->points,c->count);
    if(rc)fprintf(stderr,"index rc=%d %s\n",rc,sqlite3_errmsg(c->store->db));
    if(rc!=SQLITE_OK)return false;
    c->count=0;return true;
}
static bool checkpoint(void *context,uint64_t raw,uint64_t logical)
{
    context_t *c=context;c->points[c->count++]=(feishu_data_text_point_t){raw,logical};
    return c->count<8 || flush(c);
}
static void execute(feishu_data_t *s,const char *sql)
{ int rc=sqlite3_exec(s->db,sql,NULL,NULL,NULL);if(rc)fprintf(stderr,"SQL rc=%d %s\n",rc,sql);assert(rc==SQLITE_OK); }
static int64_t scalar(feishu_data_t *s,const char *sql)
{
    sqlite3_stmt *query=NULL;assert(sqlite3_prepare_v2(s->db,sql,-1,&query,NULL)==SQLITE_OK);
    assert(sqlite3_step(query)==SQLITE_ROW);int64_t result=sqlite3_column_int64(query,0);
    assert(sqlite3_finalize(query)==SQLITE_OK);return result;
}
static int deny_object_delete(void *context,int action,const char *table,const char *column,const char *database,const char *trigger)
{
    (void)context;(void)column;(void)database;(void)trigger;
    return action==SQLITE_DELETE && table && !strcmp(table,"objects") ? SQLITE_DENY:SQLITE_OK;
}
static char raw[20000],expected[40000];
static void build_body(feishu_data_t *s,feishu_data_writer_t *body,const char *key,const char *data,size_t size)
{
    assert(feishu_data_begin(s,key,FEISHU_DATA_TEXT,false,body)==SQLITE_OK);
    for(size_t at=0;at<size;) {
        size_t n=size-at;if(n>1024)n=1024;
        assert(feishu_data_append(s,body,(const uint8_t *)data+at,n,true)==SQLITE_OK);at+=n;
    }
}
int main(int argc,char **argv)
{
    assert(argc==2);feishu_data_t store={0};assert(feishu_data_open(&store,argv[1],NULL)==SQLITE_OK);
    feishu_data_writer_t guard,body,map;
    assert(feishu_data_begin(&store,"guard",FEISHU_DATA_REQUIRED,true,&guard)==SQLITE_OK);
    assert(feishu_data_append(&store,&guard,(const uint8_t *)"protected",10,true)==SQLITE_OK);
    assert(feishu_data_commit(&store,&guard)==SQLITE_OK);
    memset(raw,'a',1022);memset(expected,'a',1022);size_t nr=1022,ne=1022;
    assert(feishu_data_begin(&store,"mentions-stage",FEISHU_DATA_METADATA,false,&map)==SQLITE_OK);
    for(unsigned i=1;i<=32;++i) {
        FEISHU_TEXT_STORE_YIELD();
        char key[32],name[128];snprintf(key,sizeof(key),"@_user_%u",i);snprintf(name,sizeof(name),"人员%02u😀",i);
        int rc=feishu_data_mention_put(&store,&map,key,name);
        if(rc)fprintf(stderr,"mention %u rc=%d\n",i,rc);
        assert(rc==SQLITE_OK);
    }
    assert(map.stored>40 && map.stored<128*1024);
    FEISHU_TEXT_STORE_STAGE("names_ready");
    assert(feishu_data_mention_put(&store,&map,"@number","00123")==SQLITE_OK);
    char numeric[8];size_t numeric_size=0;
    assert(feishu_data_mention_get(&store,map.id,"@number",numeric,sizeof(numeric),&numeric_size)==SQLITE_OK &&
        numeric_size==5 && !memcmp(numeric,"00123",5));
    assert(feishu_data_mention_put(&store,&map,"@_user_1","人员01😀")==SQLITE_OK);
    assert(feishu_data_mention_put(&store,&map,"@_user_1","conflict")==SQLITE_CONSTRAINT);
    for(unsigned round=0;round<12;++round) {
        for(unsigned i=1;i<=32;++i) {
            nr+=(size_t)snprintf(raw+nr,sizeof(raw)-nr,"@_user_%u ",i);
            ne+=(size_t)snprintf(expected+ne,sizeof(expected)-ne,"@人员%02u😀 ",i);
        }
        nr+=(size_t)snprintf(raw+nr,sizeof(raw)-nr,"@_user_1000 未知\n");
        ne+=(size_t)snprintf(expected+ne,sizeof(expected)-ne,"@_user_1000 未知\n");
    }
    build_body(&store,&body,"body-stage",raw,nr);
    context_t context={&store,&body,&map,body.id,map.id,true,{{0}},0};
    feishu_text_map_io_t io={read_raw,lookup,checkpoint,NULL,&context};
    uint8_t workspace[1024];char preview[256];uint64_t logical=0;
    assert(feishu_text_map_scan(&io,nr,workspace,sizeof(workspace),preview,sizeof(preview),&logical));
    assert(flush(&context) && logical==ne && !memcmp(preview,expected,strlen(preview)));
    FEISHU_TEXT_STORE_STAGE("scan_ready");
    int64_t id=body.id,nonce=map.id;
    assert(feishu_data_text_publish(&store,&body,&map,logical,"account/text/msg")==SQLITE_OK && !body.id && !map.id);
    feishu_data_object_t object;assert(feishu_data_find(&store,"account/text/msg",&object)==SQLITE_OK && object.id==id && object.raw==nr);
    assert(feishu_data_stat(&store,id,"another-account/text/msg",&object)==SQLITE_NOTFOUND);
    feishu_data_text_view_t view;assert(feishu_data_text_info(&store,id,nr,&view)==SQLITE_OK && view.logical==ne && view.map==nonce);
    size_t value_size=0;char value[128];
    assert(feishu_data_mention_get(&store,nonce,"@_user_1000",value,sizeof(value),&value_size)==SQLITE_DONE);
    assert(feishu_data_mention_get(&store,nonce,"@absent",value,sizeof(value),&value_size)==SQLITE_NOTFOUND);
    assert(feishu_data_pin(&store,id,true)==SQLITE_OK);size_t freed;
    while(feishu_data_reclaim(&store,&freed)==SQLITE_OK) {FEISHU_TEXT_STORE_YIELD();}
    assert(feishu_data_mention_get(&store,nonce,"@_user_32",value,sizeof(value),&value_size)==SQLITE_OK);
    context.build=false;
    for(size_t offset=0;offset<=ne;offset+=137) {
        FEISHU_TEXT_STORE_YIELD();
        feishu_data_text_point_t point;assert(feishu_data_text_seek(&store,id,&view,offset,&point)==SQLITE_OK);
        uint8_t bytes[260];size_t size=0;
        assert(feishu_text_map_read(&io,nr,point.raw,point.logical,offset,bytes,sizeof(bytes),workspace,sizeof(workspace),&size));
        size_t want=ne-offset;if(want>sizeof(bytes))want=sizeof(bytes);
        assert(size==want && !memcmp(bytes,expected+offset,want));
    }
    FEISHU_TEXT_STORE_STAGE("views_ready");
    assert(feishu_data_pin(&store,id,false)==SQLITE_OK && feishu_data_close(&store)==SQLITE_OK);
    assert(feishu_data_open(&store,argv[1],NULL)==SQLITE_OK);
    assert(feishu_data_text_info(&store,id,nr,&view)==SQLITE_OK && view.logical==ne);
    char sql[256];snprintf(sql,sizeof(sql),"UPDATE text_aux SET crc=crc+1 WHERE owner=%lld AND kind=0 AND k='@_user_1'",(long long)nonce);
    execute(&store,sql);
    assert(feishu_data_mention_get(&store,nonce,"@_user_1",value,sizeof(value),&value_size)==SQLITE_CORRUPT);
    snprintf(sql,sizeof(sql),"UPDATE text_aux SET crc=crc-1 WHERE owner=%lld AND kind=0 AND k='@_user_1'",(long long)nonce);execute(&store,sql);
    assert(feishu_data_mention_get(&store,nonce,"@_user_1",value,sizeof(value),&value_size)==SQLITE_OK);
    size_t unknown=(size_t)(strstr(expected,"@_user_1000")-expected);
    snprintf(sql,sizeof(sql),"SELECT crc FROM text_aux WHERE owner=%lld AND kind=0 AND k='@_user_1000'",(long long)nonce);
    int64_t literal_crc=scalar(&store,sql);
    snprintf(sql,sizeof(sql),"DELETE FROM text_aux WHERE owner=%lld AND kind=0 AND k='@_user_1000'",(long long)nonce);execute(&store,sql);
    feishu_data_text_point_t missing_point;uint8_t missing_bytes[24];size_t missing_size=0;
    assert(feishu_data_text_seek(&store,id,&view,unknown,&missing_point)==SQLITE_OK);
    assert(!feishu_text_map_read(&io,nr,missing_point.raw,missing_point.logical,unknown,missing_bytes,sizeof(missing_bytes),workspace,sizeof(workspace),&missing_size));
    snprintf(sql,sizeof(sql),"INSERT INTO text_aux(owner,kind,k,v,crc) VALUES(%lld,0,'@_user_1000',NULL,%lld)",(long long)nonce,(long long)literal_crc);execute(&store,sql);
    assert(feishu_text_map_read(&io,nr,missing_point.raw,missing_point.logical,unknown,missing_bytes,sizeof(missing_bytes),workspace,sizeof(workspace),&missing_size));
    assert(!memcmp(missing_bytes,expected+unknown,missing_size));
    snprintf(sql,sizeof(sql),"SELECT min(k) FROM text_aux WHERE owner=%lld AND kind=2",(long long)id);
    int64_t first_point=scalar(&store,sql);
    snprintf(sql,sizeof(sql),"UPDATE text_aux SET crc=crc+1 WHERE owner=%lld AND kind=2 AND k=%lld",(long long)id,(long long)first_point);execute(&store,sql);
    feishu_data_text_point_t point;
    assert(feishu_data_text_seek(&store,id,&view,(uint64_t)first_point,&point)==SQLITE_CORRUPT);
    snprintf(sql,sizeof(sql),"UPDATE text_aux SET crc=crc-1 WHERE owner=%lld AND kind=2 AND k=%lld",(long long)id,(long long)first_point);execute(&store,sql);
    assert(feishu_data_text_seek(&store,id,&view,(uint64_t)first_point,&point)==SQLITE_OK);
    /* Corrupt one view to point at somebody else's active map. */
    feishu_data_writer_t safe_body,safe_map;
    build_body(&store,&safe_body,"safe-stage","@_user_1",8);
    assert(feishu_data_begin(&store,"safe-map",FEISHU_DATA_METADATA,false,&safe_map)==SQLITE_OK);
    assert(feishu_data_mention_put(&store,&safe_map,"@_user_1","safe")==SQLITE_OK);
    int64_t safe_id=safe_body.id,safe_nonce=safe_map.id;
    snprintf(sql,sizeof(sql),"UPDATE text_aux SET v=%lld WHERE owner=%lld AND kind=1",(long long)safe_nonce,(long long)id);execute(&store,sql);
    assert(feishu_data_text_info(&store,id,nr,&view)==SQLITE_CORRUPT);
    /* A bad disposable view never directs GC into another map. Orphan cleanup
     * subsequently removes its unreachable metadata and normal operations work. */
    while(feishu_data_reclaim(&store,&freed)==SQLITE_OK) {FEISHU_TEXT_STORE_YIELD();}
    assert(scalar(&store,"SELECT count(*) FROM text_aux")==1);
    assert(feishu_data_mention_get(&store,safe_nonce,"@_user_1",value,sizeof(value),&value_size)==SQLITE_OK && value_size==4 && !memcmp(value,"safe",4));
    assert(feishu_data_text_publish(&store,&safe_body,&safe_map,5,"safe-live")==SQLITE_OK);
    assert(feishu_data_pin(&store,safe_id,true)==SQLITE_OK);
    /* A failure after body/view publication but before map ownership transfer
     * must roll all three changes back, preserving the previous pinned version. */
    build_body(&store,&body,"replacement-stage","@_user_1",8);
    assert(feishu_data_begin(&store,"replacement-map",FEISHU_DATA_METADATA,false,&map)==SQLITE_OK);
    assert(feishu_data_mention_put(&store,&map,"@_user_1","fresh")==SQLITE_OK);
    int64_t new_id=body.id,new_nonce=map.id;
    assert(sqlite3_set_authorizer(store.db,deny_object_delete,NULL)==SQLITE_OK);
    assert(feishu_data_text_publish(&store,&body,&map,6,"safe-live")==SQLITE_AUTH && body.id==new_id && map.id==new_nonce);
    assert(sqlite3_set_authorizer(store.db,NULL,NULL)==SQLITE_OK);
    assert(feishu_data_find(&store,"safe-live",&object)==SQLITE_OK && object.id==safe_id);
    assert(feishu_data_text_info(&store,new_id,8,&view)==SQLITE_NOTFOUND);
    assert(feishu_data_text_publish(&store,&body,&map,6,"safe-live")==SQLITE_OK);
    assert(feishu_data_pin(&store,new_id,true)==SQLITE_OK);
    /* Metadata pressure is bounded by the shared class cap. Releasing an
     * abandoned map restores normal writes without evicting pinned versions. */
    feishu_data_writer_t existing;
    assert(feishu_data_begin(&store,"existing-list-cache",FEISHU_DATA_METADATA,false,&existing)==SQLITE_OK);
    memset(raw,'R',8192);
    for(unsigned i=0;i<12;++i)assert(feishu_data_append_span(&store,&existing,(const uint8_t *)raw,8192,false)==SQLITE_OK);
    for(unsigned i=0;i<4;++i)assert(feishu_data_append(&store,&existing,(const uint8_t *)raw,1024,false)==SQLITE_OK);
    assert(feishu_data_commit(&store,&existing)==SQLITE_OK); /* 100 KiB ordinary list occupancy. */
    feishu_data_writer_t pressure;
    assert(feishu_data_begin(&store,"pressure-map",FEISHU_DATA_METADATA,false,&pressure)==SQLITE_OK);
    char long_name[129];memset(long_name,'Q',128);long_name[128]=0;unsigned inserted=0;
    FEISHU_TEXT_STORE_STAGE("pressure_start");
    for(;inserted<2000;++inserted) {
        FEISHU_TEXT_STORE_YIELD();
        char key[32];snprintf(key,sizeof(key),"@pressure_%u",inserted);
        int rc=feishu_data_mention_put(&store,&pressure,key,long_name);
        if(rc==SQLITE_FULL)break;
        if(rc)fprintf(stderr,"pressure %u rc=%d\n",inserted,rc);
        assert(rc==SQLITE_OK);
    }
    assert(inserted>=32 && inserted<2000 && pressure.quota_limited);
    FEISHU_TEXT_STORE_STAGE("pressure_full");
    assert(feishu_data_abort(&store,&pressure)==SQLITE_OK);
    while(feishu_data_reclaim(&store,&freed)==SQLITE_OK) {FEISHU_TEXT_STORE_YIELD();}
    assert(feishu_data_mention_get(&store,safe_nonce,"@_user_1",value,sizeof(value),&value_size)==SQLITE_OK);
    assert(feishu_data_mention_get(&store,new_nonce,"@_user_1",value,sizeof(value),&value_size)==SQLITE_OK);
    assert(scalar(&store,"SELECT count(*) FROM text_aux")==4);
    assert(feishu_data_pin(&store,safe_id,false)==SQLITE_OK && feishu_data_pin(&store,new_id,false)==SQLITE_OK);
    while(feishu_data_reclaim(&store,&freed)==SQLITE_OK) {FEISHU_TEXT_STORE_YIELD();}
    assert(scalar(&store,"SELECT count(*) FROM text_aux")==0);
    FEISHU_TEXT_STORE_STAGE("pressure_recovered");
    assert(feishu_data_find(&store,"guard",&object)==SQLITE_OK);
    build_body(&store,&body,"normal-stage","normal",7);
    assert(feishu_data_commit_as(&store,&body,"normal-live")==SQLITE_OK);
    assert(feishu_data_find(&store,"normal-live",&object)==SQLITE_OK);
    assert(feishu_data_close(&store)==SQLITE_OK);
    puts("stored mentions: 32 names, indexed pages, pin/reopen/CRC, atomic publication, quota/full/GC and recovery PASS");
    return 0;
}

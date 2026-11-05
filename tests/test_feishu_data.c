#include "feishu_data.h"
#include "sqlite3.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void save(feishu_data_t *s, const char *key, const char *value, bool required)
{
    feishu_data_writer_t w;
    assert(feishu_data_begin(s,key,required ? FEISHU_DATA_REQUIRED : FEISHU_DATA_TEXT,required,&w)==SQLITE_OK);
    assert(feishu_data_append(s,&w,(const uint8_t *)value,strlen(value)+1,true)==SQLITE_OK);
    assert(feishu_data_commit(s,&w)==SQLITE_OK);
}
static void expect(feishu_data_t *s, const char *key, const char *value)
{
    feishu_data_object_t o;
    uint8_t out[1024]; size_t size;
    assert(feishu_data_find(s,key,&o)==SQLITE_OK);
    assert(feishu_data_read(s,o.id,0,out,sizeof(out),&size)==SQLITE_OK);
    assert(size==strlen(value)+1 && !memcmp(out,value,size));
}
static int deny_raw_update(void *unused,int action,const char *table,const char *column,const char *database,const char *trigger)
{
    (void)unused;(void)database;(void)trigger;
    return action==SQLITE_UPDATE && table && column && !strcmp(table,"objects") && !strcmp(column,"raw") ? SQLITE_DENY:SQLITE_OK;
}
static int deny_second_raw_update(void *context,int action,const char *table,const char *column,const char *database,const char *trigger)
{
    (void)database;(void)trigger;
    if(action==SQLITE_UPDATE && table && column && !strcmp(table,"objects") && !strcmp(column,"raw"))
        return ++*(unsigned *)context==2 ? SQLITE_DENY:SQLITE_OK;
    return SQLITE_OK;
}

int main(int argc, char **argv)
{
    assert(argc==2);
    feishu_data_t s={0}, second={0};
    feishu_data_writer_t w;
    feishu_data_object_t o;
    uint8_t input[1024], out[1024];
    size_t size;
    memset(input,'x',sizeof(input));
    assert(feishu_data_open(&s,argv[1],NULL)==SQLITE_OK);
    assert(feishu_data_open(&second,argv[1],NULL)==SQLITE_BUSY);
    save(&s,"required","keep-owner",true);
    assert(sqlite3_exec(s.db,"BEGIN;UPDATE objects SET committed=3 WHERE k='required'",NULL,NULL,NULL)==SQLITE_OK);
    expect(&s,"required","keep-owner");
    assert(sqlite3_get_autocommit(s.db));
    save(&s,"old","visible-before",false);
    /* Staging is invisible; cancellation preserves the prior committed value. */
    assert(feishu_data_begin(&s,"old",FEISHU_DATA_TEXT,false,&w)==SQLITE_OK);
    assert(feishu_data_append(&s,&w,input,sizeof(input),true)==SQLITE_OK);
    expect(&s,"old","visible-before");
    feishu_data_writer_t stream_writer;
    /* Metadata may arrive after a streamed body; rename only at publication. */
    assert(feishu_data_begin(&s,"temporary",FEISHU_DATA_TEXT,false,&stream_writer)==SQLITE_OK);
    assert(feishu_data_append(&s,&stream_writer,(const uint8_t *)"streamed",9,true)==SQLITE_OK);
    int64_t stream_id=stream_writer.id;
    assert(feishu_data_commit_as(&s,&stream_writer,"final-key")==SQLITE_OK);
    expect(&s,"final-key","streamed");
    assert(feishu_data_stat(&s,stream_id,"wrong-account-key",&o)==SQLITE_NOTFOUND);
    assert(feishu_data_stat(&s,stream_id,"final-key",&o)==SQLITE_OK);
    { int rc=feishu_data_abort(&s,&w); if(rc) fprintf(stderr,"abort rc=%d\n",rc); assert(rc==SQLITE_OK); }
    expect(&s,"old","visible-before");
    assert(feishu_data_find(&s,"old",&o)==SQLITE_OK);
    int64_t pinned=o.id;
    assert(feishu_data_pin(&s,pinned,true)==SQLITE_OK);
    save(&s,"old","visible-after",false);
    /* Old pinned version remains readable after replacement and reclamation. */
    assert(feishu_data_read(&s,pinned,0,out,sizeof(out),&size)==SQLITE_OK);
    assert(!strcmp((char *)out,"visible-before"));
    expect(&s,"old","visible-after");
    size_t reclaimed;
    while(feishu_data_reclaim(&s,&reclaimed)==SQLITE_OK) {}
    expect(&s,"required","keep-owner");
    assert(feishu_data_read(&s,pinned,0,out,sizeof(out),&size)==SQLITE_OK);
    assert(feishu_data_pin(&s,pinned,false)==SQLITE_OK);
    int reclaim_code=feishu_data_reclaim(&s,&reclaimed);
    if(reclaim_code!=SQLITE_OK)fprintf(stderr,"reclaim rc=%d sqlite=%s bytes=%lld\n",reclaim_code,
        sqlite3_errmsg(s.db),(long long)sqlite3_memory_used());
    assert(reclaim_code==SQLITE_OK);
    assert(feishu_data_read(&s,pinned,0,out,sizeof(out),&size)==SQLITE_NOTFOUND);
    /* A failure after chunk INSERT but before size UPDATE cannot duplicate a
     * block when the adapter retries. Both disk and writer cursor roll back. */
    assert(feishu_data_begin(&s,"append-fault",FEISHU_DATA_TEXT,false,&w)==SQLITE_OK);
    assert(sqlite3_set_authorizer(s.db,deny_raw_update,NULL)==SQLITE_OK);
    assert(feishu_data_append(&s,&w,input,sizeof(input),true)==SQLITE_AUTH);
    assert(w.raw==0 && w.stored==0 && w.sequence==0);
    assert(sqlite3_set_authorizer(s.db,NULL,NULL)==SQLITE_OK);
    assert(feishu_data_read(&s,w.id,0,out,sizeof(out),&size)==SQLITE_NOTFOUND);
    assert(feishu_data_append(&s,&w,input,sizeof(input),true)==SQLITE_OK && w.sequence==1 && w.raw==sizeof(input));
    assert(feishu_data_read(&s,w.id,1,out,sizeof(out),&size)==SQLITE_NOTFOUND);
    assert(feishu_data_abort(&s,&w)==SQLITE_OK);
    /* A later failure inside a grouped write must also undo earlier chunks;
     * retry publishes exactly one copy, with the same fixed block codec. */
    uint8_t span[3*1024];memset(span,'g',sizeof(span));unsigned updates=0;
    assert(feishu_data_begin(&s,"span-fault",FEISHU_DATA_IMAGE,false,&w)==SQLITE_OK);
    assert(sqlite3_set_authorizer(s.db,deny_second_raw_update,&updates)==SQLITE_OK);
    assert(feishu_data_append_span(&s,&w,span,sizeof(span),true)==SQLITE_AUTH && updates==2);
    assert(w.raw==0 && w.sequence==0 && w.stored==0);
    assert(sqlite3_set_authorizer(s.db,NULL,NULL)==SQLITE_OK);
    assert(feishu_data_read(&s,w.id,0,out,sizeof(out),&size)==SQLITE_NOTFOUND);
    assert(feishu_data_append_span(&s,&w,span,sizeof(span),true)==SQLITE_OK);
    assert(w.raw==sizeof(span) && w.sequence==3);
    for(unsigned i=0;i<3;++i) {
        assert(feishu_data_read(&s,w.id,i,out,sizeof(out),&size)==SQLITE_OK && size==1024);
        assert(!memcmp(out,span,1024));
    }
    assert(feishu_data_read(&s,w.id,3,out,sizeof(out),&size)==SQLITE_NOTFOUND);
    assert(feishu_data_abort(&s,&w)==SQLITE_OK);
    feishu_data_writer_t leased,orphan;
    assert(feishu_data_begin(&s,"leased",FEISHU_DATA_TEXT,true,&leased)==SQLITE_OK);
    assert(feishu_data_begin(&s,"lost-insert-ack",FEISHU_DATA_TEXT,true,&orphan)==SQLITE_OK);
    int64_t released=0;
    assert(feishu_data_release_orphan(&s,&leased.id,1,&released)==SQLITE_OK && released==orphan.id);
    assert(feishu_data_release_orphan(&s,&leased.id,1,&released)==SQLITE_OK && released==0);
    assert(feishu_data_append(&s,&leased,input,sizeof(input),true)==SQLITE_OK);
    assert(feishu_data_abort(&s,&leased)==SQLITE_OK);
    /* Draft bytes and durable send state are one publication. A failed second
     * publication rolls the body back; retry cannot leave an orphan draft. */
    feishu_data_writer_t body_writer,meta_writer;
    assert(feishu_data_begin(&s,"draft-stage",FEISHU_DATA_REQUIRED,true,&body_writer)==SQLITE_OK);
    assert(feishu_data_append(&s,&body_writer,(const uint8_t *)"draft",6,true)==SQLITE_OK);
    assert(feishu_data_begin(&s,"queue-stage",FEISHU_DATA_REQUIRED,true,&meta_writer)==SQLITE_OK);
    assert(feishu_data_append(&s,&meta_writer,(const uint8_t *)"ready",6,true)==SQLITE_OK);
    feishu_data_writer_t invalid_writer=meta_writer;invalid_writer.id=INT64_MAX;
    assert(feishu_data_commit_pair(&s,&body_writer,"draft-live",&invalid_writer,"queue-live",0)==SQLITE_MISUSE);
    assert(feishu_data_find(&s,"draft-live",&o)==SQLITE_NOTFOUND);
    int64_t draft_id=body_writer.id;
    assert(feishu_data_commit_pair(&s,&body_writer,"draft-live",&meta_writer,"queue-live",0)==SQLITE_OK);
    expect(&s,"draft-live","draft");expect(&s,"queue-live","ready");
    while(feishu_data_reclaim(&s,&reclaimed)==SQLITE_OK) {}
    expect(&s,"draft-live","draft");expect(&s,"queue-live","ready");
    assert(feishu_data_begin(&s,"queue-new",FEISHU_DATA_REQUIRED,true,&meta_writer)==SQLITE_OK);
    assert(feishu_data_append(&s,&meta_writer,(const uint8_t *)"done",5,true)==SQLITE_OK);
    assert(feishu_data_commit_pair(&s,&meta_writer,"queue-live",NULL,NULL,draft_id)==SQLITE_OK);
    while(feishu_data_reclaim(&s,&reclaimed)==SQLITE_OK) {}
    assert(feishu_data_find(&s,"draft-live",&o)==SQLITE_NOTFOUND);
    expect(&s,"queue-live","done");
    /* Logical category cap, low-level FULL, and recoverable allocation failure. */
    assert(feishu_data_begin(&s,"large",FEISHU_DATA_TEXT,false,&w)==SQLITE_OK);
    for(unsigned i=0;i<300;i++) {
        int rc=feishu_data_append(&s,&w,input,sizeof(input),false);
        if(rc!=SQLITE_OK) fprintf(stderr,"append block=%u rc=%d\n",i,rc);
        assert(rc==SQLITE_OK);
    }
    assert(feishu_data_append(&s,&w,input,sizeof(input),false)==SQLITE_FULL);
    assert(w.quota_limited);
    { int rc=feishu_data_abort(&s,&w); if(rc) fprintf(stderr,"large abort rc=%d\n",rc); assert(rc==SQLITE_OK); }
    sqlite3_hard_heap_limit64(sqlite3_memory_used()+1);
    assert(feishu_data_begin(&s,"nomem",FEISHU_DATA_TEXT,false,&w)==SQLITE_NOMEM);
    sqlite3_hard_heap_limit64(32768);
    save(&s,"after-fault","ok",false);
    /* Data damage cannot overrun the decoder or destroy unrelated records. */
    assert(feishu_data_find(&s,"after-fault",&o)==SQLITE_OK);
    char damage_sql[128];
    snprintf(damage_sql,sizeof(damage_sql),"UPDATE chunks SET crc=crc+1 WHERE object=%lld",(long long)o.id);
    assert(sqlite3_exec(s.db,damage_sql,NULL,NULL,NULL)==SQLITE_OK);
    assert(feishu_data_read(&s,o.id,0,out,sizeof(out),&size)==SQLITE_CORRUPT);
    expect(&s,"required","keep-owner");
    save(&s,"after-fault","recovered",false);
    feishu_data_trim(&s);
    assert(feishu_data_close(&s)==SQLITE_OK);
    assert(feishu_data_open(&s,argv[1],NULL)==SQLITE_OK);
    expect(&s,"required","keep-owner"); expect(&s,"after-fault","recovered");
    /* Reboot in the middle of replacement never publishes a partial object. */
    assert(feishu_data_begin(&s,"after-fault",FEISHU_DATA_TEXT,false,&w)==SQLITE_OK);
    assert(feishu_data_append(&s,&w,input,sizeof(input),true)==SQLITE_OK);
    assert(feishu_data_close(&s)==SQLITE_OK);
    assert(feishu_data_open(&s,argv[1],NULL)==SQLITE_OK);
    expect(&s,"after-fault","recovered");
    /* Fill the shared physical page budget across categories. */
    int full=SQLITE_OK;
    for(unsigned object=0;object<100 && full==SQLITE_OK;++object) {
        char key[32]; snprintf(key,sizeof(key),"fill-%u",object);
        full=feishu_data_begin(&s,key,(object%2) ? FEISHU_DATA_IMAGE : FEISHU_DATA_AUDIO,false,&w);
        if(full!=SQLITE_OK) break;
        for(unsigned n=0;n<16 && full==SQLITE_OK;++n)
            full=feishu_data_append(&s,&w,input,sizeof(input),false);
        if(full==SQLITE_OK) full=feishu_data_commit(&s,&w);
        else assert(feishu_data_abort(&s,&w)==SQLITE_OK);
    }
    assert(full==SQLITE_FULL);
    FILE *file=fopen(argv[1],"rb");assert(file);assert(!fseek(file,0,SEEK_END));
    long full_size=ftell(file);assert(!fclose(file));
    while(feishu_data_reclaim(&s,&reclaimed)==SQLITE_OK) {}
    for(unsigned i=0;i<100;++i)assert(feishu_data_shrink(&s)==SQLITE_OK);
    file=fopen(argv[1],"rb");assert(file);assert(!fseek(file,0,SEEK_END));
    long compact_size=ftell(file);assert(!fclose(file));
    assert(compact_size<full_size-100*1024);
    expect(&s,"required","keep-owner");
    save(&s,"after-full","normal-path",false);
    expect(&s,"after-full","normal-path");
    save(&s,"older-other-category","keep-this",false);
    assert(feishu_data_begin(&s,"image-cache",FEISHU_DATA_IMAGE,false,&w)==SQLITE_OK);
    assert(feishu_data_append(&s,&w,input,sizeof(input),false)==SQLITE_OK);
    assert(feishu_data_commit(&s,&w)==SQLITE_OK);
    assert(feishu_data_reclaim_category(&s,FEISHU_DATA_IMAGE,&reclaimed)==SQLITE_OK);
    assert(feishu_data_find(&s,"image-cache",&o)==SQLITE_NOTFOUND);
    expect(&s,"older-other-category","keep-this");
    assert(feishu_data_close(&s)==SQLITE_OK);
    puts("feishu data objects: PASS");
    return 0;
}

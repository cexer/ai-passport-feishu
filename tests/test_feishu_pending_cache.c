#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "sqlite3.h"
#include "feishu_host/cache_fixture.h"
static int deny_pin_update(void *unused,int action,const char *table,const char *column,const char *database,const char *trigger)
{
    (void)unused;(void)database;(void)trigger;
    return action==SQLITE_UPDATE && table && column && !strcmp(table,"objects") && !strcmp(column,"pin") ? SQLITE_DENY:SQLITE_OK;
}

static int deny_reads(void *unused,int action,const char *table,const char *column,const char *database,const char *trigger)
{ (void)unused;(void)table;(void)column;(void)database;(void)trigger;return action==SQLITE_SELECT ? SQLITE_DENY:SQLITE_OK; }

int main(int argc,char **argv)
{
    assert(argc==2 && strlen(argv[1])<sizeof(directory));strcpy(directory,argv[1]);
    sqlite3_vfs native=*sqlite3_vfs_find(NULL);native.zName="feishu";assert(sqlite3_vfs_register(&native,0)==SQLITE_OK);
    assert(feishu_cache_init()==ESP_OK);feishu_cache_set_scope("cli_fake","ou_fake");
    assert(feishu_cache_open()==ESP_OK);
    feishu_outbox_t missing;
    assert(!feishu_cache_pending_empty());
    assert(sqlite3_set_authorizer(store.db,deny_reads,NULL)==SQLITE_OK);
    assert(feishu_cache_pending_read(3,&missing)!=ESP_ERR_NOT_FOUND);
    assert(!feishu_cache_pending_empty());
    assert(sqlite3_set_authorizer(store.db,NULL,NULL)==SQLITE_OK);
    for(unsigned i=0;i<FEISHU_OUTBOX_SLOTS;++i)assert(feishu_cache_pending_read(i,&missing)==ESP_ERR_NOT_FOUND);
    assert(feishu_cache_pending_empty());
    assert(sqlite3_set_authorizer(store.db,deny_reads,NULL)==SQLITE_OK);
    for(unsigned i=0;i<FEISHU_OUTBOX_SLOTS;++i)assert(feishu_cache_pending_read(i,&missing)==ESP_ERR_NOT_FOUND);
    assert(sqlite3_set_authorizer(store.db,NULL,NULL)==SQLITE_OK);
    feishu_cache_set_scope("cli_other","ou_other");assert(!feishu_cache_pending_empty());
    feishu_cache_set_scope("cli_fake","ou_fake");
    for(unsigned i=0;i<FEISHU_OUTBOX_SLOTS;++i)assert(feishu_cache_pending_read(i,&missing)==ESP_ERR_NOT_FOUND);
    feishu_data_writer_t shared={0};int64_t shared_id=0,pin_a=0,pin_b=0;uint64_t shared_size;
    assert(feishu_cache_text_begin(&shared)==ESP_OK);
    assert(feishu_cache_text_append(&shared,"two readers",12)==ESP_OK);
    assert(feishu_cache_text_commit(&shared,"om_two_readers",&shared_id)==ESP_OK);
    assert(feishu_cache_text_pin("om_two_readers",shared_id,false,&pin_a,&shared_size)==ESP_OK);
    assert(feishu_cache_text_pin("om_two_readers",shared_id,false,&pin_b,&shared_size)==ESP_OK);
    assert(feishu_cache_text_pin("om_two_readers",shared_id,false,&pin_b,&shared_size)==ESP_OK); /* Idempotent holder. */
    feishu_cache_unpin(&pin_a);size_t evicted;
    while(feishu_data_reclaim(&store,&evicted)==SQLITE_OK) {}
    uint8_t check[16];size_t check_size;
    assert(feishu_cache_text_read("om_two_readers",shared_id,false,0,check,sizeof(check),&check_size)==ESP_OK && check_size==12);
    assert(sqlite3_set_authorizer(store.db,deny_pin_update,NULL)==SQLITE_OK);
    feishu_cache_unpin(&pin_b); /* The failed disk release remains tracked. */
    assert(sqlite3_set_authorizer(store.db,NULL,NULL)==SQLITE_OK);
    feishu_cache_reap_abandoned();
    while(feishu_data_reclaim(&store,&evicted)==SQLITE_OK) {}
    assert(feishu_cache_text_read("om_two_readers",shared_id,false,0,check,sizeof(check),&check_size)!=ESP_OK);
    assert(feishu_cache_text_begin(&shared)==ESP_OK);
    assert(feishu_cache_text_append(&shared,"pin recovery",13)==ESP_OK);
    assert(feishu_cache_text_commit(&shared,"om_failed_acquire",&shared_id)==ESP_OK);
    assert(sqlite3_set_authorizer(store.db,deny_pin_update,NULL)==SQLITE_OK);
    assert(feishu_cache_text_pin("om_failed_acquire",shared_id,false,&pin_a,&shared_size)!=ESP_OK && !pin_a);
    bool tracked=false;
    for(size_t i=0;i<16;++i)if(readers[i].id==shared_id && !readers[i].count)tracked=true;
    assert(tracked);
    assert(sqlite3_set_authorizer(store.db,NULL,NULL)==SQLITE_OK);
    /* Model an ambiguous I/O completion: the durable pin did change despite
     * the failed acquisition result. No reader owns it and it must be reaped. */
    assert(feishu_data_pin(&store,shared_id,true)==SQLITE_OK);
    feishu_cache_reap_abandoned();
    while(feishu_data_reclaim(&store,&evicted)==SQLITE_OK) {}
    assert(feishu_cache_text_read("om_failed_acquire",shared_id,false,0,check,sizeof(check),&check_size)!=ESP_OK);
    feishu_data_writer_t body={0};assert(feishu_cache_draft_begin(&body)==ESP_OK);
    uint8_t text[1024];memset(text,'Z',sizeof(text));
    for(unsigned i=0;i<300;++i)assert(feishu_cache_text_append(&body,text,sizeof(text))==ESP_OK);
    int64_t body_id=body.id;
    feishu_outbox_t pending,loaded;
    assert(feishu_outbox_create(&pending,body.id,body.raw,"draft_fake","oc_fake","om_fake","fake-uuid"));
    assert(feishu_cache_pending_write(0,&pending,&body)==ESP_OK && !body.id);
    assert(!feishu_cache_pending_empty());
    assert(feishu_cache_pending_read(0,&loaded)==ESP_OK && loaded.object_id==body_id);
    size_t freed;while(feishu_data_reclaim(&store,&freed)==SQLITE_OK) {}
    uint8_t out[260];size_t count=0;
    assert(feishu_cache_text_read(pending.body_name,body_id,false,300*1024-260,out,sizeof(out),&count)==ESP_OK && count==260);
    assert(!memcmp(out,text,count));
    /* Streaming a maximum-sized protected draft keeps only the existing
     * bounded DB pages and one caller block, then releases the page cache. */
    uint8_t stream_block[1024];
    for(unsigned i=0;i<300;++i) {
        size_t streamed=0;
        assert(feishu_cache_text_read_stream(pending.body_name,body_id,false,
            (uint64_t)i*sizeof(stream_block),stream_block,sizeof(stream_block),&streamed)==ESP_OK);
        assert(streamed==sizeof(stream_block) && !memcmp(stream_block,text,streamed));
    }
    feishu_cache_trim();
    assert(feishu_outbox_attempt(&pending,false));
    assert(feishu_cache_pending_write(0,&pending,NULL)==ESP_OK);
    feishu_cache_close();assert(feishu_cache_pending_read(0,&loaded)==ESP_OK);
    assert(loaded.state==FEISHU_OUTBOX_SENDING && !feishu_outbox_attempt(&loaded,false));
    feishu_cache_set_scope("cli_other","ou_other");
    assert(feishu_cache_text_read_stream(pending.body_name,body_id,false,0,
        stream_block,sizeof(stream_block),&count)!=ESP_OK);
    assert(feishu_cache_pending_read(0,&loaded)==ESP_ERR_NOT_FOUND);
    feishu_cache_set_scope("cli_fake","ou_fake");
    assert(feishu_cache_pending_read(0,&loaded)==ESP_OK && loaded.state==FEISHU_OUTBOX_SENDING);
    /* Local cancellation and body release publish together. */
    loaded.state=FEISHU_OUTBOX_EMPTY;
    assert(feishu_cache_pending_write(0,&loaded,NULL)==ESP_OK);
    assert(feishu_cache_pending_read(0,&pending)==ESP_ERR_NOT_FOUND);
    while(feishu_data_reclaim(&store,&freed)==SQLITE_OK) {}
    assert(feishu_cache_text_read(loaded.body_name,body_id,false,0,out,sizeof(out),&count)!=ESP_OK);
    assert(feishu_cache_write("ordinary",text,1,sizeof(text))==ESP_OK);
    assert(feishu_cache_read("ordinary",out,1,sizeof(out),&count,NULL)==ESP_ERR_INVALID_RESPONSE);
    assert(feishu_cache_read("ordinary",text,1,sizeof(text),&count,NULL)==ESP_OK && count==sizeof(text));
    feishu_data_writer_t preview={0},original={0};int64_t display_pin=0,file_pin=0,found_id=0;uint64_t blob_size=0;
    assert(feishu_cache_blob_begin(FEISHU_DATA_IMAGE,&preview)==ESP_OK);
    assert(feishu_cache_blob_begin(FEISHU_DATA_IMAGE,&original)==ESP_OK);
    for(unsigned i=0;i<100;++i)assert(feishu_cache_blob_append(&preview,text,sizeof(text),true)==ESP_OK);
    assert(feishu_cache_blob_append(&preview,text,17,false)==ESP_OK);
    assert(feishu_cache_blob_append(&original,text,32,false)==ESP_OK);
    assert(feishu_cache_blob_commit(&preview,"image/test",&original,"image_src/test",&display_pin)==ESP_OK);
    assert(!preview.id && !original.id && display_pin);
    assert(feishu_cache_blob_find("image/test",&found_id,&blob_size)==ESP_OK && found_id==display_pin && blob_size==100*1024+17);
    assert(feishu_cache_blob_read("image/test",found_id,1019,out,16,&count)==ESP_OK && count==16 && !memcmp(out,text,16));
    assert(feishu_cache_blob_read("image/test",found_id,100*1024,out,sizeof(out),&count)==ESP_OK && count==17);
    assert(feishu_cache_blob_pin("image/test",found_id,&file_pin,&blob_size)==ESP_OK);
    feishu_cache_unpin(&display_pin);
    feishu_cache_close();assert(store.db); /* A decoder's independent lease prevents premature close. */
    while(feishu_data_reclaim(&store,&freed)==SQLITE_OK) {}
    assert(feishu_cache_blob_read("image/test",found_id,0,out,sizeof(out),&count)==ESP_OK);
    feishu_cache_set_scope("cli_other","ou_other");
    assert(feishu_cache_blob_read("image/test",found_id,0,out,sizeof(out),&count)!=ESP_OK);
    feishu_cache_unpin(&file_pin);feishu_cache_set_scope("cli_fake","ou_fake");
    while(feishu_data_reclaim(&store,&freed)==SQLITE_OK) {}
    assert(feishu_cache_blob_find("image/test",&found_id,&blob_size)==ESP_ERR_NOT_FOUND);
    assert(!lock_depth);feishu_cache_close();feishu_cache_unmount();assert(!lock_depth);
    puts("pending/blob cache: atomic draft/media, independent readers, failed release, reboot isolation and cancel recovery PASS");
    return 0;
}

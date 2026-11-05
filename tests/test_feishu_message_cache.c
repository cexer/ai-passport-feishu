#include "feishu_message_store.h"
#include "feishu_host/cache_fixture.h"
#include <stdlib.h>

static bool cancelled;
static bool is_cancelled(void *context) {(void)context;return cancelled;}
static const char *fault;
static int deny_insert(void *context,int action,const char *table,const char *column,const char *database,const char *trigger)
{
    (void)context;(void)column;(void)database;(void)trigger;
    bool denied=action==SQLITE_INSERT && table &&
        ((!strcmp(fault,"metadata") && !strcmp(table,"text_aux")) || (!strcmp(fault,"body") && !strcmp(table,"chunks")));
    return denied ? SQLITE_DENY:SQLITE_OK;
}
static int64_t incoming_count(void)
{
    sqlite3_stmt *query=NULL;
    assert(sqlite3_prepare_v2(store.db,"SELECT count(*) FROM objects WHERE committed=1 AND k LIKE '%/text/om_case'",-1,&query,NULL)==SQLITE_OK);
    assert(sqlite3_step(query)==SQLITE_ROW);int64_t n=sqlite3_column_int64(query,0);sqlite3_finalize(query);return n;
}
static void guard_draft(void)
{
    feishu_outbox_t pending;
    assert(feishu_cache_pending_read(0,&pending)==ESP_OK && pending.state==FEISHU_OUTBOX_READY && pending.length==4);
    uint8_t data[8];size_t size=0;
    assert(feishu_cache_text_read(pending.body_name,pending.object_id,false,0,data,sizeof(data),&size)==ESP_OK && size==4 && !memcmp(data,"keep",4));
}
static char expected[65536];
int main(int argc,char **argv)
{
    assert(argc==7 && strlen(argv[1])<sizeof(directory));strcpy(directory,argv[1]);
    sqlite3_vfs native=*sqlite3_vfs_find(NULL);native.zName="feishu";assert(sqlite3_vfs_register(&native,0)==SQLITE_OK);
    assert(feishu_cache_init()==ESP_OK);feishu_cache_set_scope("cli_case","ou_case");
    feishu_data_writer_t draft;assert(feishu_cache_draft_begin(&draft)==ESP_OK);
    assert(feishu_cache_text_append(&draft,"keep",4)==ESP_OK);
    feishu_outbox_t pending;assert(feishu_outbox_create(&pending,draft.id,4,"draft_keep","oc_keep","","keep-uuid"));
    assert(feishu_cache_pending_write(0,&pending,&draft)==ESP_OK);
    feishu_cache_close(); /* Metadata may arrive first with no DB connection open. */
    const char *mode=argv[4];fault=argv[5];size_t fragment=(size_t)strtoul(argv[6],NULL,10);assert(fragment && fragment<=1024);
    feishu_message_store_t storage;feishu_message_store_init(&storage,is_cancelled,NULL);
    feishu_messages_stream_t parser;feishu_message_t messages[FEISHU_MAX_MESSAGES];
    feishu_messages_stream_init(&parser,messages,FEISHU_MAX_MESSAGES,"ou_self",feishu_message_store_operations(),&storage);
    if(strcmp(fault,"none")) {
        assert(feishu_cache_open()==ESP_OK);
        assert(sqlite3_set_authorizer(store.db,deny_insert,NULL)==SQLITE_OK);
    }
    FILE *file=fopen(argv[2],"rb");assert(file);uint8_t chunk[1024];size_t n;
    while((n=fread(chunk,1,fragment,file))!=0)assert(feishu_messages_stream_feed(&parser,chunk,n));
    assert(!fclose(file));
    if(!strcmp(mode,"cancel"))cancelled=true;
    if(!strcmp(mode,"scope"))feishu_cache_set_scope("cli_other","ou_other");
    size_t count=0;bool finished=feishu_messages_stream_finish(&parser,&count);
    feishu_messages_stream_abort(&parser);
    if(store.db)assert(sqlite3_set_authorizer(store.db,NULL,NULL)==SQLITE_OK);
    feishu_cache_reap_abandoned();
    bool positive=!strcmp(mode,"mapped") || !strcmp(mode,"literal");
    if(positive) {
        assert(finished && count==1 && !messages[0].text_unavailable && messages[0].text_object_id>0);
        assert(messages[0].text_mapped==(!strcmp(mode,"mapped")) && incoming_count()==1);
        file=fopen(argv[3],"rb");assert(file);size_t expected_size=fread(expected,1,sizeof(expected),file);assert(!ferror(file) && feof(file));assert(!fclose(file));
        assert(messages[0].text_length==expected_size && !memcmp(messages[0].text,expected,strlen(messages[0].text)));
        feishu_data_object_t object;char key[256];assert(make_key(key,sizeof(key),scope_app,scope_owner,"text/om_case"));
        assert(feishu_data_stat(&store,messages[0].text_object_id,key,&object)==SQLITE_OK);
        if(messages[0].text_mapped)assert(object.raw!=0); /* Original, not another normalized body. */
        int64_t pin=0;uint64_t total=0;
        assert(feishu_cache_text_pin("om_case",messages[0].text_object_id,messages[0].text_mapped,&pin,&total)==ESP_OK && total==expected_size);
        for(size_t offset=0;offset<expected_size;offset+=257) {
            uint8_t bytes[260];size_t got=0,want=expected_size-offset;if(want>sizeof(bytes))want=sizeof(bytes);
            assert(feishu_cache_text_read("om_case",messages[0].text_object_id,messages[0].text_mapped,offset,bytes,sizeof(bytes),&got)==ESP_OK && got==want && !memcmp(bytes,expected+offset,want));
        }
        feishu_cache_unpin(&pin);
        assert(feishu_cache_write("messages/test",messages,sizeof(*messages),1)==ESP_OK);feishu_cache_close();
        feishu_message_t saved;size_t saved_count=0;
        assert(feishu_cache_read("messages/test",&saved,sizeof(saved),1,&saved_count,NULL)==ESP_OK && saved_count==1 && saved.text_mapped==messages[0].text_mapped);
        assert(feishu_cache_text_pin("om_case",saved.text_object_id,saved.text_mapped,&pin,&total)==ESP_OK && total==expected_size);
        feishu_cache_unpin(&pin);feishu_cache_set_scope("cli_other","ou_other");
        assert(feishu_cache_text_pin("om_case",saved.text_object_id,saved.text_mapped,&pin,&total)!=ESP_OK);
        assert(feishu_cache_pending_read(0,&pending)==ESP_ERR_NOT_FOUND);
        feishu_cache_set_scope("cli_case","ou_case");
    } else {
        if(!strcmp(mode,"envelope"))assert(!finished && !count);
        else assert(finished && count==1 && messages[0].text_unavailable && !messages[0].text_object_id);
        assert(incoming_count()==0);
        feishu_cache_set_scope("cli_case","ou_case");cancelled=false;
        feishu_message_store_init(&storage,is_cancelled,NULL);
        feishu_messages_stream_init(&parser,messages,FEISHU_MAX_MESSAGES,"ou_self",feishu_message_store_operations(),&storage);
        const char normal[]="{\"code\":0,\"data\":{\"items\":[{\"message_id\":\"om_recovery\",\"msg_type\":\"text\",\"sender\":{},\"body\":{\"content\":\"{\\\"text\\\":\\\"normal recovery\\\"}\"}}]}}";
        assert(feishu_messages_stream_feed(&parser,normal,sizeof(normal)-1));
        assert(feishu_messages_stream_finish(&parser,&count) && count==1 && messages[0].text_object_id>0 && !messages[0].text_unavailable);
        feishu_messages_stream_abort(&parser);
    }
    guard_draft();assert(!lock_depth);feishu_cache_close();feishu_cache_unmount();assert(!lock_depth);
    printf("message cache %s/%s fragment=%zu PASS; parser=%zu store=%zu\n",mode,fault,fragment,sizeof(parser),sizeof(storage));
    return 0;
}

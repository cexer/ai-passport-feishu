#include "feishu_host/cache_fixture.h"
#include "feishu_page.h"

static feishu_chat_page_t page,first,readback; /* Test oracles, not product RAM. */
static uint32_t random_state=0x41a7c3u;
static void random_text(char *out,size_t capacity)
{
    for(size_t i=0;i+1<capacity;++i) {
        random_state^=random_state<<13;random_state^=random_state>>17;random_state^=random_state<<5;
        out[i]=(char)(33+random_state%94);
    }
    out[capacity-1]=0;
}
static void make_page(unsigned number,char key[64])
{
    memset(&page,0,sizeof(page));char token[64],next[64];
    snprintf(token,sizeof(token),"cursor+%u/=",number);snprintf(next,sizeof(next),"cursor+%u/=",number+1);
    assert(feishu_page_response(&page.page,number ? token:"",next,true,FEISHU_MAX_CHATS));
    page.page.number=number;
    for(size_t i=0;i<FEISHU_MAX_CHATS;++i) {
        random_text(page.chats[i].chat_id,sizeof(page.chats[i].chat_id));
        random_text(page.chats[i].name,sizeof(page.chats[i].name));
        random_text(page.chats[i].summary,sizeof(page.chats[i].summary));
    }
    feishu_page_key("chats","",page.page.token,key);
}
static unsigned inserted_chunks;
static int reject_chunk_insert(void *unused,int action,const char *table,const char *column,const char *db,const char *trigger)
{
    (void)unused;(void)column;(void)db;(void)trigger;
    return action==SQLITE_INSERT && table && !strcmp(table,"chunks") && ++inserted_chunks==3 ? SQLITE_DENY:SQLITE_OK;
}
static int64_t number(const char *sql)
{
    sqlite3_stmt *q=NULL;assert(sqlite3_prepare_v2(store.db,sql,-1,&q,NULL)==SQLITE_OK);
    assert(sqlite3_step(q)==SQLITE_ROW);int64_t n=sqlite3_column_int64(q,0);sqlite3_finalize(q);return n;
}
int main(int argc,char **argv)
{
    assert(argc==2 && strlen(argv[1])<sizeof(directory));strcpy(directory,argv[1]);
    sqlite3_vfs native=*sqlite3_vfs_find(NULL);native.zName="feishu";assert(sqlite3_vfs_register(&native,0)==SQLITE_OK);
    assert(feishu_cache_init()==ESP_OK);feishu_cache_set_scope("cli_pages","ou_fake");
    feishu_data_writer_t draft={0};assert(feishu_cache_draft_begin(&draft)==ESP_OK);
    assert(feishu_cache_text_append(&draft,"protected draft",16)==ESP_OK);
    feishu_outbox_t pending,loaded;
    assert(feishu_outbox_create(&pending,draft.id,draft.raw,"draft_page_guard","oc_saved","","fake-uuid"));
    assert(feishu_cache_pending_write(0,&pending,&draft)==ESP_OK);
    char key[64],first_key[64],cold_key[64],full_key[256];size_t count=0;int64_t pin=0;
    make_page(0,first_key);first=page;
    assert(feishu_cache_write(first_key,&page,sizeof(page),1)==ESP_OK);
    assert(feishu_cache_read(first_key,&readback,sizeof(readback),1,&count,&pin)==ESP_OK && count==1);
    uint64_t admitted=0;
    for(unsigned i=1;i<=500;++i) {
        make_page(i,key);if(i==1)strcpy(cold_key,key);
        assert(feishu_cache_write(key,&page,sizeof(page),1)==ESP_OK);
        assert(make_key(full_key,sizeof(full_key),scope_app,scope_owner,key));
        feishu_data_object_t o;assert(feishu_data_find(&store,full_key,&o)==SQLITE_OK);admitted+=o.stored;
        assert(number("SELECT coalesce(sum(stored),0) FROM objects WHERE kind=0 AND (committed=1 OR pin>0)")<=128*1024);
    }
    assert(admitted>=10u*128u*1024u);
    assert(feishu_cache_read(cold_key,&readback,sizeof(readback),1,&count,NULL)==ESP_ERR_NOT_FOUND);
    assert(feishu_cache_read(first_key,&readback,sizeof(readback),1,&count,NULL)==ESP_OK && !memcmp(&first,&readback,sizeof(first)));
    assert(feishu_cache_pending_read(0,&loaded)==ESP_OK && loaded.object_id==pending.object_id);
    assert(sqlite3_set_authorizer(store.db,reject_chunk_insert,NULL)==SQLITE_OK);
    assert(feishu_cache_write(key,&first,sizeof(first),1)!=ESP_OK);
    assert(sqlite3_set_authorizer(store.db,NULL,NULL)==SQLITE_OK);
    assert(feishu_cache_read(key,&readback,sizeof(readback),1,&count,NULL)==ESP_OK && !memcmp(&page,&readback,sizeof(page)));
    assert(feishu_cache_write("normal-after-fault",&first,sizeof(first),1)==ESP_OK);
    assert(feishu_cache_read("normal-after-fault",&readback,sizeof(readback),1,&count,NULL)==ESP_OK);
    assert(feishu_cache_pending_read(0,&loaded)==ESP_OK && loaded.object_id==pending.object_id);
    printf("page cache: 500 windows, %llu stored bytes admitted, bounded quota, pinned page/draft, cold eviction and fault recovery PASS\n",(unsigned long long)admitted);
    feishu_cache_unpin(&pin);feishu_cache_close();assert(!lock_depth);
    return 0;
}

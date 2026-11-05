#include "feishu_host/cache_fixture.h"
#include "feishu_root.h"

static uint32_t file_crc(const char *name)
{
    FILE *f=fopen(name,"rb");assert(f);uint32_t crc=UINT32_MAX;uint8_t bytes[512];size_t size;
    while((size=fread(bytes,1,sizeof(bytes),f)))for(size_t i=0;i<size;++i) {
        crc^=bytes[i];for(unsigned b=0;b<8;++b)crc=(crc>>1)^((0u-(crc&1u))&0xedb88320u);
    }
    assert(!ferror(f) && !fclose(f));return crc^UINT32_MAX;
}
static uint32_t damage_header(void)
{
    char path[256];snprintf(path,sizeof(path),"%s/objects.db",directory);
    FILE *f=fopen(path,"r+b");assert(f);assert(fputc('X',f)!=EOF && !fclose(f));return file_crc(path);
}
static void recovered(unsigned generation)
{
    assert(feishu_cache_open()==ESP_ERR_INVALID_STATE);
    assert(feishu_cache_open()==ESP_OK && feishu_cache_recovery_generation()==generation);
}
static int fault_code,read_calls;
static int failed_read(sqlite3_file *file,void *out,int size,sqlite3_int64 offset)
{(void)file;(void)out;(void)size;(void)offset;++read_calls;return fault_code;}
static void engine_fault(int code)
{
    sqlite3_file *file=NULL;assert(sqlite3_file_control(store.db,"main",SQLITE_FCNTL_FILE_POINTER,&file)==SQLITE_OK && file);
    const sqlite3_io_methods *original=file->pMethods;sqlite3_io_methods injected=*original;injected.xRead=failed_read;
    sqlite3_db_release_memory(store.db);fault_code=code;read_calls=0;file->pMethods=&injected;
    char key[256];assert(make_key(key,sizeof(key),scope_app,scope_owner,"text/om_same"));
    feishu_data_object_t object;int rc=feishu_data_find(&store,key,&object);
    file->pMethods=original;
    assert(read_calls>0 && (rc&255)==code);
}
int main(int argc,char **argv)
{
    assert(argc==2 && strlen(argv[1])<sizeof(directory));strcpy(directory,argv[1]);
    sqlite3_vfs native=*sqlite3_vfs_find(NULL);native.zName="feishu";assert(sqlite3_vfs_register(&native,0)==SQLITE_OK);
    assert(feishu_cache_init()==ESP_OK);feishu_cache_set_scope("cli_recovery","ou_fake");
    feishu_root_t root={.directory=directory};feishu_root_part_t config={"required settings",17};
    assert(feishu_root_write(&root,1,&config,1)==FEISHU_ROOT_OK);
    static uint8_t staged[1024];memset(staged,'S',sizeof(staged));
    feishu_data_writer_t old={0},stale={0};int64_t old_id=0,old_pin=0;uint64_t length=0;
    assert(feishu_cache_text_begin(&old)==ESP_OK && feishu_cache_text_append(&old,"old body",8)==ESP_OK);
    assert(feishu_cache_text_commit(&old,"om_same",&old_id)==ESP_OK);
    assert(feishu_cache_text_pin("om_same",old_id,false,&old_pin,&length)==ESP_OK);
    feishu_data_writer_t draft={0};feishu_outbox_t pending,loaded;
    assert(feishu_cache_draft_begin(&draft)==ESP_OK && feishu_cache_text_append(&draft,"old draft",9)==ESP_OK);
    assert(feishu_outbox_create(&pending,draft.id,draft.raw,"old-draft","oc_saved","","old-uuid"));
    assert(feishu_cache_pending_write(0,&pending,&draft)==ESP_OK);
    assert(feishu_cache_text_begin(&stale)==ESP_OK && feishu_cache_text_append(&stale,staged,sizeof(staged))==ESP_OK);
    int64_t floor=stale.id;uint32_t scope=feishu_cache_scope_version();
    /* Model a cold corrupt open while UI values and logical leases survive. */
    assert(feishu_data_close(&store)==SQLITE_OK);uint32_t crc=damage_header();recovered(1);
    assert(feishu_cache_scope_version()!=scope);
    char path[256];snprintf(path,sizeof(path),"%s/objects.q00000001.db",directory);assert(file_crc(path)==crc);
    assert(feishu_cache_pending_read(0,&loaded)==ESP_ERR_NOT_FOUND);
    uint8_t bytes[32];size_t size=0;
    assert(feishu_cache_text_read("om_same",old_id,false,0,bytes,sizeof(bytes),&size)!=ESP_OK);
    feishu_data_writer_t fresh={0};int64_t fresh_id=0,fresh_pin=0;
    assert(feishu_cache_text_begin(&fresh)==ESP_OK && fresh.id>floor);
    assert(feishu_cache_text_append(&fresh,"fresh body",10)==ESP_OK && feishu_cache_text_commit(&fresh,"om_same",&fresh_id)==ESP_OK);
    assert(feishu_cache_text_pin("om_same",fresh_id,false,&fresh_pin,&length)==ESP_OK);
    feishu_cache_unpin(&old_pin);
    feishu_data_object_t object;char full[256];assert(make_key(full,sizeof(full),scope_app,scope_owner,"text/om_same"));
    assert(feishu_data_stat(&store,fresh_id,full,&object)==SQLITE_OK);
    assert(feishu_cache_text_append(&stale,"must not write",14)!=ESP_OK);feishu_cache_text_abort(&stale);
    assert(feishu_cache_text_read("om_same",fresh_id,false,0,bytes,sizeof(bytes),&size)==ESP_OK && size==10 && !memcmp(bytes,"fresh body",10));
    assert(feishu_cache_draft_begin(&draft)==ESP_OK && feishu_cache_text_append(&draft,"new draft",9)==ESP_OK);
    feishu_outbox_t next;assert(feishu_outbox_create(&next,draft.id,draft.raw,"new-draft","oc_saved","","new-uuid"));
    assert(feishu_cache_pending_write(0,&next,&draft)==ESP_OK);
    assert(feishu_cache_pending_write(0,&pending,NULL)!=ESP_OK);
    assert(feishu_cache_pending_read(0,&loaded)==ESP_OK && loaded.object_id==next.object_id);
    /* A damaged record CRC is local corruption, not an engine rebuild. */
    char sql[128];snprintf(sql,sizeof(sql),"UPDATE chunks SET crc=crc+1 WHERE object=%lld",(long long)fresh_id);
    assert(sqlite3_exec(store.db,sql,NULL,NULL,NULL)==SQLITE_OK);
    assert(feishu_cache_text_read("om_same",fresh_id,false,0,bytes,sizeof(bytes),&size)!=ESP_OK && !feishu_data_broken(&store));
    assert(feishu_cache_recovery_generation()==1);
    engine_fault(SQLITE_IOERR);assert(!feishu_data_broken(&store));
    assert(feishu_cache_open()==ESP_OK && feishu_cache_recovery_generation()==1);
    engine_fault(SQLITE_CORRUPT);assert(feishu_data_broken(&store));recovered(2);
    assert(feishu_cache_recovery_generation()==2);
    assert(feishu_cache_write("normal","normal",7,1)==ESP_OK);
    char value[7];size_t count=0;assert(feishu_cache_read("normal",value,sizeof(value),1,&count,NULL)==ESP_OK && !memcmp(value,"normal",7));
    char settings[17];size_t total=0;
    assert(feishu_root_read(&root,1,0,settings,sizeof(settings),&total)==FEISHU_ROOT_OK && !memcmp(settings,"required settings",17));
    assert(file_crc(path)==crc);feishu_cache_unpin(&fresh_pin);feishu_cache_close();assert(!lock_depth);
    puts("recovery cache: corrupt open/live engine, repeated isolation, ID floors, stale handles/drafts, local CRC/IO classification and normal recovery PASS");
    return 0;
}

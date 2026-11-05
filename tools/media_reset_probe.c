/* Silent reset-during-transaction acceptance; synthetic records only. */
#include "feishu_cache.h"
#include "feishu_data.h"
#include "feishu_root.h"
#include "sqlite3.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static int wait_for_reset(void *unused)
{
    (void)unused;
    printf("RESET_PROBE ready=1\n");fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(5000));
    return 0;
}
static bool save(feishu_data_t *store,const char *key,const char *value)
{
    feishu_data_writer_t writer={0};
    return feishu_data_begin(store,key,FEISHU_DATA_REQUIRED,true,&writer)==SQLITE_OK &&
        feishu_data_append(store,&writer,(const uint8_t *)value,strlen(value)+1,false)==SQLITE_OK &&
        feishu_data_commit(store,&writer)==SQLITE_OK;
}
static bool expect(feishu_data_t *store,const char *key,const char *value)
{
    feishu_data_object_t object;uint8_t bytes[64];size_t got=0;
    return feishu_data_find(store,key,&object)==SQLITE_OK &&
        feishu_data_read(store,object.id,0,bytes,sizeof(bytes),&got)==SQLITE_OK &&
        got==strlen(value)+1 && !memcmp(bytes,value,got);
}
static bool rolled_back(feishu_data_t *store)
{
    sqlite3_stmt *query=NULL;
    int rc=sqlite3_prepare_v2(store->db,"SELECT raw,stored,(SELECT count(*) FROM chunks WHERE object=objects.id) "
        "FROM objects WHERE k='interrupted'",-1,&query,NULL);
    if(rc!=SQLITE_OK)return false;
    rc=sqlite3_step(query);
    bool pass=rc==SQLITE_DONE || (rc==SQLITE_ROW && sqlite3_column_int64(query,0)==0 &&
        sqlite3_column_int64(query,1)==0 && sqlite3_column_int64(query,2)==0);
    sqlite3_finalize(query);return pass;
}
void app_main(void)
{
    feishu_data_t store={0};feishu_root_t root={.directory="/feishu"};
    uint8_t phase=0;size_t total=0;
    bool pass=feishu_cache_init()==ESP_OK && feishu_cache_mount()==ESP_OK;
    int marker=pass ? feishu_root_read(&root,0x7e,0,&phase,1,&total):FEISHU_ROOT_IO;
    pass=pass && (marker==FEISHU_ROOT_MISSING || (marker==FEISHU_ROOT_OK && total==1 && phase==1));
    if(pass)pass=feishu_data_open(&store,"/feishu/reset-probe.db","feishu")==SQLITE_OK;
    if(pass && !phase) {
        pass=save(&store,"guard","protected-before-reset");
        phase=1;feishu_root_part_t part={&phase,1};
        if(pass)pass=feishu_root_write(&root,0x7e,&part,1)==FEISHU_ROOT_OK;
        feishu_data_writer_t writer={0};
        if(pass)pass=feishu_data_begin(&store,"interrupted",FEISHU_DATA_IMAGE,false,&writer)==SQLITE_OK;
        if(pass) {
            static uint8_t bytes[8192];
            for(size_t i=0;i<sizeof(bytes);++i)bytes[i]=(uint8_t)(i*37u+i/251u);
            sqlite3_commit_hook(store.db,wait_for_reset,NULL);
            (void)feishu_data_append_span(&store,&writer,bytes,sizeof(bytes),false);
            /* Reaching here means the host did not reset in the announced window. */
            pass=false;
        }
    } else if(pass) {
        feishu_data_object_t object;
        pass=expect(&store,"guard","protected-before-reset") &&
            feishu_data_find(&store,"interrupted",&object)==SQLITE_NOTFOUND &&
            rolled_back(&store) &&
            save(&store,"normal","write-read-after-reset") &&
            expect(&store,"normal","write-read-after-reset");
    }
    if(store.db)feishu_data_close(&store);
    feishu_cache_unmount();
    printf("RESET_PROBE DONE pass=%u\n",(unsigned)pass);
}

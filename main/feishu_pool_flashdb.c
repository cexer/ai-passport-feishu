#include "feishu_pool.h"
#include "feishu_flash_port.h"
#include "feishu_data_flashdb.h"
#include "feishu_root_flashdb.h"
#include <stdio.h>
esp_err_t feishu_pool_mount(const char *directory)
{
    if(!directory || !*directory)return ESP_ERR_INVALID_ARG;
    if(feishu_fdb_status()==FEISHU_FDB_OK)return ESP_OK;
    feishu_fdb_close();feishu_fdb_io_t io;
    esp_err_t err=feishu_flash_port_open(&io);if(err!=ESP_OK)return err;
    int rc=feishu_data_flashdb_configure(&io);if(rc!=FEISHU_DATA_OK)return ESP_FAIL;
    /* Full blank check precedes formatting. Nonblank legacy data requires
     * the explicitly authorized external erase; no automatic migration. */
    rc=feishu_fdb_open(&io,false);
    if(rc!=FEISHU_FDB_OK)rc=feishu_fdb_open(&io,true);
    if(rc!=FEISHU_FDB_OK)return ESP_FAIL;
    size_t size=0;char byte;
    rc=feishu_fdb_get("@catalog",&byte,1,&size);
    if(rc==FEISHU_FDB_NOTFOUND)rc=feishu_data_flashdb_initialize()==FEISHU_DATA_OK?FEISHU_FDB_OK:FEISHU_FDB_ERROR;
    if(rc!=FEISHU_FDB_OK && rc!=FEISHU_FDB_TOOBIG){feishu_fdb_close();return ESP_FAIL;}
    printf("FEISHU_STORAGE backend=flashdb pool=%u\n",(unsigned)io.size);
    return ESP_OK;
}
void feishu_pool_unmount(void) {feishu_fdb_close();}
bool feishu_pool_room(size_t margin) {return feishu_fdb_room(margin);}
esp_err_t feishu_pool_open_data(feishu_data_t *store,const char *directory,
                               uint64_t *floor,bool broken,feishu_pool_info_t *info)
{
    (void)broken;
    *info=(feishu_pool_info_t){.id_floor=*floor};
    int rc=feishu_data_open(store,directory,NULL);
    if(rc==FEISHU_DATA_OK)rc=feishu_data_id_floor(store,floor);
    if(rc==FEISHU_DATA_OK)return ESP_OK;
    feishu_data_close(store);
    return rc==FEISHU_DATA_NOMEM?ESP_ERR_NO_MEM:ESP_FAIL;
}
int feishu_pool_reclaim(feishu_data_t *store,int kind,size_t *bytes)
{
    int rc=feishu_root_flashdb_collect(bytes);
    if(rc==FEISHU_ROOT_OK)return FEISHU_DATA_OK;
    if(rc!=FEISHU_ROOT_MISSING)return FEISHU_DATA_IOERR;
    return kind<0?feishu_data_reclaim(store,bytes):feishu_data_reclaim_category(store,(feishu_data_kind_t)kind,bytes);
}

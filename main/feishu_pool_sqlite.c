#include "feishu_pool.h"
#include "feishu_recovery.h"
#include "esp_partition.h"
#include "esp_spiffs.h"
#include <stdio.h>
static bool mounted;
int feishu_pool_reclaim(feishu_data_t *store,int kind,size_t *bytes)
{return kind<0?feishu_data_reclaim(store,bytes):feishu_data_reclaim_category(store,(feishu_data_kind_t)kind,bytes);}
static bool blank_pool(void)
{
    const esp_partition_t *p=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_DATA_SPIFFS,"feishu_img");
    if(!p)return false;
    uint8_t buffer[256];
    for(size_t pos=0;pos<p->size;pos+=sizeof(buffer)) {
        if(esp_partition_read(p,pos,buffer,sizeof(buffer))!=ESP_OK)return false;
        for(size_t i=0;i<sizeof(buffer);++i)if(buffer[i]!=0xff)return false;
    }
    return true;
}
esp_err_t feishu_pool_mount(const char *directory)
{
    if(mounted)return ESP_OK;
    esp_vfs_spiffs_conf_t cfg={.base_path=directory,.partition_label="feishu_img",
                              .max_files=5,.format_if_mount_failed=false};
    esp_err_t rc=esp_vfs_spiffs_register(&cfg);
    if(rc!=ESP_OK && blank_pool()) {
        rc=esp_spiffs_format("feishu_img");
        if(rc==ESP_OK)rc=esp_vfs_spiffs_register(&cfg);
    }
    if(rc==ESP_OK) {
        mounted=true;
        char legacy[256];int n=snprintf(legacy,sizeof(legacy),"%s/current.jpg",directory);
        if(n>0 && (size_t)n<sizeof(legacy))(void)remove(legacy);
    }
    return rc;
}
void feishu_pool_unmount(void)
{if(mounted){esp_vfs_spiffs_unregister("feishu_img");mounted=false;}}
bool feishu_pool_room(size_t margin)
{
    size_t total=0,used=0;
    return esp_spiffs_info("feishu_img",&total,&used)==ESP_OK && total>=used && total-used>=margin;
}
static void transfer(feishu_pool_info_t *out,const feishu_recovery_info_t *in)
{*out=(feishu_pool_info_t){in->generation,in->id_floor,in->held};}
esp_err_t feishu_pool_open_data(feishu_data_t *store,const char *directory,
                               uint64_t *floor,bool broken,feishu_pool_info_t *info)
{
    const feishu_recovery_t recovery={.root={.directory=directory}};
    feishu_recovery_info_t native={0};
    int recovered=feishu_recovery_resume(&recovery,&native);transfer(info,&native);
    if(recovered!=FEISHU_ROOT_OK)return ESP_FAIL;
    if(native.id_floor>*floor)*floor=native.id_floor;
    if(broken) {
        if(!feishu_pool_room(64u*1024u))return ESP_ERR_INVALID_SIZE;
        recovered=feishu_recovery_isolate(&recovery,*floor,&native);transfer(info,&native);
        if(recovered!=FEISHU_ROOT_OK)return ESP_FAIL;
    }
    char path[256];int n=snprintf(path,sizeof(path),"%s/objects.db",directory);
    if(n<0 || (size_t)n>=sizeof(path))return ESP_ERR_INVALID_SIZE;
    bool isolated=broken;
    for(unsigned attempt=0;attempt<2;++attempt) {
        int rc=feishu_data_open(store,path,"feishu");
        if(rc==FEISHU_DATA_OK) {
            rc=feishu_data_id_floor(store,floor);
            if(rc!=FEISHU_DATA_OK){fprintf(stderr,"FEISHU_DB_FLOOR rc=%d\n",rc);feishu_data_close(store);}
        }
        if(rc==FEISHU_DATA_OK)return isolated?ESP_ERR_INVALID_STATE:ESP_OK;
        if(rc==FEISHU_DATA_NOMEM)return ESP_ERR_NO_MEM;
        if(((rc&255)!=FEISHU_DATA_CORRUPT && (rc&255)!=FEISHU_DATA_BAD_FORMAT) || isolated)return ESP_FAIL;
        if(!feishu_pool_room(64u*1024u))return ESP_ERR_INVALID_SIZE;
        recovered=feishu_recovery_isolate(&recovery,*floor,&native);transfer(info,&native);
        if(recovered!=FEISHU_ROOT_OK)return ESP_FAIL;
        isolated=true;
    }
    return ESP_FAIL;
}

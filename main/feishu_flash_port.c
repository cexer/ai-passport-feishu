#include "feishu_flash_port.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#ifdef FEISHU_SEND_PERF_AUTOTEST
#include "esp_timer.h"
static feishu_flash_profile_t profile;
feishu_flash_profile_t feishu_flash_profile(void) {return profile;}
#define PROFILE_START() int64_t profile_start=esp_timer_get_time()
#define PROFILE_END(kind) do {++profile.kind##s;profile.kind##_us+=(uint32_t)(esp_timer_get_time()-profile_start);}while(0)
#else
#define PROFILE_START()
#define PROFILE_END(kind)
#endif
#define POOL_SIZE (1024u*1024u)
static unsigned operations;
static void cooperate(void) {if((++operations&127u)==0)vTaskDelay(1);}
#ifdef FEISHU_STORAGE_AUTOTEST
static int read_left=-1,write_left=-1;
static bool partial_write,tripped;
static unsigned injected,after_fault;
void feishu_flash_test_faults(int r,int w,bool p)
{read_left=r;write_left=w;partial_write=p;tripped=false;injected=after_fault=0;}
unsigned feishu_flash_test_injected(void) {return injected;}
unsigned feishu_flash_test_after_fault(void) {return after_fault;}
static bool trip(int *left)
{if(*left==0){*left=-1;tripped=true;++injected;return true;}if(*left>0)--*left;return false;}
#endif
static int read_bytes(void *ctx,uint32_t at,void *out,size_t size)
{
    cooperate();
#ifdef FEISHU_STORAGE_AUTOTEST
    if(trip(&read_left))return -1;
#endif
    PROFILE_START();esp_err_t result=esp_partition_read(ctx,at,out,size);PROFILE_END(read);
    return result==ESP_OK?(int)size:-1;
}
static int write_bytes(void *ctx,uint32_t at,const void *in,size_t size)
{
    cooperate();
#ifdef FEISHU_STORAGE_AUTOTEST
    if(tripped)++after_fault;
    if(trip(&write_left)) {
        if(partial_write && size && esp_partition_write(ctx,at,in,(size+1)/2)!=ESP_OK)return -1;
        return -1;
    }
#endif
    PROFILE_START();esp_err_t result=esp_partition_write(ctx,at,in,size);PROFILE_END(write);
    return result==ESP_OK?(int)size:-1;
}
static int erase_bytes(void *ctx,uint32_t at,size_t size)
{
    cooperate();
#ifdef FEISHU_STORAGE_AUTOTEST
    if(tripped)++after_fault;
    if(trip(&write_left))return -1;
#endif
    PROFILE_START();esp_err_t result=esp_partition_erase_range(ctx,at,size);PROFILE_END(erase);
    return result==ESP_OK?(int)size:-1;
}
esp_err_t feishu_flash_port_open(feishu_fdb_io_t *io)
{
    if(!io)return ESP_ERR_INVALID_ARG;
    const esp_partition_t *p=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_ANY,"feishu_img");
    if(!p || p->address!=0x370000 || p->size!=POOL_SIZE)return ESP_ERR_INVALID_SIZE;
    *io=(feishu_fdb_io_t){(void *)p,POOL_SIZE,read_bytes,write_bytes,erase_bytes};return ESP_OK;
}

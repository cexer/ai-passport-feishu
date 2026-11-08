#include "feishu_flash_port.h"
#include "esp_partition.h"
#include "esp_log.h"
#include <string.h>
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
static TickType_t cooperate_at;
#ifdef FEISHU_ASSISTANT_AUTOTEST
#include "esp_freertos_hooks.h"
#include <stdatomic.h>
static atomic_uint probe_idles;
static uint32_t probe_calls,probe_yields;
static TickType_t probe_report_at;
static bool probe_hook_registered;
static bool scheduler_idle_probe(void) {atomic_fetch_add(&probe_idles,1);return true;}
feishu_flash_scheduler_probe_t feishu_flash_scheduler_probe(void)
{return (feishu_flash_scheduler_probe_t){probe_calls,probe_yields,atomic_load(&probe_idles)};}
#endif
static const esp_partition_t *mapped_partition;
static const void *mapped_pool;
static esp_partition_mmap_handle_t mapped_handle;
void feishu_flash_port_close(void)
{
    if(mapped_pool)esp_partition_munmap(mapped_handle);
    mapped_pool=NULL;mapped_partition=NULL;mapped_handle=0;
}
static void cooperate(void)
{
    TickType_t now=xTaskGetTickCount();
#ifdef FEISHU_ASSISTANT_AUTOTEST
    ++probe_calls;
#endif
    if((TickType_t)(now-cooperate_at)>=pdMS_TO_TICKS(4)){
        /* One tick can be consumed entirely by another ready UI/audio task. */
        vTaskDelay(2);cooperate_at=xTaskGetTickCount();
#ifdef FEISHU_ASSISTANT_AUTOTEST
        ++probe_yields;
#endif
    }
#ifdef FEISHU_ASSISTANT_AUTOTEST
    if((TickType_t)(now-probe_report_at)>=pdMS_TO_TICKS(1000)){
        probe_report_at=now;
        ESP_LOGI("feishu_flash","FLASH_SCHED calls=%u yields=%u idles=%u",(unsigned)probe_calls,(unsigned)probe_yields,(unsigned)atomic_load(&probe_idles));
    }
#endif
}
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
    const esp_partition_t *partition=ctx;
    if(at>partition->size || size>partition->size-at)return -1;
    PROFILE_START();esp_err_t result=ESP_OK;
    if(mapped_pool && partition==mapped_partition) {
        memcpy(out,(const uint8_t *)mapped_pool+at,size);
#ifdef FEISHU_SEND_PERF_AUTOTEST
        ++profile.mapped_reads;
#endif
    } else {
        result=esp_partition_read(ctx,at,out,size);
#ifdef FEISHU_SEND_PERF_AUTOTEST
        ++profile.driver_reads;
#endif
    }
    PROFILE_END(read);
    return result==ESP_OK?(int)size:-1;
}
#ifdef FEISHU_SEND_PERF_AUTOTEST
static void prime_mapping(uint32_t at,size_t size)
{
    if(mapped_pool && size) {
        volatile const uint8_t *p=mapped_pool;
        volatile uint8_t old=p[at];old=p[at+size-1];(void)old;
    }
}
static esp_err_t verify_mapping(const esp_partition_t *p,uint32_t at,size_t size)
{
    if(!mapped_pool || p!=mapped_partition)return ESP_OK;
    uint8_t reference[64];
    for(size_t offset=0;offset<size;) {
        size_t n=size-offset;if(n>sizeof(reference))n=sizeof(reference);
        ++profile.verify_reads;
        esp_err_t err=esp_partition_read(p,at+offset,reference,n);if(err!=ESP_OK)return err;
        if(memcmp(reference,(const uint8_t *)mapped_pool+at+offset,n))return ESP_ERR_INVALID_STATE;
        offset+=n;
    }
    return ESP_OK;
}
#endif
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
#ifdef FEISHU_SEND_PERF_AUTOTEST
    prime_mapping(at,size);
#endif
    PROFILE_START();esp_err_t result=esp_partition_write(ctx,at,in,size);
#ifdef FEISHU_SEND_PERF_AUTOTEST
    if(result==ESP_OK)result=verify_mapping(ctx,at,size);
#endif
    PROFILE_END(write);
    return result==ESP_OK?(int)size:-1;
}
static int erase_bytes(void *ctx,uint32_t at,size_t size)
{
    cooperate();
#ifdef FEISHU_STORAGE_AUTOTEST
    if(tripped)++after_fault;
    if(trip(&write_left))return -1;
#endif
#ifdef FEISHU_SEND_PERF_AUTOTEST
    prime_mapping(at,size);
#endif
    PROFILE_START();esp_err_t result=esp_partition_erase_range(ctx,at,size);
#ifdef FEISHU_SEND_PERF_AUTOTEST
    if(result==ESP_OK)result=verify_mapping(ctx,at,size);
#endif
    PROFILE_END(erase);
    return result==ESP_OK?(int)size:-1;
}
esp_err_t feishu_flash_port_open(feishu_fdb_io_t *io)
{
    if(!io)return ESP_ERR_INVALID_ARG;
    feishu_fdb_set_cooperate(cooperate);
#ifdef FEISHU_ASSISTANT_AUTOTEST
    if(!probe_hook_registered)probe_hook_registered=esp_register_freertos_idle_hook(scheduler_idle_probe)==ESP_OK;
#endif
    const esp_partition_t *p=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_ANY,"feishu_img");
    if(!p || p->address!=0x370000 || p->size!=POOL_SIZE)return ESP_ERR_INVALID_SIZE;
    if(!mapped_pool) {
        const void *address=NULL;esp_partition_mmap_handle_t handle=0;
        /* ESP-IDF flushes mapped Flash cache lines after writes and erases.
         * No copy of the 1 MiB pool is allocated in application RAM. */
        esp_err_t mapped=esp_partition_mmap(p,0,p->size,ESP_PARTITION_MMAP_DATA,&address,&handle);
        if(mapped==ESP_OK){mapped_pool=address;mapped_handle=handle;mapped_partition=p;}
        ESP_LOGI("feishu_flash","FLASH_READ_PATH mapped=%u pool_bytes=%u result=%d",mapped_pool?1u:0u,(unsigned)p->size,mapped);
    }
    *io=(feishu_fdb_io_t){(void *)p,POOL_SIZE,read_bytes,write_bytes,erase_bytes};return ESP_OK;
}

/* Synthetic object-backend acceptance. No credentials, radio or audio. */
#include "feishu_data.h"
#include "feishu_data_flashdb.h"
#include "esp_partition.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
#define BYTES (640u*1024u)
static const esp_partition_t *part;
static int read_left=-1,write_left=-1;
static bool partial,tripped;
static bool fail(int *counter)
{if(*counter==0){*counter=-1;tripped=true;return true;}if(*counter>0)--*counter;return false;}
static int rd(void *ctx,uint32_t at,void *out,size_t size)
{
    (void)ctx;if(at>BYTES || size>BYTES-at || fail(&read_left))return -1;
    return esp_partition_read(part,at,out,size)==ESP_OK?(int)size:-1;
}
static int wr(void *ctx,uint32_t at,const void *in,size_t size)
{
    (void)ctx;if(at>BYTES || size>BYTES-at)return -1;
    if(fail(&write_left)) {
        if(partial && size && esp_partition_write(part,at,in,(size+1)/2)!=ESP_OK)return -1;
        return -1;
    }
    return esp_partition_write(part,at,in,size)==ESP_OK?(int)size:-1;
}
static int er(void *ctx,uint32_t at,size_t size)
{
    (void)ctx;if(at>BYTES || size>BYTES-at || fail(&write_left))return -1;
    return esp_partition_erase_range(part,at,size)==ESP_OK?(int)size:-1;
}
static feishu_fdb_io_t io={NULL,BYTES,rd,wr,er};
static void faults(int r,int w,bool p) {read_left=r;write_left=w;partial=p;tripped=false;}
static void prepare(void)
{
    faults(-1,-1,false);
    assert(esp_partition_erase_range(part,0,BYTES)==ESP_OK);
    assert(feishu_fdb_open(&io,true)==FEISHU_FDB_OK);feishu_fdb_close();
    assert(feishu_data_flashdb_configure(&io)==FEISHU_DATA_OK);
    assert(feishu_data_flashdb_initialize()==FEISHU_DATA_OK);
}
static void stage(feishu_data_t *s,const char *key,const char *value,feishu_data_writer_t *w)
{
    assert(feishu_data_begin(s,key,FEISHU_DATA_REQUIRED,true,w)==FEISHU_DATA_OK);
    assert(feishu_data_append(s,w,(const uint8_t *)value,strlen(value)+1,true)==FEISHU_DATA_OK);
}
static void value(feishu_data_t *s,const char *key,char *out)
{
    feishu_data_object_t obj;size_t n=0;
    assert(feishu_data_find(s,key,&obj)==FEISHU_DATA_OK && obj.protected_data);
    assert(feishu_data_read(s,obj.id,0,(uint8_t *)out,32,&n)==FEISHU_DATA_OK && n && n<=32 && !out[n-1]);
}
extern int storage_contract_main(int,char **);
static void task(void *arg)
{
    (void)arg;vTaskDelay(pdMS_TO_TICKS(5000));uint32_t chip=0;int pass=0;
    if(esp_flash_get_size(NULL,&chip)!=ESP_OK || chip!=0x800000)goto done;
    esp_partition_iterator_t it=esp_partition_find(ESP_PARTITION_TYPE_ANY,ESP_PARTITION_SUBTYPE_ANY,NULL);
    while(it) {
        const esp_partition_t *p=esp_partition_get(it);
        if(p->address<0x800000 && p->address+p->size>0x700000){esp_partition_iterator_release(it);goto done;}
        it=esp_partition_next(it);
    }
    if(esp_partition_register_external(NULL,0x700000,BYTES,"fdb_object",ESP_PARTITION_TYPE_DATA,
       ESP_PARTITION_SUBTYPE_DATA_SPIFFS,&part)!=ESP_OK)goto done;
    puts("FDB_OBJECT_REGION offset=7340032 bytes=655360 live_pool_written=0");
    unsigned heap_before=esp_get_free_heap_size();int64_t start=esp_timer_get_time();
    prepare();char *args[]={"test","raw-pool","flashdb"};
    assert(storage_contract_main(3,args)==0);
    puts("FDB_OBJECT_CONTRACT pass=1");
    prepare();feishu_data_t s={0};feishu_data_writer_t a,b;
    assert(feishu_data_open(&s,"pool",NULL)==FEISHU_DATA_OK);
    stage(&s,"owner","synthetic owner",&a);assert(feishu_data_commit(&s,&a)==FEISHU_DATA_OK);
    const int cuts[]={0,1,3,7,12,24};unsigned injected=0,controls=0;
    for(unsigned mode=0;mode<2;++mode)for(unsigned cut=0;cut<6;++cut) {
        stage(&s,"body","old body",&a);stage(&s,"state","old state",&b);
        assert(feishu_data_commit_pair(&s,&a,NULL,&b,NULL,0)==FEISHU_DATA_OK);
        stage(&s,"body-stage","new body",&a);stage(&s,"state-stage","new state",&b);
        faults(mode?-1:cuts[cut],mode?cuts[cut]:-1,cut%2!=0);
        int rc=feishu_data_commit_pair(&s,&a,"body",&b,"state",0);
        if(tripped){assert(rc==FEISHU_DATA_IOERR);++injected;}else{assert(rc==FEISHU_DATA_OK);++controls;}
        assert(feishu_data_close(&s)==FEISHU_DATA_OK);faults(-1,-1,false);
        assert(feishu_data_open(&s,"pool",NULL)==FEISHU_DATA_OK);
        char body[32],state[32],owner[32];value(&s,"body",body);value(&s,"state",state);value(&s,"owner",owner);
        assert(!strcmp(owner,"synthetic owner") &&
            ((!strcmp(body,"old body") && !strcmp(state,"old state")) ||
             (!strcmp(body,"new body") && !strcmp(state,"new state"))));
        /* The main write path must recover after the injected exception. */
        stage(&s,"recovery","normal write",&a);assert(feishu_data_commit(&s,&a)==FEISHU_DATA_OK);
        for(unsigned i=0;i<256;++i){size_t n;int collect=feishu_data_reclaim(&s,&n);if(collect==FEISHU_DATA_FULL)break;assert(collect==FEISHU_DATA_OK);vTaskDelay(1);}
        printf("FDB_OBJECT_FAULT mode=%u cut=%d recovered=1\n",mode,cuts[cut]);
    }
    assert(feishu_data_close(&s)==FEISHU_DATA_OK);
    printf("FDB_OBJECT_TIMING elapsed_us=%lld heap_before=%u heap_after=%u stack_remaining=%u injected=%u controls=%u\n",
        (long long)(esp_timer_get_time()-start),heap_before,(unsigned)esp_get_free_heap_size(),
        (unsigned)uxTaskGetStackHighWaterMark(NULL),injected,controls);
    pass=1;
done:printf("FDB_OBJECT_RESULT pass=%d live_pool_written=0\n",pass);vTaskDelete(NULL);
}
void app_main(void) {setvbuf(stdout,NULL,_IONBF,0);xTaskCreate(task,"fdb_object",16384,NULL,5,NULL);}

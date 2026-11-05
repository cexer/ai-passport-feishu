#include "feishu_cache.h"
#include "feishu_data.h"
#include "feishu_text_map.h"
#include "feishu_pool.h"
#include <stdatomic.h>
#define IO_BEGIN() feishu_data_io_begin()
#define IO_END() feishu_data_io_end()
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#ifndef FEISHU_CACHE_DIRECTORY
#define FEISHU_CACHE_DIRECTORY "/feishu"
#endif

static feishu_data_t store;
static uint64_t id_floor;
static atomic_uint recovery_generation;
uint32_t feishu_cache_recovery_generation(void) {return atomic_load(&recovery_generation);}
static SemaphoreHandle_t mutex;
static char scope_app[64],scope_owner[64];
static uint32_t staging_sequence;
static uint32_t scope_version;
/* Four proven-empty flags, not draft bodies. Unknown/error states must read
 * durable metadata. Foreground empty checks never wait behind cache writes. */
static atomic_uint pending_absent;
bool feishu_cache_pending_empty(void)
{ return atomic_load(&pending_absent)==((1u<<FEISHU_OUTBOX_SLOTS)-1); }
/* Fixed leases survive a failed abort until the disk/allocator recovers. */
static int64_t writer_ids[16];
static bool writer_abandoned[16];
static bool writer_registered(int64_t id)
{
    for(size_t i=0;i<16;++i)if(writer_ids[i]==id && !writer_abandoned[i])return id>0;
    return false;
}
/* Fixed reader leases avoid changing persistent pin state for every redraw.
 * Failed releases remain tracked until storage/allocator recovery. */
static struct {int64_t id;unsigned count;} readers[16];
static bool cache_room(void) {return feishu_pool_room(48u*1024u);}

static void writer_forget(int64_t id)
{ for(size_t i=0;i<16;++i)if(writer_ids[i]==id) {writer_ids[i]=0;writer_abandoned[i]=false;} }
static void readers_reap(void)
{
    if(!feishu_data_is_open(&store))return;
    for(size_t i=0;i<16;++i)if(readers[i].id && !readers[i].count) {
        int rc=feishu_data_pin(&store,readers[i].id,false);
        if(rc==FEISHU_DATA_OK || rc==FEISHU_DATA_NOTFOUND)readers[i].id=0;
    }
}
static int reader_acquire(int64_t id)
{
    readers_reap();size_t empty=16;
    for(size_t i=0;i<16;++i) {
        if(readers[i].id==id) {
            if(readers[i].count==UINT32_MAX)return FEISHU_DATA_FULL;
            if(!readers[i].count) {
                int rc=feishu_data_pin(&store,id,true);if(rc!=FEISHU_DATA_OK)return rc;
            }
            ++readers[i].count;return FEISHU_DATA_OK;
        }
        if(!readers[i].id)empty=i;
    }
    if(empty==16)return FEISHU_DATA_FULL;
    int rc=feishu_data_pin(&store,id,true);
    if(rc==FEISHU_DATA_OK) {readers[empty].id=id;readers[empty].count=1;}
    /* An I/O error can leave the UPDATE durable even though its caller saw
     * failure. Retain a zero-reader lease so recovery clears that pin. */
    else if(rc!=FEISHU_DATA_NOTFOUND) {readers[empty].id=id;readers[empty].count=0;}
    return rc;
}
static void reader_release(int64_t id)
{
    for(size_t i=0;i<16;++i)if(readers[i].id==id) {
        if(readers[i].count)--readers[i].count;
        if(!readers[i].count) {
            int rc=feishu_data_pin(&store,id,false);
            if(rc==FEISHU_DATA_OK || rc==FEISHU_DATA_NOTFOUND)readers[i].id=0;
        }
        return;
    }
}
static void writers_reap(void)
{
    readers_reap();
    for(size_t i=0;i<16;++i)if(writer_ids[i] && writer_abandoned[i]) {
        feishu_data_writer_t w={.id=writer_ids[i]};
        if(feishu_data_abort(&store,&w)==FEISHU_DATA_OK) {writer_ids[i]=0;writer_abandoned[i]=false;}
    }
    /* An IO error at a first INSERT's commit can leave an empty staging row
     * despite begin reporting failure. Only rows outside all live leases may
     * lose their pin; this never discards another worker's active writer. */
    for(unsigned i=0;feishu_data_is_open(&store) && i<32;++i) {
        int64_t released=0;
        if(feishu_data_release_orphan(&store,writer_ids,16,&released)!=FEISHU_DATA_OK || !released)break;
    }
}
static int writer_begin(const char *key,feishu_data_kind_t kind,bool required,feishu_data_writer_t *w)
{
    writers_reap();
    for(unsigned tries=0;tries<256;++tries) {
        size_t slot=0;while(slot<16 && writer_ids[slot])++slot;
        if(slot==16)return FEISHU_DATA_FULL;
        int rc=cache_room() ? feishu_data_begin(&store,key,kind,required,w):FEISHU_DATA_FULL;
        if(rc==FEISHU_DATA_OK) {if((uint64_t)w->id>id_floor)id_floor=(uint64_t)w->id;writer_ids[slot]=w->id;writer_abandoned[slot]=false;return rc;}
        if(rc!=FEISHU_DATA_FULL)return rc;
        size_t reclaimed;
        if(feishu_pool_reclaim(&store,-1,&reclaimed)!=FEISHU_DATA_OK)return rc;
        if(feishu_data_shrink(&store)!=FEISHU_DATA_OK)return rc;
        xSemaphoreGiveRecursive(mutex);vTaskDelay(1);xSemaphoreTakeRecursive(mutex,portMAX_DELAY);
    }
    return FEISHU_DATA_FULL;
}
static void writer_abort(feishu_data_writer_t *w)
{
    if(!w->id)return;
    int64_t id=w->id;
    feishu_data_trim(&store);
    if(feishu_data_abort(&store,w)==FEISHU_DATA_OK)writer_forget(id);
    else {
        for(size_t i=0;i<16;++i)if(writer_ids[i]==id)writer_abandoned[i]=true;
        w->id=0;
    }
}

esp_err_t feishu_cache_init(void)
{
    if(!mutex) mutex=xSemaphoreCreateRecursiveMutex();
    return mutex ? ESP_OK : ESP_ERR_NO_MEM;
}

void feishu_cache_set_scope(const char *app,const char *owner)
{
    if(!mutex || !app || !owner) return;
    xSemaphoreTakeRecursive(mutex,portMAX_DELAY);
    if(strcmp(scope_app,app) || strcmp(scope_owner,owner)) {++scope_version;atomic_store(&pending_absent,0);}
    snprintf(scope_app,sizeof(scope_app),"%s",app);
    snprintf(scope_owner,sizeof(scope_owner),"%s",owner);
    xSemaphoreGiveRecursive(mutex);
}

uint32_t feishu_cache_scope_version(void)
{
    if(!mutex)return 0;
    xSemaphoreTakeRecursive(mutex,portMAX_DELAY);uint32_t value=scope_version;xSemaphoreGiveRecursive(mutex);
    return value;
}

static esp_err_t mount_impl(void) {return feishu_pool_mount(FEISHU_CACHE_DIRECTORY);}

static void invalidate_leases(void)
{
    atomic_store(&pending_absent,0);
    memset(writer_ids,0,sizeof(writer_ids));memset(writer_abandoned,0,sizeof(writer_abandoned));
    memset(readers,0,sizeof(readers));
}
static void recovery_changed(const feishu_pool_info_t *info)
{
    if(info->id_floor>id_floor)id_floor=info->id_floor;
    if(info->generation!=atomic_load(&recovery_generation)) {
        atomic_store(&recovery_generation,info->generation);++scope_version;invalidate_leases();
    }
}
static void recover_io_handle(void)
{
    if(feishu_data_needs_reopen(&store)) {
        if(feishu_data_close(&store)==FEISHU_DATA_OK) {
            ++scope_version;invalidate_leases();
        }
    }
}
static esp_err_t open_impl(void)
{
    recover_io_handle();
    bool broken=feishu_data_broken(&store);
    if(feishu_data_is_open(&store) && !broken)return ESP_OK;
    if(feishu_data_is_open(&store)) {
        if(feishu_data_close(&store)!=FEISHU_DATA_OK)return ESP_FAIL;
        invalidate_leases();
    }
    esp_err_t mounted_ok=feishu_cache_mount();if(mounted_ok!=ESP_OK)return mounted_ok;
    feishu_pool_info_t info={.generation=atomic_load(&recovery_generation),.id_floor=id_floor};
    esp_err_t rc=feishu_pool_open_data(&store,FEISHU_CACHE_DIRECTORY,&id_floor,broken,&info);
    recovery_changed(&info);return rc;
}

esp_err_t feishu_cache_pool_lock(void)
{
    esp_err_t err=feishu_cache_init();if(err!=ESP_OK)return err;
    xSemaphoreTakeRecursive(mutex,portMAX_DELAY);
    recover_io_handle();
    err=mount_impl();
    if(err!=ESP_OK)xSemaphoreGiveRecursive(mutex);
    return err;
}
void feishu_cache_pool_unlock(void) { xSemaphoreGiveRecursive(mutex); }
esp_err_t feishu_cache_release_space(void)
{
    if(!mutex)return ESP_ERR_INVALID_STATE;
    xSemaphoreTakeRecursive(mutex,portMAX_DELAY);
    IO_BEGIN();
    esp_err_t err=open_impl();
    if(err==ESP_OK) {
        writers_reap();
        size_t reclaimed=0;
        int rc=feishu_pool_reclaim(&store,-1,&reclaimed);
        if(rc==FEISHU_DATA_OK)rc=feishu_data_shrink(&store);
        err=rc==FEISHU_DATA_OK ? ESP_OK:ESP_FAIL;
        feishu_data_trim(&store);
    }
    IO_END();xSemaphoreGiveRecursive(mutex);return err;
}

static void close_impl(void)
{
    atomic_store(&pending_absent,0);
    for(size_t i=0;i<16;++i)if(readers[i].count)return;
    if(feishu_data_close(&store)==FEISHU_DATA_OK) {
        memset(writer_ids,0,sizeof(writer_ids));memset(writer_abandoned,0,sizeof(writer_abandoned));
        memset(readers,0,sizeof(readers));
    }
}
static void unmount_impl(void)
{
    feishu_cache_close();
    if(!feishu_data_is_open(&store))feishu_pool_unmount();
}

static bool make_key(char *out,size_t size,const char *app,const char *owner,const char *name)
{
    if(!app || !owner || !*owner || !name) return false;
    int n=snprintf(out,size,"%u:%s/%u:%s/%s",(unsigned)strlen(app),app,(unsigned)strlen(owner),owner,name);
    return n>0 && (size_t)n<size;
}

static void unpin_impl(int64_t *pin)
{
    if(pin && *pin && feishu_data_is_open(&store)) reader_release(*pin);
    if(pin) *pin=0;
}

static void protect_impl(const char *name,int64_t *pin)
{
    char key[256];
    feishu_data_object_t obj;
    if(!pin || !feishu_data_is_open(&store) || !make_key(key,sizeof(key),scope_app,scope_owner,name)) return;
    if(feishu_data_find(&store,key,&obj)!=FEISHU_DATA_OK || *pin==obj.id) return;
    if(reader_acquire(obj.id)!=FEISHU_DATA_OK) return;
    feishu_cache_unpin(pin);
    *pin=obj.id;
}

static esp_err_t read_impl(const char *name,
                             void *data,size_t unit,size_t capacity,size_t *count,int64_t *pin)
{
    char key[256];
    if(!data || !unit || !count || !make_key(key,sizeof(key),scope_app,scope_owner,name)) return ESP_ERR_INVALID_ARG;
    *count=0;
    esp_err_t opened=feishu_cache_open();if(opened!=ESP_OK)return opened;
    feishu_data_object_t obj;
    int rc=feishu_data_find(&store,key,&obj);
    if(rc!=FEISHU_DATA_OK) return rc==FEISHU_DATA_NOTFOUND ? ESP_ERR_NOT_FOUND:ESP_FAIL;
    uint32_t header[3]; size_t got=0;
    rc=feishu_data_read(&store,obj.id,0,(uint8_t *)header,sizeof(header),&got);
    if(rc!=FEISHU_DATA_OK || got!=sizeof(header) || header[0]!=1 || header[1]!=unit || header[2]>capacity || header[2]>SIZE_MAX/unit ||
       obj.raw!=sizeof(header)+(uint64_t)unit*header[2]) return ESP_ERR_INVALID_RESPONSE;
    size_t total=unit*header[2], offset=0;
    for(unsigned seq=1;offset<total;++seq) {
        rc=feishu_data_read(&store,obj.id,seq,(uint8_t *)data+offset,total-offset,&got);
        if(rc!=FEISHU_DATA_OK || got==0 || got>total-offset) { memset(data,0,total); return ESP_ERR_INVALID_RESPONSE; }
        offset+=got;
    }
    if(pin && *pin!=obj.id) {
        rc=reader_acquire(obj.id);
        if(rc!=FEISHU_DATA_OK) return ESP_FAIL;
        feishu_cache_unpin(pin);
        *pin=obj.id;
    }
    *count=header[2];
    feishu_data_trim(&store);
    return ESP_OK;
}

static int append_mode(feishu_data_writer_t *w,const uint8_t *data,size_t size,bool compress)
{
    for(unsigned tries=0;tries<256;++tries) {
        bool room=cache_room();
        int rc=room ? feishu_data_append_span(&store,w,data,size,compress):FEISHU_DATA_FULL;
        if(rc!=FEISHU_DATA_FULL) return rc;
        size_t reclaimed;
        int collect=room && w->quota_limited ? feishu_pool_reclaim(&store,w->kind,&reclaimed):
            feishu_pool_reclaim(&store,-1,&reclaimed);
        if(collect!=FEISHU_DATA_OK)return rc;
        if(feishu_data_shrink(&store)!=FEISHU_DATA_OK)return rc;
        /* Every caller holds one adapter lock here. Yield it between small
         * transactions so a foreground disk hit can overtake reclamation. */
        xSemaphoreGiveRecursive(mutex);
        vTaskDelay(1);
        xSemaphoreTakeRecursive(mutex,portMAX_DELAY);
    }
    return FEISHU_DATA_FULL;
}
static int append(feishu_data_writer_t *w,const uint8_t *data,size_t size)
{ return append_mode(w,data,size,true); }

static esp_err_t write_impl(const char *name,
                              const void *data,size_t unit,size_t count)
{
    if(name && !strncmp(name,"pending/",8))atomic_store(&pending_absent,0);
    char key[256];
    if(!data || !unit || unit>UINT32_MAX || count>UINT32_MAX || count>SIZE_MAX/unit ||
       !make_key(key,sizeof(key),scope_app,scope_owner,name)) return ESP_ERR_INVALID_ARG;
    if(feishu_cache_open()!=ESP_OK) return ESP_FAIL;
    feishu_data_writer_t w={0};
    int rc=writer_begin(key,FEISHU_DATA_METADATA,false,&w);
    uint32_t header[3]={1,(uint32_t)unit,(uint32_t)count};
    if(rc==FEISHU_DATA_OK) rc=append(&w,(const uint8_t *)header,sizeof(header));
    for(size_t pos=0;rc==FEISHU_DATA_OK && pos<unit*count;) {
        size_t size=unit*count-pos;
        /* Small transactions bound the work repeated after quota pressure.
         * Publish the page only after every chunk has been stored. */
        if(size>FEISHU_BLOCK_BYTES)size=FEISHU_BLOCK_BYTES;
        rc=append(&w,(const uint8_t *)data+pos,size);
        pos+=size;
    }
    int64_t pending=w.id;
    if(rc==FEISHU_DATA_OK) rc=feishu_data_commit(&store,&w);
    if(rc==FEISHU_DATA_OK)writer_forget(pending);
    else writer_abort(&w);
    feishu_data_trim(&store);
    return rc==FEISHU_DATA_OK ? ESP_OK : rc==FEISHU_DATA_NOMEM ? ESP_ERR_NO_MEM : ESP_FAIL;
}

/* External serialization covers the connection, engine globals and pin IDs.
 * UI callbacks never call these disk operations; both callers are workers. */
#define LOCK() (xSemaphoreTakeRecursive(mutex,portMAX_DELAY),IO_BEGIN())
#define UNLOCK() (IO_END(),xSemaphoreGiveRecursive(mutex))
esp_err_t feishu_cache_mount(void)
{ if(!mutex) return ESP_ERR_INVALID_STATE; LOCK(); esp_err_t rc=mount_impl(); UNLOCK(); return rc; }
esp_err_t feishu_cache_open(void)
{ if(!mutex) return ESP_ERR_INVALID_STATE; LOCK(); esp_err_t rc=open_impl(); UNLOCK(); return rc; }
void feishu_cache_close(void)
{ if(!mutex) return; LOCK(); close_impl(); UNLOCK(); }
void feishu_cache_unmount(void)
{ if(!mutex) return; LOCK(); unmount_impl(); UNLOCK(); }
void feishu_cache_trim(void)
{ if(!mutex)return;LOCK();feishu_data_trim(&store);UNLOCK(); }
void feishu_cache_unpin(int64_t *pin)
{ if(!mutex) return; LOCK(); unpin_impl(pin); UNLOCK(); }
void feishu_cache_protect(const char *name,int64_t *pin)
{ if(!mutex) return; LOCK(); protect_impl(name,pin); UNLOCK(); }
esp_err_t feishu_cache_read(const char *name,void *data,size_t unit,size_t capacity,size_t *count,int64_t *pin)
{ if(!mutex) return ESP_ERR_INVALID_STATE; LOCK(); esp_err_t rc=read_impl(name,data,unit,capacity,count,pin); UNLOCK(); return rc; }
esp_err_t feishu_cache_write(const char *name,const void *data,size_t unit,size_t count)
{ if(!mutex) return ESP_ERR_INVALID_STATE; LOCK(); esp_err_t rc=write_impl(name,data,unit,count); UNLOCK(); return rc; }
esp_err_t feishu_cache_write_if_scope(uint32_t version,const char *name,const void *data,size_t unit,size_t count)
{
    if(!mutex)return ESP_ERR_INVALID_STATE;
    LOCK();esp_err_t rc=version==scope_version ? write_impl(name,data,unit,count) : ESP_ERR_INVALID_STATE;UNLOCK();return rc;
}

static esp_err_t data_result(int rc)
{
    return rc==FEISHU_DATA_OK ? ESP_OK : rc==FEISHU_DATA_NOMEM ? ESP_ERR_NO_MEM :
        rc==FEISHU_DATA_FULL ? ESP_ERR_INVALID_SIZE : rc==FEISHU_DATA_NOTFOUND ? ESP_ERR_NOT_FOUND : ESP_FAIL;
}
static esp_err_t text_begin(feishu_data_writer_t *w,bool required)
{
    if(!mutex || !w)return ESP_ERR_INVALID_ARG;
    LOCK();
    int rc=FEISHU_DATA_ERROR;
    if(open_impl()==ESP_OK && scope_owner[0]) {
        char key[48];snprintf(key,sizeof(key),"staging/%lu",(unsigned long)++staging_sequence);
        rc=writer_begin(key,FEISHU_DATA_TEXT,required,w);
    }
#ifdef ESP_PLATFORM
    if(rc!=FEISHU_DATA_OK)printf("FEISHU_TEXT_BEGIN rc=%d ext=%d sys=%d owner=%u required=%u\n",rc,
        feishu_data_diagnostics(&store).native_code,feishu_data_diagnostics(&store).system_code,
        (unsigned)(scope_owner[0]!=0),(unsigned)required);
#endif
    UNLOCK();return data_result(rc);
}
esp_err_t feishu_cache_text_begin(feishu_data_writer_t *w) {return text_begin(w,false);}
esp_err_t feishu_cache_draft_begin(feishu_data_writer_t *w) {return text_begin(w,true);}
esp_err_t feishu_cache_text_append(feishu_data_writer_t *w,const void *data,size_t size)
{
    /* Text-range addressing requires full blocks followed by one final tail. */
    if(!mutex || !w || !data || w->raw%FEISHU_BLOCK_BYTES)return ESP_ERR_INVALID_ARG;
    LOCK();int rc=append(w,data,size);
#ifdef ESP_PLATFORM
    if(rc!=FEISHU_DATA_OK)printf("FEISHU_TEXT_APPEND rc=%d ext=%d sys=%d raw=%u size=%u\n",rc,
        feishu_data_diagnostics(&store).native_code,feishu_data_diagnostics(&store).system_code,
        (unsigned)w->raw,(unsigned)size);
#endif
    UNLOCK();return data_result(rc);
}
esp_err_t feishu_cache_text_commit(feishu_data_writer_t *w,const char *message,int64_t *id)
{
    if(!mutex || !w || !message || !id)return ESP_ERR_INVALID_ARG;
    *id=0;
    LOCK();
    char name[96],key[256];snprintf(name,sizeof(name),"text/%s",message);
    int rc=FEISHU_DATA_MISUSE;
    int64_t pending=w->id;
    if(make_key(key,sizeof(key),scope_app,scope_owner,name))rc=feishu_data_commit_as(&store,w,key);
    if(rc==FEISHU_DATA_OK) {*id=pending;writer_forget(pending);}
    feishu_data_trim(&store);
    UNLOCK();return data_result(rc);
}
void feishu_cache_text_abort(feishu_data_writer_t *w)
{
    if(!mutex || !w || !w->id)return;
    LOCK();writer_abort(w);UNLOCK();
}
void feishu_cache_reap_abandoned(void)
{
    if(!mutex)return;
    LOCK();writers_reap();feishu_data_trim(&store);UNLOCK();
}
static int metadata_space(bool category_limited)
{
    writers_reap();size_t reclaimed=0;
    int rc=category_limited ? feishu_pool_reclaim(&store,FEISHU_DATA_METADATA,&reclaimed):
        feishu_pool_reclaim(&store,-1,&reclaimed);
    return rc==FEISHU_DATA_OK ? feishu_data_shrink(&store):rc;
}
esp_err_t feishu_cache_text_add_mention(feishu_data_writer_t *map,uint32_t scope,const char *key,const char *name)
{
    if(!mutex || !map || !key)return ESP_ERR_INVALID_ARG;
    LOCK();int rc=FEISHU_DATA_ERROR;
    if(scope!=scope_version || !scope_owner[0]) {UNLOCK();return ESP_ERR_INVALID_STATE;}
    esp_err_t opened=open_impl();
    if(opened!=ESP_OK) {UNLOCK();return opened;}
    for(unsigned tries=0;tries<256;++tries) {
        if(scope!=scope_version || !scope_owner[0]) {rc=FEISHU_DATA_MISUSE;break;}
        if(!map->id) {
            char staging[48];snprintf(staging,sizeof(staging),"mapping/%lu",(unsigned long)++staging_sequence);
            rc=writer_begin(staging,FEISHU_DATA_METADATA,false,map);
            if(rc!=FEISHU_DATA_OK)break;
        }
        if(!writer_registered(map->id) || scope!=scope_version) {rc=FEISHU_DATA_MISUSE;break;}
        map->quota_limited=false;
        rc=cache_room() ? feishu_data_mention_put(&store,map,key,name):FEISHU_DATA_FULL;
        if(rc!=FEISHU_DATA_FULL)break;
        int collect=metadata_space(map->quota_limited);if(collect!=FEISHU_DATA_OK)break;
        UNLOCK();vTaskDelay(1);LOCK();
    }
    UNLOCK();vTaskDelay(1);return data_result(rc);
}
typedef struct {
    feishu_data_writer_t *body,*map;
    int64_t body_id,map_id;
    uint32_t scope;
    bool build;
    bool (*cancelled)(void *);
    void *cancel_context;
    feishu_data_text_point_t points[8];size_t count;
    unsigned operations;
    esp_err_t error;
} text_mapping_t;
static bool mapping_valid(text_mapping_t *mapping)
{
    return mapping->scope==scope_version && feishu_data_is_open(&store) &&
        (!mapping->build || (writer_registered(mapping->body_id) && writer_registered(mapping->map_id)));
}
static bool mapping_read(void *context,uint64_t offset,void *data,size_t capacity,size_t *size)
{
    text_mapping_t *mapping=context;*size=0;
    if(offset%FEISHU_BLOCK_BYTES || offset/FEISHU_BLOCK_BYTES>UINT32_MAX) {mapping->error=ESP_ERR_INVALID_ARG;return false;}
    LOCK();int rc=mapping_valid(mapping) ? feishu_data_read(&store,mapping->body_id,
        (unsigned)(offset/FEISHU_BLOCK_BYTES),data,capacity,size):FEISHU_DATA_MISUSE;
    UNLOCK();
    if(rc!=FEISHU_DATA_OK)mapping->error=data_result(rc);
    return rc==FEISHU_DATA_OK;
}
static int mapping_lookup(void *context,const char *key,void *value,size_t capacity,size_t *size)
{
    text_mapping_t *mapping=context;
    LOCK();int rc=mapping_valid(mapping) ? feishu_data_mention_get(&store,mapping->map_id,key,value,capacity,size):FEISHU_DATA_MISUSE;
    UNLOCK();
    if(++mapping->operations%16==0)vTaskDelay(1);
    if(rc==FEISHU_DATA_OK)return 1;
    if(rc==FEISHU_DATA_DONE)return 0;
    if(rc==FEISHU_DATA_NOTFOUND && mapping->build) {
        mapping->error=feishu_cache_text_add_mention(mapping->map,mapping->scope,key,NULL);
        if(mapping->error==ESP_OK)return 0;
    } else mapping->error=data_result(rc);
    return -1;
}
static bool mapping_flush(text_mapping_t *mapping)
{
    if(!mapping->count)return true;
    LOCK();int rc=FEISHU_DATA_ERROR;
    for(unsigned tries=0;tries<256;++tries) {
        if(!mapping_valid(mapping)) {rc=FEISHU_DATA_MISUSE;break;}
        mapping->map->quota_limited=false;
        rc=cache_room() ? feishu_data_text_index(&store,mapping->body,mapping->map,mapping->points,mapping->count):FEISHU_DATA_FULL;
        if(rc!=FEISHU_DATA_FULL)break;
        int collect=metadata_space(mapping->map->quota_limited);if(collect!=FEISHU_DATA_OK)break;
        UNLOCK();vTaskDelay(1);LOCK();
    }
    UNLOCK();vTaskDelay(1);
    if(rc!=FEISHU_DATA_OK) {mapping->error=data_result(rc);return false;}
    mapping->count=0;return true;
}
static bool mapping_checkpoint(void *context,uint64_t raw,uint64_t logical)
{
    text_mapping_t *mapping=context;
    mapping->points[mapping->count++]=(feishu_data_text_point_t){raw,logical};
    return mapping->count<8 || mapping_flush(mapping);
}
static bool mapping_cancelled(void *context)
{
    text_mapping_t *mapping=context;
    return mapping->cancelled && mapping->cancelled(mapping->cancel_context);
}
esp_err_t feishu_cache_text_prepare(feishu_data_writer_t *body,feishu_data_writer_t *map,uint32_t scope,
                                     bool metadata_valid,char *preview,size_t preview_capacity,
                                     void *workspace,size_t workspace_size,uint64_t *length,bool *mapped,
                                     bool (*cancelled)(void *),void *cancel_context)
{
    if(!mutex || !body || !map || !preview || !preview_capacity || preview_capacity>256 ||
       !workspace || workspace_size<FEISHU_TEXT_MAP_WORKSPACE || !length || !mapped)return ESP_ERR_INVALID_ARG;
    *length=body->raw;*mapped=false;
    LOCK();bool active=scope==scope_version && writer_registered(body->id);UNLOCK();
    if(!active || (cancelled && cancelled(cancel_context)))return ESP_ERR_INVALID_STATE;
    char original[256];memcpy(original,preview,preview_capacity);
    text_mapping_t mapping={.body=body,.map=map,.body_id=body->id,.map_id=map->id,.scope=scope,
        .build=true,.cancelled=cancelled,.cancel_context=cancel_context,.error=ESP_OK};
    bool ready=false;
    if(metadata_valid && map->id) {
        LOCK();int clear=feishu_data_text_index_clear(&store,body->id);UNLOCK();
        feishu_text_map_io_t io={mapping_read,mapping_lookup,mapping_checkpoint,mapping_cancelled,&mapping};
        ready=clear==FEISHU_DATA_OK && feishu_text_map_scan(&io,body->raw,workspace,workspace_size,
            preview,preview_capacity,length) && mapping_flush(&mapping);
    }
    if(scope!=feishu_cache_scope_version() || (cancelled && cancelled(cancel_context))) {
        memcpy(preview,original,preview_capacity);return ESP_ERR_INVALID_STATE;
    }
    if(ready) {*mapped=true;feishu_cache_trim();return ESP_OK;}
    /* Metadata is disposable. Never discard a complete raw body just because
     * a name, index, allocation or quota could not be resolved. */
    LOCK();(void)feishu_data_text_index_clear(&store,body->id);writer_abort(map);UNLOCK();
    memcpy(preview,original,preview_capacity);*length=body->raw;feishu_cache_trim();return ESP_OK;
}
esp_err_t feishu_cache_text_publish(feishu_data_writer_t *body,feishu_data_writer_t *map,uint32_t scope,
                                     uint64_t logical,const char *message,int64_t *id)
{
    if(!mutex || !body || !message || !id)return ESP_ERR_INVALID_ARG;
    *id=0;LOCK();int rc=FEISHU_DATA_MISUSE;
    char name[96],key[256];snprintf(name,sizeof(name),"text/%s",message);
    int64_t body_id=body->id,map_id=map ? map->id:0;
    for(unsigned tries=0;tries<256;++tries) {
        if(scope!=scope_version || !writer_registered(body_id) || (map_id && !writer_registered(map_id)) ||
           !make_key(key,sizeof(key),scope_app,scope_owner,name)) {rc=FEISHU_DATA_MISUSE;break;}
        rc=map_id ? feishu_data_text_publish(&store,body,map,logical,key):feishu_data_commit_as(&store,body,key);
        if(rc!=FEISHU_DATA_FULL)break;
        if(metadata_space(false)!=FEISHU_DATA_OK)break;
        UNLOCK();vTaskDelay(1);LOCK();
    }
    if(rc==FEISHU_DATA_OK) {*id=body_id;writer_forget(body_id);if(map_id)writer_forget(map_id);}
    feishu_data_trim(&store);UNLOCK();return data_result(rc);
}
static int text_stat(const char *message,int64_t id,feishu_data_object_t *obj)
{
    char name[96],key[256];
    snprintf(name,sizeof(name),"text/%s",message);
    if(!make_key(key,sizeof(key),scope_app,scope_owner,name))return FEISHU_DATA_MISUSE;
    return feishu_data_stat(&store,id,key,obj);
}
esp_err_t feishu_cache_text_pin(const char *message,int64_t id,bool mapped,int64_t *pin,uint64_t *size)
{
    if(!mutex || !message || !pin || !size)return ESP_ERR_INVALID_ARG;
    LOCK();
    feishu_data_object_t obj={0};
    int rc=text_stat(message,id,&obj);
    feishu_data_text_view_t view={0};
    if(rc==FEISHU_DATA_OK && mapped)rc=obj.protected_data ? FEISHU_DATA_MISUSE:feishu_data_text_info(&store,id,obj.raw,&view);
    if(rc==FEISHU_DATA_OK && *pin!=id)rc=reader_acquire(id);
    if(rc==FEISHU_DATA_OK) {
        if(*pin!=id)unpin_impl(pin);
        *pin=id;*size=mapped ? view.logical:obj.raw;
    }
    UNLOCK();return data_result(rc);
}
static esp_err_t text_read(const char *message,int64_t id,bool mapped,uint64_t offset,
                            void *data,size_t capacity,size_t *size,bool trim)
{
    if(!mutex || !message || !data || !size)return ESP_ERR_INVALID_ARG;
    *size=0;
    LOCK();
    feishu_data_object_t obj={0};
    int rc=text_stat(message,id,&obj);
    uint8_t block[FEISHU_BLOCK_BYTES];
    if(rc==FEISHU_DATA_OK && mapped) {
        feishu_data_text_view_t view;feishu_data_text_point_t point;
        rc=obj.protected_data ? FEISHU_DATA_MISUSE:feishu_data_text_info(&store,id,obj.raw,&view);
        if(rc==FEISHU_DATA_OK && offset<view.logical) {
            rc=feishu_data_text_seek(&store,id,&view,offset,&point);
            if(rc==FEISHU_DATA_OK) {
                text_mapping_t mapping={.body_id=id,.map_id=view.map,.scope=scope_version,.error=ESP_OK};
                feishu_text_map_io_t io={mapping_read,mapping_lookup,NULL,NULL,&mapping};
                bool read=feishu_text_map_read(&io,obj.raw,point.raw,point.logical,offset,data,capacity,
                    block,sizeof(block),size);
                if(!read)rc=FEISHU_DATA_CORRUPT;
            }
        }
        if(trim)feishu_data_trim(&store);
        UNLOCK();return data_result(rc);
    }
    while(rc==FEISHU_DATA_OK && offset<obj.raw && *size<capacity) {
        size_t got=0;
        uint64_t seq=offset/FEISHU_BLOCK_BYTES;
        if(seq>UINT32_MAX) {rc=FEISHU_DATA_CORRUPT;break;}
        rc=feishu_data_read(&store,id,(unsigned)seq,block,sizeof(block),&got);
        if(rc!=FEISHU_DATA_OK)break;
        size_t skip=(size_t)(offset%FEISHU_BLOCK_BYTES);
        if(got<=skip) {rc=FEISHU_DATA_CORRUPT;break;}
        size_t n=got-skip;
        if(n>capacity-*size)n=capacity-*size;
        memcpy((uint8_t *)data+*size,block+skip,n);
        *size+=n;offset+=n;
    }
    if(trim)feishu_data_trim(&store);
    UNLOCK();return data_result(rc);
}

esp_err_t feishu_cache_text_read(const char *message,int64_t id,bool mapped,uint64_t offset,
                                 void *data,size_t capacity,size_t *size)
{ return text_read(message,id,mapped,offset,data,capacity,size,true); }

esp_err_t feishu_cache_text_read_stream(const char *message,int64_t id,bool mapped,uint64_t offset,
                                        void *data,size_t capacity,size_t *size)
{ return text_read(message,id,mapped,offset,data,capacity,size,false); }

esp_err_t feishu_cache_pending_read(unsigned slot,feishu_outbox_t *p)
{
    if(!mutex || !p || slot>=FEISHU_OUTBOX_SLOTS)return ESP_ERR_INVALID_ARG;
    if(atomic_load(&pending_absent)&(1u<<slot))return ESP_ERR_NOT_FOUND;
    LOCK();char name[32];snprintf(name,sizeof(name),"pending/%u",slot);size_t count=0;
    esp_err_t err=read_impl(name,p,sizeof(*p),1,&count,NULL);
    if(err==ESP_OK && (count!=1 || !feishu_outbox_valid(p)))err=ESP_ERR_INVALID_RESPONSE;
    if(err==ESP_OK && p->state==FEISHU_OUTBOX_EMPTY)err=ESP_ERR_NOT_FOUND;
    if(err==ESP_ERR_NOT_FOUND)atomic_fetch_or(&pending_absent,1u<<slot);
    /* Preserve readable send metadata even if its body is damaged. The UI can
     * show recovery/cancel choices instead of silently losing a pending send. */
    UNLOCK();return err;
}
esp_err_t feishu_cache_pending_write(unsigned slot,const feishu_outbox_t *p,feishu_data_writer_t *body)
{
    if(!mutex || !feishu_outbox_valid(p) || slot>=FEISHU_OUTBOX_SLOTS ||
       (body && (p->object_id!=body->id || p->length!=body->raw || p->state==FEISHU_OUTBOX_EMPTY)))return ESP_ERR_INVALID_ARG;
    atomic_fetch_and(&pending_absent,~(1u<<slot));
    LOCK();int rc=FEISHU_DATA_MISUSE;
    char name[32],meta_key[256],body_name[96],body_key[256];
    snprintf(name,sizeof(name),"pending/%u",slot);
    feishu_outbox_t previous={0};size_t count=0;
    esp_err_t prior=read_impl(name,&previous,sizeof(previous),1,&count,NULL);
    if(prior==ESP_OK && (count!=1 || !feishu_outbox_valid(&previous)))goto done;
    bool have_previous=prior==ESP_OK && count==1 && feishu_outbox_valid(&previous) && previous.state!=FEISHU_OUTBOX_EMPTY;
    /* An update must refer to the currently pending immutable body, not an
     * arbitrary protected object supplied by a stale UI action. */
    if(!body && (!have_previous || previous.object_id!=p->object_id || previous.length!=p->length ||
       strcmp(previous.body_name,p->body_name) || strcmp(previous.chat,p->chat) ||
       strcmp(previous.message,p->message) || strcmp(previous.uuid,p->uuid) ||
       p->offset<previous.offset || p->segment<previous.segment))goto done;
    if(prior!=ESP_OK && prior!=ESP_ERR_NOT_FOUND)goto done;
    if(open_impl()!=ESP_OK || !make_key(meta_key,sizeof(meta_key),scope_app,scope_owner,name))goto done;
    if(body) {
        snprintf(body_name,sizeof(body_name),"text/%s",p->body_name);
        if(!make_key(body_key,sizeof(body_key),scope_app,scope_owner,body_name))goto done;
    }
    feishu_data_writer_t metadata={0};
    rc=writer_begin(meta_key,FEISHU_DATA_METADATA,true,&metadata);
    uint32_t header[3]={1,sizeof(*p),1};
    if(rc==FEISHU_DATA_OK)rc=append(&metadata,(const uint8_t *)header,sizeof(header));
    const uint8_t *bytes=(const uint8_t *)p;
    for(size_t offset=0;rc==FEISHU_DATA_OK && offset<sizeof(*p);) {
        size_t n=sizeof(*p)-offset;if(n>FEISHU_BLOCK_BYTES)n=FEISHU_BLOCK_BYTES;
        rc=append(&metadata,bytes+offset,n);offset+=n;
    }
    int64_t release=0;
    if(have_previous && (p->state==FEISHU_OUTBOX_EMPTY || previous.object_id!=p->object_id)) {
        feishu_data_object_t object={0};
        if(text_stat(previous.body_name,previous.object_id,&object)==FEISHU_DATA_OK)release=previous.object_id;
    }
    int64_t meta_id=metadata.id,body_id=body ? body->id:0;
    if(rc==FEISHU_DATA_OK)rc=body ? feishu_data_commit_pair(&store,body,body_key,&metadata,meta_key,release):
        feishu_data_commit_pair(&store,&metadata,meta_key,NULL,NULL,release);
    if(rc==FEISHU_DATA_OK) {writer_forget(meta_id);if(body_id)writer_forget(body_id);}
    else writer_abort(&metadata);
done:
    if(rc==FEISHU_DATA_OK && p->state==FEISHU_OUTBOX_EMPTY)atomic_fetch_or(&pending_absent,1u<<slot);
    feishu_data_trim(&store);UNLOCK();return data_result(rc);
}

esp_err_t feishu_cache_blob_begin(feishu_data_kind_t kind,feishu_data_writer_t *w)
{
    if(!mutex || !w || kind<0 || kind>=FEISHU_DATA_KINDS)return ESP_ERR_INVALID_ARG;
    LOCK();int rc=FEISHU_DATA_ERROR;
    if(open_impl()==ESP_OK && scope_owner[0]) {
        char key[48];snprintf(key,sizeof(key),"staging/%lu",(unsigned long)++staging_sequence);
        rc=writer_begin(key,kind,false,w);
    }
    UNLOCK();return data_result(rc);
}
esp_err_t feishu_cache_blob_append(feishu_data_writer_t *w,const void *data,size_t size,bool compress)
{
    if(!mutex || !w || !data || w->raw%FEISHU_BLOCK_BYTES)return ESP_ERR_INVALID_ARG;
    LOCK();int rc=append_mode(w,data,size,compress);UNLOCK();return data_result(rc);
}
esp_err_t feishu_cache_blob_find(const char *name,int64_t *id,uint64_t *size)
{
    if(!mutex || !name || !id || !size)return ESP_ERR_INVALID_ARG;
    *id=0;*size=0;LOCK();char key[256];feishu_data_object_t object;
    int rc=FEISHU_DATA_NOTFOUND;
    if(open_impl()==ESP_OK && make_key(key,sizeof(key),scope_app,scope_owner,name))rc=feishu_data_find(&store,key,&object);
    if(rc==FEISHU_DATA_OK) {*id=object.id;*size=object.raw;}
    UNLOCK();return data_result(rc);
}
esp_err_t feishu_cache_blob_pin(const char *name,int64_t id,int64_t *pin,uint64_t *size)
{
    if(!mutex || !name || !pin || !size)return ESP_ERR_INVALID_ARG;
    LOCK();char key[256];feishu_data_object_t object;int rc=FEISHU_DATA_MISUSE;
    if(feishu_data_is_open(&store) && make_key(key,sizeof(key),scope_app,scope_owner,name))rc=feishu_data_stat(&store,id,key,&object);
    if(rc==FEISHU_DATA_OK && *pin!=id)rc=reader_acquire(id);
    if(rc==FEISHU_DATA_OK) {if(*pin!=id)unpin_impl(pin);*pin=id;*size=object.raw;}
    UNLOCK();return data_result(rc);
}
esp_err_t feishu_cache_blob_read(const char *name,int64_t id,uint64_t offset,void *data,size_t capacity,size_t *size)
{
    if(!mutex || !name || !data || !size)return ESP_ERR_INVALID_ARG;
    *size=0;LOCK();char key[256];feishu_data_object_t object;int rc=FEISHU_DATA_MISUSE;
    if(feishu_data_is_open(&store) && make_key(key,sizeof(key),scope_app,scope_owner,name))rc=feishu_data_stat(&store,id,key,&object);
    uint8_t block[FEISHU_BLOCK_BYTES];
    while(rc==FEISHU_DATA_OK && offset<object.raw && *size<capacity) {
        uint64_t sequence=offset/FEISHU_BLOCK_BYTES;
        if(sequence>INT32_MAX) {rc=FEISHU_DATA_CORRUPT;break;}
        size_t skip=(size_t)(offset%FEISHU_BLOCK_BYTES),got=0;
        bool direct=!skip && capacity-*size>=FEISHU_BLOCK_BYTES;
        uint8_t *target=direct ? (uint8_t *)data+*size:block;
        rc=feishu_data_read(&store,id,(unsigned)sequence,target,FEISHU_BLOCK_BYTES,&got);
        if(rc!=FEISHU_DATA_OK)break;
        if(got<=skip || (got!=FEISHU_BLOCK_BYTES && offset-skip+got!=object.raw)) {rc=FEISHU_DATA_CORRUPT;break;}
        size_t n=got-skip;if(n>capacity-*size)n=capacity-*size;
        if(!direct)memcpy((uint8_t *)data+*size,block+skip,n);
        *size+=n;offset+=n;
    }
    if(rc!=FEISHU_DATA_OK)feishu_data_trim(&store);
    UNLOCK();return data_result(rc);
}
esp_err_t feishu_cache_blob_commit(feishu_data_writer_t *first,const char *name,
                                    feishu_data_writer_t *second,const char *second_name,int64_t *pin)
{
    if(!mutex || !first || !name || !pin || (second && !second_name))return ESP_ERR_INVALID_ARG;
    LOCK();char first_key[256],second_key[256];int rc=FEISHU_DATA_MISUSE;
    int64_t first_id=first->id,second_id=second ? second->id:0;
    if(make_key(first_key,sizeof(first_key),scope_app,scope_owner,name) &&
       (!second || make_key(second_key,sizeof(second_key),scope_app,scope_owner,second_name)))
        rc=feishu_data_commit_pair(&store,first,first_key,second,second ? second_key:NULL,0);
    if(rc==FEISHU_DATA_OK) {
        writer_forget(first_id);if(second_id)writer_forget(second_id);
        rc=reader_acquire(first_id);
        if(rc==FEISHU_DATA_OK) {unpin_impl(pin);*pin=first_id;}
    }
    feishu_data_trim(&store);UNLOCK();return data_result(rc);
}

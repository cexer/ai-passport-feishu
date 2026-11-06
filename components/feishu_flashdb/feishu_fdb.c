#include "feishu_fdb.h"
#include "flashdb.h"
#include "fdb_low_lvl.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static struct fdb_kvdb database;
static feishu_fdb_io_t media;
static bool claimed, opened, failed;
static size_t used_upper,catalog_bytes;
static uint8_t root_headers[64];
static uint64_t mutation_epoch,root_epoch;
static bool usage_may_overcount;
uint64_t feishu_fdb_root_revision(void) {return root_epoch;}
uint64_t feishu_fdb_revision(void) {return mutation_epoch;}
static int root_header(const char *key)
{
    if(strlen(key)!=4 || key[0]!='r' || (key[3]!='a' && key[3]!='b'))return -1;
    unsigned n=0;
    for(unsigned i=1;i<3;++i) {
        unsigned c=(unsigned char)key[i],v;
        if(c>='0' && c<='9')v=c-'0';else if(c>='a' && c<='f')v=c-'a'+10;else return -1;
        n=n*16+v;
    }
    return (int)(n*2+(key[3]=='b'));
}
static size_t record_bytes(const char *key,size_t size) {return strlen(key)+size+64;}
static bool count_record(const char *key,size_t size,void *ctx)
{
    (void)ctx;used_upper+=record_bytes(key,size);
    if(!strcmp(key,"@catalog"))catalog_bytes=record_bytes(key,size);
    int root=root_header(key);if(root>=0 && size==32)root_headers[root/8]|=(uint8_t)(1u<<(root%8));
    return false;
}
static bool recount(void)
{
    used_upper=catalog_bytes=0;memset(root_headers,0,sizeof(root_headers));
    bool ok=feishu_fdb_visit(count_record,NULL)==FEISHU_FDB_OK;
    if(ok)usage_may_overcount=false;
    return ok;
}
static struct fal_partition pool = {
    .magic_word=0x45503130, .name="feishu-pool", .flash_name="feishu-raw"
};
static const struct fal_flash_dev device = {
    .name="feishu-raw", .blk_size=4096, .write_gran=8
};
int fal_init(void) { return 1; }
const struct fal_partition *fal_partition_find(const char *name)
{ return !strcmp(name,pool.name) ? &pool : NULL; }
const struct fal_flash_dev *fal_flash_device_find(const char *name)
{ return !strcmp(name,device.name) ? &device : NULL; }

static bool valid(const struct fal_partition *part,uint32_t pos,size_t size)
{
    return claimed && !failed && part==&pool && pos<=media.size &&
           size<=media.size-pos && size<=INT_MAX;
}
int fal_partition_read(const struct fal_partition *part,uint32_t pos,uint8_t *buf,size_t size)
{
    if(!valid(part,pos,size) || media.read(media.ctx,pos,buf,size)!=(int)size) {
        if(buf) memset(buf,0,size);
        failed=true;
        return -1;
    }
    return (int)size;
}
int fal_partition_write(const struct fal_partition *part,uint32_t pos,const uint8_t *buf,size_t size)
{
    ++mutation_epoch;
    if(!valid(part,pos,size) || media.write(media.ctx,pos,buf,size)!=(int)size) {
        failed=true;
        return -1;
    }
    return (int)size;
}
int fal_partition_erase(const struct fal_partition *part,uint32_t pos,size_t size)
{
    ++mutation_epoch;
    if(!valid(part,pos,size) || pos%4096 || size%4096 ||
       !size) {
        failed=true;
        return -1;
    }
    /* Logical KV sectors are 16 KiB, physical erase units only 4 KiB.
     * Preserve the GC/recovery header until every payload unit is erased.
     * Otherwise a reset after the first unit leaves an unknown header above
     * occupied bytes, which safe non-destructive open must reject. */
    for(size_t base=0;base<size;base+=16384) {
        size_t chunk=size-base;if(chunk>16384)chunk=16384;
        for(size_t offset=4096;offset<chunk;offset+=4096) {
            if(media.erase(media.ctx,pos+(uint32_t)(base+offset),4096)!=4096)goto io_failed;
        }
        if(media.erase(media.ctx,pos+(uint32_t)base,4096)!=4096)goto io_failed;
    }
    return (int)size;
io_failed:
    failed=true;return -1;
}
static int result(fdb_err_t rc)
{
    if(failed || database.parent.io_failed) return FEISHU_FDB_IO;
    if(rc==FDB_NO_ERR) return FEISHU_FDB_OK;
    if(rc==FDB_SAVED_FULL) return FEISHU_FDB_FULL;
    if(rc==FDB_READ_ERR || rc==FDB_WRITE_ERR || rc==FDB_ERASE_ERR) return FEISHU_FDB_IO;
    return FEISHU_FDB_ERROR;
}
int feishu_fdb_status(void)
{
    if(failed || database.parent.io_failed) return FEISHU_FDB_IO;
    return opened ? FEISHU_FDB_OK : FEISHU_FDB_INVALID;
}
void feishu_fdb_close(void)
{
    ++root_epoch;usage_may_overcount=false;
    ++mutation_epoch;
    if(claimed) fdb_kvdb_deinit(&database);
    memset(&database,0,sizeof(database));
    memset(&media,0,sizeof(media));
    claimed=opened=failed=false;
    used_upper=catalog_bytes=0;memset(root_headers,0,sizeof(root_headers));
}
int feishu_fdb_open(const feishu_fdb_io_t *io,bool allow_blank)
{
    if(claimed) return FEISHU_FDB_BUSY;
    if(!io || !io->read || !io->write || !io->erase || io->size<32768 ||
       io->size%16384 || io->size>UINT32_MAX) return FEISHU_FDB_INVALID;
    media=*io;
    ++mutation_epoch;
    claimed=true;
    pool.len=io->size;
    bool blank=true;
    uint8_t block[256];
    /* A full scan is required only for an explicitly permitted blank format. */
    if(allow_blank) {
        for(uint32_t at=0; at<io->size; at+=sizeof(block)) {
            if(fal_partition_read(&pool,at,block,sizeof(block))<0) {
                feishu_fdb_close();
                return FEISHU_FDB_IO;
            }
            for(size_t i=0;i<sizeof(block);++i) if(block[i]!=0xff) blank=false;
        }
    } else blank=false;
    uint32_t sector=16384;
    bool no_format=!blank;
    fdb_kvdb_control(&database,FDB_KVDB_CTRL_SET_SEC_SIZE,&sector);
    fdb_kvdb_control(&database,FDB_KVDB_CTRL_SET_NOT_FORMAT,&no_format);
    int rc=result(fdb_kvdb_init(&database,"feishu",pool.name,NULL,NULL));
    if(rc!=FEISHU_FDB_OK) {
        feishu_fdb_close();
        return rc==FEISHU_FDB_IO ? rc : FEISHU_FDB_FORMAT;
    }
    database.parent.not_formatable=true;
    opened=true;
    if(!recount()){feishu_fdb_close();return FEISHU_FDB_IO;}
    return FEISHU_FDB_OK;
}
static bool key_valid(const char *key)
{ return key && *key && strlen(key)<FDB_KV_NAME_MAX; }
int feishu_fdb_put(const char *key,const void *data,size_t size)
{
    int rc=feishu_fdb_status();
    if(rc) return rc;
    if(!key_valid(key) || !data || !size) return FEISHU_FDB_INVALID;
    /* Object adapter splits larger values into bounded immutable chunks. */
    if(size>8192) return FEISHU_FDB_TOOBIG;
    if(key[0]=='r')++root_epoch;
    struct fdb_blob blob;
    int saved=result(fdb_kv_set_blob(&database,key,fdb_blob_make(&blob,data,size)));
    if(!saved) {
        size_t n=record_bytes(key,size),old=0;
        int root=root_header(key);
        if(!strcmp(key,"@catalog")){old=catalog_bytes;catalog_bytes=n;}
        else if(root>=0) {
            if(size!=32 || !(root_headers[root/8]&(1u<<(root%8))))usage_may_overcount=true;
            if(root_headers[root/8]&(1u<<(root%8)))old=record_bytes(key,32);
            if(size==32)root_headers[root/8]|=(uint8_t)(1u<<(root%8));
            else root_headers[root/8]&=(uint8_t)~(1u<<(root%8));
        }
        else usage_may_overcount=true;
        used_upper=used_upper>=old?used_upper-old+n:n;
    }
    return saved;
}
int feishu_fdb_get(const char *key,void *data,size_t capacity,size_t *size)
{
    if(size) *size=0;
    int rc=feishu_fdb_status();
    if(rc) return rc;
    if(!key_valid(key) || !size || (!data && capacity)) return FEISHU_FDB_INVALID;
    struct fdb_kv object={0};
    fdb_kv_t found=fdb_kv_get_obj(&database,key,&object);
    if(feishu_fdb_status()) return FEISHU_FDB_IO;
    if(!found) return FEISHU_FDB_NOTFOUND;
    *size=object.value_len;
    if(capacity<object.value_len) return FEISHU_FDB_TOOBIG;
    struct fdb_blob blob;
    fdb_blob_make(&blob,data,capacity);
    fdb_kv_to_blob(&object,&blob);
    size_t count=fdb_blob_read((fdb_db_t)&database,&blob);
    rc=feishu_fdb_status();
    if(rc || count!=object.value_len) {
        if(data) memset(data,0,capacity);
        *size=0;
        return FEISHU_FDB_IO;
    }
    return FEISHU_FDB_OK;
}
int feishu_fdb_delete(const char *key)
{
    int rc=feishu_fdb_status();
    if(rc) return rc;
    if(!key_valid(key)) return FEISHU_FDB_INVALID;
    struct fdb_kv object={0};
    fdb_kv_t found=fdb_kv_get_obj(&database,key,&object);
    if(feishu_fdb_status())return FEISHU_FDB_IO;
    size_t removed=found?record_bytes(key,object.value_len):0;
    if(key[0]=='r')++root_epoch;
    int saved=result(fdb_kv_del(&database,key));
    if(!saved)used_upper=used_upper>=removed?used_upper-removed:0;
    return saved;
}
int feishu_fdb_visit(feishu_fdb_visit_t visitor,void *ctx)
{
    int rc=feishu_fdb_status();
    if(rc) return rc;
    if(!visitor) return FEISHU_FDB_INVALID;
    struct fdb_kv_iterator iterator;
    fdb_kv_iterator_init(&database,&iterator);
    while(fdb_kv_iterate(&database,&iterator)) {
        if(feishu_fdb_status()) break;
        if(visitor(iterator.curr_kv.name,iterator.curr_kv.value_len,ctx)) break;
    }
    /* An early visitor stop must return the borrowed window as well. */
    _fdb_read_cache_release((fdb_db_t)&database);
    return feishu_fdb_status();
}
bool feishu_fdb_room(size_t margin)
{
    if(feishu_fdb_status()!=FEISHU_FDB_OK)return false;
    size_t budget=media.size-32768;
    if(used_upper<=budget && margin<=budget-used_upper)return true;
    /* Retried immutable-key writes may overcount. Recount only under pressure
     * rather than scanning the pool for each ordinary append. */
    return usage_may_overcount && recount() && used_upper<=budget && margin<=budget-used_upper;
}

int feishu_fdb_collect_if(feishu_fdb_visit_t match,void *ctx,size_t *bytes)
{
    if(!match || !bytes)return FEISHU_FDB_INVALID;
    *bytes=0;int rc=feishu_fdb_status();if(rc)return rc;
    typedef struct {char key[FDB_KV_NAME_MAX+1];uint32_t address;size_t size;} candidate_t;
    candidate_t single={0},*allocated=calloc(32,sizeof(candidate_t));
    candidate_t *items=allocated?allocated:&single;unsigned count=0,capacity=allocated?32u:1u;
    struct fdb_kv_iterator iterator;fdb_kv_iterator_init(&database,&iterator);
    while(count<capacity && fdb_kv_iterate(&database,&iterator)) {
        if(feishu_fdb_status()!=FEISHU_FDB_OK)break;
        if(match(iterator.curr_kv.name,iterator.curr_kv.value_len,ctx)) {
            candidate_t *item=&items[count++];
            memcpy(item->key,iterator.curr_kv.name,iterator.curr_kv.name_len+1);
            item->address=iterator.curr_kv.addr.start;item->size=iterator.curr_kv.value_len;
        }
    }
    _fdb_read_cache_release((fdb_db_t)&database);
    rc=feishu_fdb_status();
    if(!rc)for(unsigned i=0;i<count;++i) {
        _fdb_kv_cache_hint(&database,items[i].key,items[i].address);
        rc=feishu_fdb_status();if(rc)break;
    }
    if(!rc)for(unsigned i=0;i<count;++i) {
        rc=feishu_fdb_delete(items[i].key);if(rc)break;
        *bytes+=items[i].size;
    }
    free(allocated);return rc;
}

int feishu_fdb_prefetch(const char *prefix,const char *first,const char *last,unsigned count)
{
    int rc=feishu_fdb_status();if(rc)return rc;
    if(!key_valid(prefix) || !key_valid(first) || !key_valid(last) || !count)return FEISHU_FDB_INVALID;
    bool cached=_fdb_kv_cache_contains(&database,first) && _fdb_kv_cache_contains(&database,last);
    _fdb_read_cache_release((fdb_db_t)&database);
    rc=feishu_fdb_status();if(rc || cached)return rc;
    size_t length=strlen(prefix);unsigned found=0;
    struct fdb_kv_iterator iterator;fdb_kv_iterator_init(&database,&iterator);
    while(fdb_kv_iterate(&database,&iterator)) {
        if(feishu_fdb_status()!=FEISHU_FDB_OK)break;
        if(!strncmp(iterator.curr_kv.name,prefix,length)) {
            _fdb_kv_cache_hint(&database,iterator.curr_kv.name,iterator.curr_kv.addr.start);
            if(++found>=count)break;
        }
    }
    _fdb_read_cache_release((fdb_db_t)&database);
    return feishu_fdb_status();
}

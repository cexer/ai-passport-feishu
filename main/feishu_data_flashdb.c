/* Raw object backend. CMake selects this or the reference SQLite implementation. */
#include "feishu_data.h"
#include "feishu_data_flashdb.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SLOTS 128
#define RECORD_BYTES 48
#define CATALOG_BYTES (32+SLOTS*RECORD_BYTES)
enum {UNUSED,STAGING,LIVE,RETIRED};
typedef struct {
    uint64_t id;
    uint32_t raw,stored,sequence,hash,touch,key_tag;
    uint8_t state,kind,protected_data;
    uint32_t point_batches;
    uint64_t map;
} entry_t;
static entry_t entries[SLOTS];
static uint8_t pins[SLOTS];
static uint64_t high,next;
static uint32_t clock_value;
static feishu_data_t *owner;
static feishu_fdb_io_t media;
static int last_error;
static const uint32_t quotas[]={128*1024,300*1024,300*1024,300*1024,0};
static uint64_t metadata_usage(void);
static bool map_referenced(uint64_t id);
static void u32(uint8_t *p,uint32_t n) {for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(n>>(8*i));}
static void u64(uint8_t *p,uint64_t n) {for(unsigned i=0;i<8;++i)p[i]=(uint8_t)(n>>(8*i));}
static uint32_t g32(const uint8_t *p) {uint32_t n=0;for(unsigned i=0;i<4;++i)n|=(uint32_t)p[i]<<(8*i);return n;}
static uint64_t g64(const uint8_t *p) {uint64_t n=0;for(unsigned i=0;i<8;++i)n|=(uint64_t)p[i]<<(8*i);return n;}
static int mapped(int rc)
{
    last_error=rc;
    switch(rc) {
    case FEISHU_FDB_OK:return FEISHU_DATA_OK;
    case FEISHU_FDB_IO:return FEISHU_DATA_IOERR;
    case FEISHU_FDB_FORMAT:return FEISHU_DATA_BAD_FORMAT;
    case FEISHU_FDB_FULL:return FEISHU_DATA_FULL;
    case FEISHU_FDB_NOTFOUND:return FEISHU_DATA_NOTFOUND;
    case FEISHU_FDB_TOOBIG:return FEISHU_DATA_TOOBIG;
    case FEISHU_FDB_BUSY:return FEISHU_DATA_BUSY;
    default:return FEISHU_DATA_ERROR;
    }
}
static bool valid(feishu_data_t *s) {return s && owner==s && s->db==entries;}
#define CHECK_STORE(s) do { \
    if(!valid(s))return FEISHU_DATA_MISUSE; \
    int engine_status=mapped(feishu_fdb_status()); \
    if(engine_status)return engine_status; \
} while(0)
static entry_t *by_id(int64_t id)
{for(unsigned i=0;i<SLOTS;++i)if(entries[i].state && entries[i].id==(uint64_t)id)return &entries[i];return NULL;}
static bool key_valid(const char *key) {return key && *key && strlen(key)<=255;}
static uint32_t hash(const char *key) {return feishu_block_crc32((const uint8_t *)key,strlen(key));}
static void key_name(char *out,uint64_t id,uint32_t tag) {snprintf(out,40,"k%016llx.%08x",(unsigned long long)id,(unsigned)tag);}
static void block_name(char *out,uint64_t id,uint32_t sequence)
{snprintf(out,40,"b%016llx.%08x",(unsigned long long)id,(unsigned)sequence);}
static int key_read(entry_t *e,char *key)
{
    char name[40];size_t n=0;key_name(name,e->id,e->key_tag);
    int rc=mapped(feishu_fdb_get(name,key,256,&n));
    if(rc) return rc;
    if(!n || n>256 || key[n-1] || strlen(key)!=n-1 || hash(key)!=e->hash) return FEISHU_DATA_CORRUPT;
    return FEISHU_DATA_OK;
}
static int matching(const char *key,unsigned state,entry_t **found)
{
    *found=NULL;uint32_t h=hash(key);
    for(unsigned i=0;i<SLOTS;++i)if(entries[i].state==state && entries[i].hash==h) {
        char actual[256];int rc=key_read(&entries[i],actual);if(rc)return rc;
        if(!strcmp(key,actual)) {*found=&entries[i];break;}
    }
    return FEISHU_DATA_OK;
}
static int save_catalog_masked(const uint8_t omitted[16])
{
    uint8_t *bytes=calloc(1,CATALOG_BYTES);
    if(!bytes)return FEISHU_DATA_NOMEM;
    memcpy(bytes,"FKV1",4);u32(bytes+4,SLOTS);u64(bytes+8,high);u32(bytes+16,clock_value);
    for(unsigned i=0;i<SLOTS;++i) {
        entry_t *e=&entries[i];uint8_t *p=bytes+32+i*RECORD_BYTES;
        /* In-flight chunks are recoverable garbage after reboot; only fully
         * published versions enter the durable catalog. */
        if((e->state!=LIVE && e->state!=RETIRED) || (omitted && (omitted[i/8]&(1u<<(i%8)))))continue;
        u64(p,e->id);u32(p+8,e->raw);u32(p+12,e->stored);u32(p+16,e->sequence);
        u32(p+20,e->hash);u32(p+24,e->touch);u32(p+28,e->key_tag);p[32]=e->state;p[33]=e->kind;p[34]=e->protected_data;u32(p+36,e->point_batches);u64(p+40,e->map);
    }
    u32(bytes+20,feishu_block_crc32(bytes+32,CATALOG_BYTES-32));
    u32(bytes+24,feishu_block_crc32(bytes,24));
    int rc=mapped(feishu_fdb_put("@catalog",bytes,CATALOG_BYTES));free(bytes);return rc;
}
static int save_catalog(void) {return save_catalog_masked(NULL);}
static int load_catalog(void)
{
    uint8_t *bytes=malloc(CATALOG_BYTES);if(!bytes)return FEISHU_DATA_NOMEM;
    size_t size=0;int rc=mapped(feishu_fdb_get("@catalog",bytes,CATALOG_BYTES,&size));
    /* Bootstrap is explicit: no missing catalog may silently reset an
     * existing store. The host/target preparation step creates it separately. */
    if(rc==FEISHU_DATA_OK) {
        if(size!=CATALOG_BYTES || memcmp(bytes,"FKV1",4) || g32(bytes+4)!=SLOTS ||
           g32(bytes+20)!=feishu_block_crc32(bytes+32,CATALOG_BYTES-32) ||
           g32(bytes+24)!=feishu_block_crc32(bytes,24) || g64(bytes+8)>INT64_MAX)rc=FEISHU_DATA_CORRUPT;
        else {
            high=g64(bytes+8);next=high;clock_value=g32(bytes+16);
            for(unsigned i=0;i<SLOTS && !rc;++i) {
                uint8_t *p=bytes+32+i*RECORD_BYTES;entry_t *e=&entries[i];
                *e=(entry_t){g64(p),g32(p+8),g32(p+12),g32(p+16),g32(p+20),g32(p+24),g32(p+28),p[32],p[33],p[34],g32(p+36),g64(p+40)};
                if(e->state && ((e->state!=LIVE && e->state!=RETIRED) || !e->id || e->id>high ||
                   e->kind>=FEISHU_DATA_KINDS || e->protected_data>1 || e->touch>clock_value))rc=FEISHU_DATA_CORRUPT;
                for(unsigned j=0;j<i;++j)if(e->state && entries[j].state && entries[j].id==e->id)rc=FEISHU_DATA_CORRUPT;
            }
        }
    }
    free(bytes);return rc;
}
int feishu_data_flashdb_configure(const feishu_fdb_io_t *io)
{if(owner)return FEISHU_DATA_BUSY;if(!io)return FEISHU_DATA_MISUSE;media=*io;return FEISHU_DATA_OK;}
static bool any_key(const char *key,size_t size,void *ctx)
{(void)key;(void)size;*(bool *)ctx=true;return true;}
/* Only for an explicitly prepared, empty KV store, never ordinary open. */
int feishu_data_flashdb_initialize(void)
{
    if(owner)return FEISHU_DATA_BUSY;
    bool borrowed=feishu_fdb_status()==FEISHU_FDB_OK;
    int rc=borrowed?FEISHU_DATA_OK:mapped(feishu_fdb_open(&media,false));if(rc)return rc;
    bool exists=false;rc=mapped(feishu_fdb_visit(any_key,&exists));
    if(rc || exists) {if(!borrowed)feishu_fdb_close();return rc?rc:FEISHU_DATA_CONSTRAINT;}
    memset(entries,0,sizeof(entries));high=next=clock_value=0;
    rc=save_catalog();if(!borrowed)feishu_fdb_close();return rc;
}
bool feishu_data_is_open(const feishu_data_t *s) {return s && owner==s && s->db==entries;}
bool feishu_data_needs_reopen(const feishu_data_t *s)
{return feishu_data_is_open(s) && feishu_fdb_status()!=FEISHU_FDB_OK;}
bool feishu_data_broken(feishu_data_t *s) {(void)s;return false;}
const char *feishu_data_backend_name(void) {return "flashdb";}
feishu_data_diagnostics_t feishu_data_diagnostics(const feishu_data_t *s)
{return (feishu_data_diagnostics_t){feishu_data_is_open(s)?last_error:0,0};}
void feishu_data_io_begin(void) {}
void feishu_data_io_end(void) {}
int feishu_data_open(feishu_data_t *s,const char *path,const char *vfs)
{
    if(!s || s->db || !path || vfs)return FEISHU_DATA_MISUSE;
    if(owner)return FEISHU_DATA_BUSY;
    memset(entries,0,sizeof(entries));memset(pins,0,sizeof(pins));
    int rc=feishu_fdb_status()==FEISHU_FDB_OK?FEISHU_DATA_OK:mapped(feishu_fdb_open(&media,false));if(rc)return rc;
    rc=load_catalog();
    if(rc) {feishu_fdb_close();memset(entries,0,sizeof(entries));return rc;}
    owner=s;s->db=entries;return FEISHU_DATA_OK;
}
int feishu_data_close(feishu_data_t *s)
{
    if(!s || !s->db)return FEISHU_DATA_OK;
    if(!valid(s))return FEISHU_DATA_MISUSE;
    feishu_fdb_close();memset(entries,0,sizeof(entries));memset(pins,0,sizeof(pins));
    s->db=NULL;owner=NULL;return FEISHU_DATA_OK;
}
int feishu_data_id_floor(feishu_data_t *s,uint64_t *floor)
{
    CHECK_STORE(s);
    if(!valid(s) || !floor || *floor>INT64_MAX)return FEISHU_DATA_MISUSE;
    if(*floor>high) {uint64_t old=high;high=*floor;int rc=save_catalog();if(rc){high=old;return rc;}next=high;}
    *floor=high;return FEISHU_DATA_OK;
}
void feishu_data_trim(feishu_data_t *s) {(void)s;}
int feishu_data_shrink(feishu_data_t *s) {return valid(s)?FEISHU_DATA_OK:FEISHU_DATA_MISUSE;}
int feishu_data_begin(feishu_data_t *s,const char *key,feishu_data_kind_t kind,bool protect,feishu_data_writer_t *w)
{
    CHECK_STORE(s);
    if(!valid(s) || !key_valid(key) || kind<0 || kind>=FEISHU_DATA_KINDS || !w)return FEISHU_DATA_MISUSE;
    entry_t *e=NULL;for(unsigned i=0;i<SLOTS;++i)if(!entries[i].state){e=&entries[i];break;}
    /* Catalog capacity is not Flash capacity. Reuse an unpinned superseded
     * version directly instead of making the caller scan every raw key to
     * reclaim unrelated chunks before it can obtain a catalog slot.
     * Keep the old entry unchanged until the new immutable key is durable.
     * A reset before publication may restore that retired entry; the new
     * staging chunks remain ordinary recoverable garbage. IDs never repeat. */
    if(!e)for(unsigned i=0;i<SLOTS;++i)
        if(entries[i].state==RETIRED && !pins[i] && !map_referenced(entries[i].id) &&
           (!e || entries[i].touch<e->touch))e=&entries[i];
    if(!e)return FEISHU_DATA_FULL;
    if(next==high) {
        if(high>INT64_MAX-64)return FEISHU_DATA_FULL;
        high+=64;int rc=save_catalog();if(rc){high-=64;return rc;}
    }
    uint64_t id=++next;char name[40];key_name(name,id,0);
    int rc=mapped(feishu_fdb_put(name,key,strlen(key)+1));if(rc)return rc;
    *e=(entry_t){.id=id,.hash=hash(key),.state=STAGING,.kind=(uint8_t)kind,.protected_data=protect};
    pins[e-entries]=1;*w=(feishu_data_writer_t){.id=(int64_t)id,.kind=kind};return FEISHU_DATA_OK;
}
static entry_t *writer_entry(const feishu_data_writer_t *w)
{
    if(!w || w->id<=0)return NULL;
    entry_t *e=by_id(w->id);
    return e && e->state==STAGING && e->kind==w->kind && e->raw==w->raw &&
           e->stored==w->stored && e->sequence==w->sequence ? e:NULL;
}
int feishu_data_append_span(feishu_data_t *s,feishu_data_writer_t *w,const uint8_t *data,size_t size,bool compress)
{
    CHECK_STORE(s);
    if(!valid(s) || !data || !size || size>8*FEISHU_BLOCK_BYTES)return FEISHU_DATA_MISUSE;
    entry_t *e=writer_entry(w);if(!e)return FEISHU_DATA_MISUSE;
    w->quota_limited=false;if(e->raw>UINT32_MAX-size || e->sequence>UINT32_MAX-8)return FEISHU_DATA_TOOBIG;
    uint64_t used=0;
    for(unsigned i=0;i<SLOTS;++i)if(entries[i].kind==e->kind &&
       (entries[i].state==LIVE || entries[i].state==STAGING || pins[i]))used+=entries[i].stored;
    if(e->kind==FEISHU_DATA_METADATA)used=metadata_usage();
    uint8_t packed[FEISHU_BLOCK_BYTES+12];feishu_block_info_t info;uint32_t added=0;
    /* Preflight quota before any block is written; a FULL result does not
     * advance the writer or expose a prefix of an append span. */
    for(size_t at=0;at<size;at+=FEISHU_BLOCK_BYTES) {
        size_t n=size-at;if(n>FEISHU_BLOCK_BYTES)n=FEISHU_BLOCK_BYTES;
        if(!feishu_block_pack(data+at,n,compress,packed+12,FEISHU_BLOCK_BYTES,&info))return FEISHU_DATA_MISUSE;
        added+=info.stored_size;
    }
    if(quotas[e->kind] && used+added>quotas[e->kind]) {w->quota_limited=true;return FEISHU_DATA_FULL;}
    unsigned sequence=e->sequence;
    for(size_t at=0;at<size;at+=FEISHU_BLOCK_BYTES) {
        size_t n=size-at;if(n>FEISHU_BLOCK_BYTES)n=FEISHU_BLOCK_BYTES;
        if(!feishu_block_pack(data+at,n,compress,packed+12,FEISHU_BLOCK_BYTES,&info))return FEISHU_DATA_ERROR;
        memcpy(packed,"FBL1",4);u32(packed+4,info.crc32);packed[8]=(uint8_t)info.raw_size;
        packed[9]=(uint8_t)(info.raw_size>>8);packed[10]=info.codec;packed[11]=0;
        char name[40];block_name(name,e->id,sequence++);
        int rc=mapped(feishu_fdb_put(name,packed,info.stored_size+12));if(rc)return rc;
    }
    e->raw+=(uint32_t)size;e->stored+=added;e->sequence=sequence;
    w->raw=e->raw;w->stored=e->stored;w->sequence=e->sequence;return FEISHU_DATA_OK;
}
int feishu_data_append(feishu_data_t *s,feishu_data_writer_t *w,const uint8_t *data,size_t size,bool compress)
{return size>FEISHU_BLOCK_BYTES?FEISHU_DATA_MISUSE:feishu_data_append_span(s,w,data,size,compress);}
static int publish(entry_t *e,const char *key)
{
    char old[256],name[40];if(!key){int rc=key_read(e,old);if(rc)return rc;key=old;}
    entry_t *previous=NULL;int rc=matching(key,LIVE,&previous);if(rc)return rc;
    if(e->key_tag==UINT32_MAX)return FEISHU_DATA_FULL;
    key_name(name,e->id,e->key_tag+1);rc=mapped(feishu_fdb_put(name,key,strlen(key)+1));if(rc)return rc;
    ++e->key_tag;
    if(previous)previous->state=RETIRED;
    e->hash=hash(key);e->state=LIVE;e->touch=++clock_value;return FEISHU_DATA_OK;
}
int feishu_data_commit_pair(feishu_data_t *s,feishu_data_writer_t *first,const char *fk,
                           feishu_data_writer_t *second,const char *sk,int64_t release)
{
    CHECK_STORE(s);
    if(!valid(s) || (fk && !key_valid(fk)) || (sk && !key_valid(sk)) || release<0 ||
       !first || (second && first->id==second->id) || (fk && sk && !strcmp(fk,sk)) ||
       (release && (release==first->id || (second && release==second->id))))return FEISHU_DATA_MISUSE;
    entry_t *a=writer_entry(first),*b=second?writer_entry(second):NULL;
    if(!a || (second && !b))return FEISHU_DATA_MISUSE;
    /* Catalog publication is the sole visibility point for both objects and
     * the predecessor's protection. Roll back RAM on non-success. */
    entry_t *before=malloc(sizeof(entries));if(!before)return FEISHU_DATA_NOMEM;
    memcpy(before,entries,sizeof(entries));uint32_t old_clock=clock_value;
    int rc=publish(a,fk);if(!rc && b)rc=publish(b,sk);
    entry_t *old=release?by_id(release):NULL;if(!rc && old)old->protected_data=false;
    if(!rc)rc=save_catalog();
    if(rc){memcpy(entries,before,sizeof(entries));clock_value=old_clock;}
    else {pins[a-entries]=0;first->id=0;if(b){pins[b-entries]=0;second->id=0;}}
    free(before);return rc;
}
int feishu_data_commit_as(feishu_data_t *s,feishu_data_writer_t *w,const char *key)
{return feishu_data_commit_pair(s,w,key,NULL,NULL,0);}
int feishu_data_commit(feishu_data_t *s,feishu_data_writer_t *w) {return feishu_data_commit_as(s,w,NULL);}
int feishu_data_abort(feishu_data_t *s,feishu_data_writer_t *w)
{
    CHECK_STORE(s);
    if(!valid(s) || !w || w->id<=0)return FEISHU_DATA_MISUSE;
    entry_t *e=by_id(w->id);
    if(e && e->state==STAGING){memset(e,0,sizeof(*e));pins[e-entries]=0;}
    w->id=0;return FEISHU_DATA_OK;
}
int feishu_data_discard_pending(feishu_data_t *s)
{
    CHECK_STORE(s);
    if(!valid(s))return FEISHU_DATA_MISUSE;
    for(unsigned i=0;i<SLOTS;++i)if(entries[i].state==STAGING){memset(&entries[i],0,sizeof(entries[i]));pins[i]=0;}
    return FEISHU_DATA_OK;
}
int feishu_data_release_orphan(feishu_data_t *s,const int64_t *active,size_t count,int64_t *released)
{
    CHECK_STORE(s);
    if(!valid(s) || !released || count>64 || (count && !active))return FEISHU_DATA_MISUSE;
    *released=0;
    for(unsigned i=0;i<SLOTS;++i)if(entries[i].state==STAGING) {
        bool keep=false;for(size_t j=0;j<count;++j)if((uint64_t)active[j]==entries[i].id)keep=true;
        if(!keep){*released=(int64_t)entries[i].id;memset(&entries[i],0,sizeof(entries[i]));pins[i]=0;break;}
    }
    return FEISHU_DATA_OK;
}
static void object(entry_t *e,feishu_data_object_t *o)
{*o=(feishu_data_object_t){(int64_t)e->id,e->raw,e->stored,e->protected_data!=0};}
int feishu_data_find(feishu_data_t *s,const char *key,feishu_data_object_t *o)
{
    CHECK_STORE(s);
    if(!valid(s) || !key_valid(key) || !o)return FEISHU_DATA_MISUSE;
    entry_t *e=NULL;int rc=matching(key,LIVE,&e);if(rc)return rc;
    if(!e)return FEISHU_DATA_NOTFOUND;
    object(e,o);return FEISHU_DATA_OK;
}
int feishu_data_stat(feishu_data_t *s,int64_t id,const char *key,feishu_data_object_t *o)
{
    CHECK_STORE(s);
    if(!valid(s) || id<=0 || !key_valid(key) || !o)return FEISHU_DATA_MISUSE;
    entry_t *e=by_id(id);if(!e || (e->state!=LIVE && e->state!=RETIRED))return FEISHU_DATA_NOTFOUND;
    char actual[256];int rc=key_read(e,actual);if(rc)return rc;
    if(strcmp(key,actual))return FEISHU_DATA_NOTFOUND;
    object(e,o);return FEISHU_DATA_OK;
}
int feishu_data_read(feishu_data_t *s,int64_t id,unsigned sequence,uint8_t *data,size_t capacity,size_t *size)
{
    CHECK_STORE(s);
    if(!valid(s) || id<=0 || !data || !size)return FEISHU_DATA_MISUSE;
    *size=0;entry_t *e=by_id(id);if(!e || sequence>=e->sequence)return FEISHU_DATA_NOTFOUND;
    if(!sequence && e->sequence>=3) {
        char prefix[24],first[40],last[40];
        snprintf(prefix,sizeof(prefix),"b%016llx.",(unsigned long long)e->id);
        block_name(first,e->id,0);block_name(last,e->id,e->sequence-1);
        int prefetched=mapped(feishu_fdb_prefetch(prefix,first,last,e->sequence));
        if(prefetched)return prefetched;
    }
    char name[40];uint8_t packed[FEISHU_BLOCK_BYTES+12];size_t n=0;block_name(name,e->id,sequence);
    int rc=mapped(feishu_fdb_get(name,packed,sizeof(packed),&n));if(rc)return rc;
    if(n<=12 || memcmp(packed,"FBL1",4) || packed[11])return FEISHU_DATA_CORRUPT;
    feishu_block_info_t info={g32(packed+4),(uint16_t)(packed[8]|packed[9]<<8),(uint16_t)(n-12),packed[10]};
    if(!feishu_block_unpack(packed+12,n-12,&info,data,capacity))return FEISHU_DATA_CORRUPT;
    *size=info.raw_size;return FEISHU_DATA_OK;
}
int feishu_data_pin(feishu_data_t *s,int64_t id,bool pin)
{
    CHECK_STORE(s);
    if(!valid(s) || id<=0)return FEISHU_DATA_MISUSE;
    entry_t *e=by_id(id);if(!e || (e->state!=LIVE && e->state!=RETIRED))return FEISHU_DATA_NOTFOUND;
    pins[e-entries]=pin;e->touch=++clock_value;return FEISHU_DATA_OK;
}
static bool garbage(const char *key,size_t size,void *ctx)
{
    unsigned long long id=0,map_id=0;unsigned seq=0;int consumed=0;entry_t *e=NULL;bool unused=false;
    if(sscanf(key,"b%16llx.%8x%n",&id,&seq,&consumed)==2 && !key[consumed]) {
        e=by_id((int64_t)id);unused=!e || seq>=e->sequence;
    } else if(sscanf(key,"k%16llx.%8x%n",&id,&seq,&consumed)==2 && !key[consumed]) {
        e=by_id((int64_t)id);unused=!e || seq!=e->key_tag;
    }
    else if(sscanf(key,"m%16llx/%n",&id,&consumed)==1 && consumed>0)unused=!by_id((int64_t)id);
    else if(sscanf(key,"p%16llx.%8x%n",&id,&seq,&consumed)==2 && !key[consumed]) {
        e=by_id((int64_t)id);unused=!e || seq>=e->point_batches;
    } else if(sscanf(key,"v%16llx.%16llx%n",&id,&map_id,&consumed)==2 && !key[consumed]) {
        e=by_id((int64_t)id);unused=!e || e->map!=map_id;
    }
    (void)size;(void)ctx;return unused;
}
static bool map_referenced(uint64_t id)
{for(unsigned i=0;i<SLOTS;++i)if(entries[i].state && entries[i].map==id)return true;return false;}
static int reclaim(feishu_data_t *s,int kind,size_t *bytes)
{
    CHECK_STORE(s);
    if(!valid(s) || !bytes)return FEISHU_DATA_MISUSE;
    *bytes=0;int rc=mapped(feishu_fdb_collect_if(garbage,NULL,bytes));
    if(rc || *bytes)return rc;
    uint8_t omitted[16]={0};unsigned count=0;uint64_t selected=0;
    /* Build an omission mask; RAM entries stay intact until the new catalog
     * is durable. The existing catalog buffer is the only publication copy. */
    while(count<16 && selected<4096) {
        entry_t *victim=NULL;
        for(unsigned i=0;i<SLOTS;++i) {
            entry_t *e=&entries[i];if((omitted[i/8]&(1u<<(i%8))) || !e->state || e->state==STAGING || pins[i] ||
               (e->state==LIVE && e->protected_data) || map_referenced(e->id) ||
               (kind>=0 && e->kind!=kind && !(kind==FEISHU_DATA_METADATA && e->map)))continue;
            if(!victim || (e->state==RETIRED && victim->state!=RETIRED) ||
               (e->state==victim->state && e->touch<victim->touch))victim=e;
        }
        if(!victim)break;
        unsigned index=(unsigned)(victim-entries);omitted[index/8]|=(uint8_t)(1u<<(index%8));
        selected+=victim->stored;++count;
    }
    if(!count)return FEISHU_DATA_FULL;
    rc=save_catalog_masked(omitted);
    if(!rc)for(unsigned i=0;i<SLOTS;++i)if(omitted[i/8]&(1u<<(i%8)))memset(&entries[i],0,sizeof(entries[i]));
    return rc;
}
int feishu_data_reclaim(feishu_data_t *s,size_t *bytes) {return reclaim(s,-1,bytes);}
int feishu_data_reclaim_category(feishu_data_t *s,feishu_data_kind_t kind,size_t *bytes)
{return kind<0 || kind>=FEISHU_DATA_KINDS?FEISHU_DATA_MISUSE:reclaim(s,kind,bytes);}

static uint64_t metadata_usage(void)
{
    uint64_t used=0;
    for(unsigned i=0;i<SLOTS;++i) {
        entry_t *e=&entries[i];
        if(e->kind==FEISHU_DATA_METADATA && (e->state==LIVE || e->state==STAGING ||
           pins[i] || (e->state && map_referenced(e->id))))used+=e->stored;
    }
    return used;
}
static void mention_name(char *out,uint64_t id,const char *key)
{snprintf(out,64,"m%016llx/%s",(unsigned long long)id,key);}
static void point_name(char *out,uint64_t id,uint32_t batch)
{snprintf(out,40,"p%016llx.%08x",(unsigned long long)id,(unsigned)batch);}
static void view_name(char *out,uint64_t id,uint64_t map)
{snprintf(out,48,"v%016llx.%016llx",(unsigned long long)id,(unsigned long long)map);}
static bool map_writer(const feishu_data_writer_t *w,entry_t **e)
{
    *e=writer_entry(w);
    return *e && (*e)->kind==FEISHU_DATA_METADATA && !(*e)->protected_data && !(*e)->raw && !(*e)->sequence;
}
int feishu_data_mention_get(feishu_data_t *s,int64_t map,const char *key,void *value,size_t capacity,size_t *size)
{
    CHECK_STORE(s);
    if(map<=0 || !key || strlen(key)<2 || strlen(key)>31 || !size || (!value && capacity))return FEISHU_DATA_MISUSE;
    *size=0;
    entry_t *e=by_id(map);if(!e)return FEISHU_DATA_NOTFOUND;
    char name[64];uint8_t bytes[148];size_t n=0;mention_name(name,(uint64_t)map,key);
    int rc=mapped(feishu_fdb_get(name,bytes,sizeof(bytes),&n));if(rc)return rc;
    if(n<20 || memcmp(bytes,"FMN1",4) || g64(bytes+4)!=(uint64_t)map || bytes[12]>1 ||
       bytes[13]>128 || n!=20u+bytes[13] || (bytes[12] && bytes[13]) ||
       g32(bytes+n-4)!=feishu_block_crc32(bytes,n-4))return FEISHU_DATA_CORRUPT;
    if(bytes[12])return FEISHU_DATA_DONE;
    if(!bytes[13])return FEISHU_DATA_CORRUPT;
    if(capacity<bytes[13])return FEISHU_DATA_TOOBIG;
    memcpy(value,bytes+16,bytes[13]);*size=bytes[13];return FEISHU_DATA_OK;
}
int feishu_data_mention_put(feishu_data_t *s,feishu_data_writer_t *w,const char *key,const char *value)
{
    CHECK_STORE(s);
    if(!key || strlen(key)<2 || strlen(key)>31 || (value && (!*value || strlen(value)>128)))return FEISHU_DATA_MISUSE;
    entry_t *e=NULL;if(!map_writer(w,&e))return FEISHU_DATA_MISUSE;
    w->quota_limited=false;
    uint8_t previous[128];size_t size=0,n=value?strlen(value):0;
    int rc=feishu_data_mention_get(s,w->id,key,previous,sizeof(previous),&size);
    if(rc==FEISHU_DATA_OK || rc==FEISHU_DATA_DONE) {
        bool same=value ? rc==FEISHU_DATA_OK && n==size && !memcmp(value,previous,n):rc==FEISHU_DATA_DONE;
        return same?FEISHU_DATA_OK:FEISHU_DATA_CONSTRAINT;
    }
    if(rc!=FEISHU_DATA_NOTFOUND)return rc;
    uint32_t charge=(uint32_t)(14+strlen(key)+n+(e->stored?0:40));
    if(metadata_usage()+charge>quotas[FEISHU_DATA_METADATA]) {w->quota_limited=true;return FEISHU_DATA_FULL;}
    uint8_t bytes[148]={0};char name[64];mention_name(name,e->id,key);
    memcpy(bytes,"FMN1",4);u64(bytes+4,e->id);bytes[12]=value?0:1;bytes[13]=(uint8_t)n;
    if(n)memcpy(bytes+16,value,n);
    u32(bytes+16+n,feishu_block_crc32(bytes,16+n));
    rc=mapped(feishu_fdb_put(name,bytes,n+20));
    if(!rc){e->stored+=charge;w->stored=e->stored;}
    return rc;
}
typedef struct {feishu_data_text_point_t points[8];unsigned count;} point_batch_t;
static int batch_read(entry_t *body,uint64_t map,uint32_t number,point_batch_t *batch)
{
    char name[40];uint8_t bytes[156];size_t n=0;point_name(name,body->id,number);
    int rc=mapped(feishu_fdb_get(name,bytes,sizeof(bytes),&n));
    if(rc)return rc==FEISHU_DATA_NOTFOUND?FEISHU_DATA_CORRUPT:rc;
    if(n<44 || memcmp(bytes,"FPB1",4) || g64(bytes+4)!=body->id || g64(bytes+12)!=map ||
       !g32(bytes+20) || g32(bytes+20)>8 || n!=28+16*g32(bytes+20) ||
       g32(bytes+n-4)!=feishu_block_crc32(bytes,n-4))return FEISHU_DATA_CORRUPT;
    batch->count=g32(bytes+20);
    for(unsigned i=0;i<batch->count;++i) {
        batch->points[i]=(feishu_data_text_point_t){g64(bytes+24+i*16),g64(bytes+32+i*16)};
        if(!batch->points[i].raw || !batch->points[i].logical || batch->points[i].raw>body->raw ||
           batch->points[i].logical>INT64_MAX || (i &&
           (batch->points[i].raw<=batch->points[i-1].raw || batch->points[i].logical<=batch->points[i-1].logical)))return FEISHU_DATA_CORRUPT;
    }
    return FEISHU_DATA_OK;
}
int feishu_data_text_index(feishu_data_t *s,const feishu_data_writer_t *body,feishu_data_writer_t *map,
                           const feishu_data_text_point_t *points,size_t count)
{
    CHECK_STORE(s);
    if(!points || !count || count>8 || !body || !map || body->id==map->id)return FEISHU_DATA_MISUSE;
    entry_t *b=writer_entry(body),*m=NULL;
    if(!b || b->kind!=FEISHU_DATA_TEXT || b->protected_data || !map_writer(map,&m) ||
       (b->map && b->map!=m->id))return FEISHU_DATA_MISUSE;
    for(size_t i=0;i<count;++i)if(!points[i].raw || !points[i].logical || points[i].raw>b->raw ||
       points[i].logical>INT64_MAX || (i && (points[i].raw<=points[i-1].raw || points[i].logical<=points[i-1].logical)))return FEISHU_DATA_MISUSE;
    if(b->point_batches) {
        point_batch_t last;int rc=batch_read(b,m->id,b->point_batches-1,&last);if(rc)return rc;
        if(points[0].raw<=last.points[last.count-1].raw || points[0].logical<=last.points[last.count-1].logical)return FEISHU_DATA_CONSTRAINT;
    }
    map->quota_limited=false;uint32_t charge=(uint32_t)(count*28+(m->stored?0:40));
    if(metadata_usage()+charge>quotas[FEISHU_DATA_METADATA]) {map->quota_limited=true;return FEISHU_DATA_FULL;}
    uint8_t bytes[156]={0};char name[40];point_name(name,b->id,b->point_batches);
    memcpy(bytes,"FPB1",4);u64(bytes+4,b->id);u64(bytes+12,m->id);u32(bytes+20,(uint32_t)count);
    for(unsigned i=0;i<count;++i){u64(bytes+24+16*i,points[i].raw);u64(bytes+32+16*i,points[i].logical);}
    size_t n=28+16*count;u32(bytes+n-4,feishu_block_crc32(bytes,n-4));
    int rc=mapped(feishu_fdb_put(name,bytes,n));
    if(!rc){++b->point_batches;b->map=m->id;m->stored+=charge;map->stored=m->stored;}
    return rc;
}
int feishu_data_text_index_clear(feishu_data_t *s,int64_t id)
{
    CHECK_STORE(s);
    entry_t *b=by_id(id);if(!b || b->state!=STAGING)return FEISHU_DATA_MISUSE;
    b->point_batches=0;b->map=0;return FEISHU_DATA_OK;
}
int feishu_data_text_info(feishu_data_t *s,int64_t id,uint64_t raw,feishu_data_text_view_t *view)
{
    CHECK_STORE(s);
    if(id<=0 || !view || raw>INT64_MAX)return FEISHU_DATA_MISUSE;
    entry_t *b=by_id(id);
    if(!b || b->state==STAGING || !b->map)return FEISHU_DATA_NOTFOUND;
    entry_t *m=by_id((int64_t)b->map);
    if(!m || b->raw!=raw || m->kind!=FEISHU_DATA_METADATA)return FEISHU_DATA_CORRUPT;
    char name[48];uint8_t bytes[48];size_t n=0;view_name(name,b->id,b->map);
    int rc=mapped(feishu_fdb_get(name,bytes,sizeof(bytes),&n));
    if(rc)return rc==FEISHU_DATA_NOTFOUND?FEISHU_DATA_CORRUPT:rc;
    if(n!=44 || memcmp(bytes,"FVW1",4) || g64(bytes+4)!=b->id || g64(bytes+12)!=b->map ||
       g64(bytes+20)!=raw || g64(bytes+28)>INT64_MAX || g32(bytes+36)!=m->stored ||
       g32(bytes+40)!=feishu_block_crc32(bytes,40))return FEISHU_DATA_CORRUPT;
    *view=(feishu_data_text_view_t){(int64_t)b->map,raw,g64(bytes+28),m->stored};return FEISHU_DATA_OK;
}
int feishu_data_text_seek(feishu_data_t *s,int64_t id,const feishu_data_text_view_t *view,
                          uint64_t offset,feishu_data_text_point_t *point)
{
    CHECK_STORE(s);
    if(id<=0 || !view || !point || view->map<=0 || offset>view->logical)return FEISHU_DATA_MISUSE;
    feishu_data_text_view_t actual;int rc=feishu_data_text_info(s,id,view->raw,&actual);if(rc)return rc;
    if(actual.map!=view->map || actual.logical!=view->logical || actual.metadata!=view->metadata)return FEISHU_DATA_CORRUPT;
    entry_t *b=by_id(id);uint32_t low=0,high_batch=b->point_batches;
    point_batch_t batch;
    /* Binary-search the batch, then inspect at most eight points and its
     * immediate successor. RAM remains independent of message length. */
    while(low<high_batch) {
        uint32_t middle=low+(high_batch-low)/2;rc=batch_read(b,b->map,middle,&batch);if(rc)return rc;
        if(batch.points[0].logical<=offset)low=middle+1;else high_batch=middle;
    }
    *point=(feishu_data_text_point_t){0,0};
    feishu_data_text_point_t end={view->raw,view->logical};
    if(low) {
        rc=batch_read(b,b->map,low-1,&batch);if(rc)return rc;
        for(unsigned i=0;i<batch.count;++i) {
            if(batch.points[i].logical<=offset)*point=batch.points[i];
            else {end=batch.points[i];break;}
        }
    }
    if(end.logical==view->logical && low<b->point_batches) {
        rc=batch_read(b,b->map,low,&batch);if(rc)return rc;end=batch.points[0];
    }
    if(point->logical>offset || point->raw>view->raw || end.logical>view->logical ||
       end.raw>view->raw || end.raw<point->raw || end.logical<point->logical ||
       end.raw-point->raw>1024+31 || end.logical-point->logical>1024+129)return FEISHU_DATA_CORRUPT;
    return FEISHU_DATA_OK;
}
int feishu_data_text_publish(feishu_data_t *s,feishu_data_writer_t *body,feishu_data_writer_t *map,
                             uint64_t logical,const char *key)
{
    CHECK_STORE(s);
    if(!key_valid(key) || !body || !map || body->id==map->id || logical>INT64_MAX)return FEISHU_DATA_MISUSE;
    entry_t *b=writer_entry(body),*m=NULL;
    if(!b || b->kind!=FEISHU_DATA_TEXT || b->protected_data || !map_writer(map,&m) || m->stored<40 ||
       (b->map && b->map!=m->id))return FEISHU_DATA_MISUSE;
    uint8_t bytes[44]={0};char name[48];view_name(name,b->id,m->id);
    memcpy(bytes,"FVW1",4);u64(bytes+4,b->id);u64(bytes+12,m->id);u64(bytes+20,b->raw);
    u64(bytes+28,logical);u32(bytes+36,m->stored);u32(bytes+40,feishu_block_crc32(bytes,40));
    int rc=mapped(feishu_fdb_put(name,bytes,sizeof(bytes)));if(rc)return rc;
    entry_t *before=malloc(sizeof(entries));if(!before)return FEISHU_DATA_NOMEM;
    memcpy(before,entries,sizeof(entries));uint32_t old_clock=clock_value;
    rc=publish(b,key);
    if(!rc){b->map=m->id;m->state=RETIRED;m->touch=b->touch;rc=save_catalog();}
    if(rc){memcpy(entries,before,sizeof(entries));clock_value=old_clock;}
    else {pins[b-entries]=pins[m-entries]=0;body->id=map->id=0;}
    free(before);return rc;
}

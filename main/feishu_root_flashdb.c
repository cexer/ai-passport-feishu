/* Raw implementation of feishu_root.h. The pool owner opens and serializes
 * the shared KV engine independently from the object catalog. */
#include "feishu_root_flashdb.h"
#include "feishu_fdb.h"
#include "feishu_block_codec.h"
#include <stdbool.h>
#include <string.h>
typedef struct {uint64_t generation;uint32_t length,crc;int status;} record_t;
static struct {bool active;unsigned key,slot;uint64_t generation;} writing;
static struct {uint64_t epoch;unsigned key,slot;record_t records[2];bool valid;} proofs[4];
static unsigned next_proof;
#ifdef FEISHU_ASSISTANT_AUTOTEST
void feishu_root_flashdb_probe_cold(void) {memset(proofs,0,sizeof(proofs));next_proof=0;}
#endif
static bool garbage_clean;
static uint64_t garbage_root_epoch;
static uint32_t get32(const uint8_t *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint64_t get64(const uint8_t *p) {return get32(p)|((uint64_t)get32(p+4)<<32);}
static void put32(uint8_t *p,uint32_t n) {for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(n>>(8*i));}
static void put64(uint8_t *p,uint64_t n) {put32(p,(uint32_t)n);put32(p+4,(uint32_t)(n>>32));}
static uint32_t update(uint32_t crc,const uint8_t *p,size_t n)
{while(n--){crc^=*p++;for(unsigned i=0;i<8;++i)crc=(crc>>1)^((0u-(crc&1u))&0xedb88320u);}return crc;}
static int mapped(int rc)
{
    if(rc==FEISHU_FDB_OK)return FEISHU_ROOT_OK;
    if(rc==FEISHU_FDB_NOTFOUND)return FEISHU_ROOT_MISSING;
    if(rc==FEISHU_FDB_FULL)return FEISHU_ROOT_FULL;
    if(rc==FEISHU_FDB_TOOBIG)return FEISHU_ROOT_CORRUPT;
    return FEISHU_ROOT_IO;
}
static bool valid(const feishu_root_t *root,unsigned key)
{return root && root->directory && *root->directory && !root->write && !root->sync && key<=255;}
static void manifest_name(char *out,unsigned key,unsigned slot)
{snprintf(out,24,"r%02x%c",key,slot?'b':'a');}
static void chunk_name(char *out,unsigned key,unsigned slot,uint64_t gen,unsigned seq)
{snprintf(out,48,"r%02x%c/%016llx/%02x",key,slot?'b':'a',(unsigned long long)gen,seq);}
static record_t header(unsigned key,unsigned slot)
{
    uint8_t bytes[32];char name[24];size_t n=0;manifest_name(name,key,slot);
    record_t r={.status=FEISHU_ROOT_CORRUPT};
    int rc=mapped(feishu_fdb_get(name,bytes,sizeof(bytes),&n));if(rc){r.status=rc;return r;}
    if(n!=32 || memcmp(bytes,"FRC1",4) || get32(bytes+4)!=key || !get64(bytes+8) ||
       get32(bytes+16)>FEISHU_ROOT_MAX || get32(bytes+28) ||
       get32(bytes+24)!=feishu_block_crc32(bytes,24))return r;
    r.generation=get64(bytes+8);r.length=get32(bytes+16);r.crc=get32(bytes+20);r.status=FEISHU_ROOT_OK;return r;
}
static int chunk(unsigned key,unsigned slot,record_t r,unsigned seq,uint8_t *bytes,size_t *n)
{
    char name[48];chunk_name(name,key,slot,r.generation,seq);
    int rc=mapped(feishu_fdb_get(name,bytes,512,n));if(rc)return rc==FEISHU_ROOT_MISSING?FEISHU_ROOT_CORRUPT:rc;
    size_t expected=r.length-seq*512u;if(expected>512)expected=512;
    return *n==expected?FEISHU_ROOT_OK:FEISHU_ROOT_CORRUPT;
}
static record_t inspect(unsigned key,unsigned slot)
{
    record_t r=header(key,slot);if(r.status)return r;
    unsigned chunks=(r.length+511u)/512u;
    if(chunks>=3){
        char prefix[24],first[48],last[48];
        snprintf(prefix,sizeof(prefix),"r%02x%c/%016llx/",key,slot?'b':'a',(unsigned long long)r.generation);
        chunk_name(first,key,slot,r.generation,0);chunk_name(last,key,slot,r.generation,chunks-1);
        r.status=mapped(feishu_fdb_prefetch(prefix,first,last,chunks));if(r.status)return r;
    }
    uint32_t crc=UINT32_MAX;uint8_t bytes[512];
    for(unsigned seq=0;seq*512u<r.length;++seq) {
        size_t n=0;r.status=chunk(key,slot,r,seq,bytes,&n);if(r.status)break;
        crc=update(crc,bytes,n);
    }
    memset(bytes,0,sizeof(bytes));
    if(!r.status && (crc^UINT32_MAX)!=r.crc)r.status=FEISHU_ROOT_CORRUPT;
    return r;
}
static int choose(unsigned key,record_t records[2],unsigned *slot)
{
    if(feishu_fdb_status()!=FEISHU_FDB_OK)return FEISHU_ROOT_IO;
    uint64_t epoch=feishu_fdb_revision();
    for(unsigned i=0;i<4;++i)if(proofs[i].valid && proofs[i].epoch==epoch && proofs[i].key==key) {
        memcpy(records,proofs[i].records,sizeof(proofs[i].records));*slot=proofs[i].slot;return FEISHU_ROOT_OK;
    }
    records[0]=inspect(key,0);records[1]=inspect(key,1);
    if(records[0].status==FEISHU_ROOT_IO || records[1].status==FEISHU_ROOT_IO)return FEISHU_ROOT_IO;
    bool a=records[0].status==FEISHU_ROOT_OK,b=records[1].status==FEISHU_ROOT_OK;
    if(a || b) {
        if(a && b && records[0].generation==records[1].generation &&
           (records[0].length!=records[1].length || records[0].crc!=records[1].crc))return FEISHU_ROOT_CORRUPT;
        *slot=!a || (b && records[1].generation>records[0].generation)?1:0;
        unsigned i=next_proof++%4;proofs[i].valid=true;proofs[i].key=key;proofs[i].slot=*slot;
        proofs[i].epoch=feishu_fdb_revision();memcpy(proofs[i].records,records,sizeof(proofs[i].records));
        return FEISHU_ROOT_OK;
    }
    return records[0].status==FEISHU_ROOT_MISSING && records[1].status==FEISHU_ROOT_MISSING?
           FEISHU_ROOT_MISSING:FEISHU_ROOT_CORRUPT;
}
int feishu_root_read(const feishu_root_t *root,unsigned key,size_t offset,void *data,size_t capacity,size_t *total)
{
    if(!valid(root,key) || !total || (!data && capacity))return FEISHU_ROOT_INVALID;
    *total=0;record_t records[2];unsigned slot=0;int rc=choose(key,records,&slot);if(rc)return rc;
    *total=records[slot].length;
    if(offset>*total || capacity>*total-offset)return FEISHU_ROOT_INVALID;
    uint8_t bytes[512];size_t copied=0;
    while(copied<capacity) {
        size_t n=0;unsigned seq=(unsigned)((offset+copied)/512);
        rc=chunk(key,slot,records[slot],seq,bytes,&n);if(rc)break;
        size_t start=(offset+copied)%512,count=n-start;
        if(count>capacity-copied)count=capacity-copied;
        memcpy((uint8_t *)data+copied,bytes+start,count);copied+=count;
    }
    memset(bytes,0,sizeof(bytes));if(rc && data)memset(data,0,capacity);return rc;
}
static void payload(const feishu_root_part_t *parts,size_t count,size_t offset,uint8_t *out,size_t n)
{
    memset(out,0,n);size_t copied=0;
    for(size_t i=0;i<count && copied<n;++i) {
        if(offset>=parts[i].size){offset-=parts[i].size;continue;}
        size_t m=parts[i].size-offset;if(m>n-copied)m=n-copied;
        if(parts[i].data)memcpy(out+copied,(const uint8_t *)parts[i].data+offset,m);
        copied+=m;offset=0;
    }
}
static bool equal(unsigned key,unsigned slot,record_t r,const feishu_root_part_t *parts,size_t count)
{
    uint8_t expected[512],actual[512];bool same=true;
    for(unsigned seq=0;seq*512u<r.length;++seq) {
        size_t n=0;if(chunk(key,slot,r,seq,actual,&n)){same=false;break;}
        payload(parts,count,seq*512u,expected,n);
        if(memcmp(expected,actual,n)){same=false;break;}
    }
    memset(expected,0,sizeof(expected));memset(actual,0,sizeof(actual));return same;
}
static int put(const char *name,const void *bytes,size_t size)
{
    for(unsigned attempt=0;attempt<64;++attempt) {
        int rc=mapped(feishu_fdb_put(name,bytes,size));if(rc!=FEISHU_ROOT_FULL)return rc;
        size_t released=0;rc=feishu_root_flashdb_collect(&released);
        if(rc!=FEISHU_ROOT_OK)return rc==FEISHU_ROOT_MISSING?FEISHU_ROOT_FULL:rc;
    }
    return FEISHU_ROOT_FULL;
}
static int write_copy_impl(unsigned key,unsigned slot,record_t r,const feishu_root_part_t *parts,size_t count)
{
    uint8_t bytes[512];char name[48];int rc=FEISHU_ROOT_OK;
    for(unsigned seq=0;seq*512u<r.length;++seq) {
        size_t n=r.length-seq*512u;if(n>512)n=512;
        payload(parts,count,seq*512u,bytes,n);chunk_name(name,key,slot,r.generation,seq);
        rc=put(name,bytes,n);if(rc)break;
    }
    memset(bytes,0,sizeof(bytes));if(rc)return rc;
    uint8_t h[32]={0};memcpy(h,"FRC1",4);put32(h+4,key);put64(h+8,r.generation);
    put32(h+16,r.length);put32(h+20,r.crc);put32(h+24,feishu_block_crc32(h,24));
    manifest_name(name,key,slot);rc=put(name,h,sizeof(h));if(rc)return rc;
    record_t check=inspect(key,slot);
    return check.status || check.generation!=r.generation || check.length!=r.length || check.crc!=r.crc?
           FEISHU_ROOT_IO:FEISHU_ROOT_OK;
}
static int write_copy(unsigned key,unsigned slot,record_t r,const feishu_root_part_t *parts,size_t count)
{
    writing.active=true;writing.key=key;writing.slot=slot;writing.generation=r.generation;
    int rc=write_copy_impl(key,slot,r,parts,count);
    memset(&writing,0,sizeof(writing));return rc;
}
int feishu_root_write(const feishu_root_t *root,unsigned key,const feishu_root_part_t *parts,size_t count)
{
    if(!valid(root,key) || (!parts && count))return FEISHU_ROOT_INVALID;
    size_t length=0;for(size_t i=0;i<count;++i){if(parts[i].size>FEISHU_ROOT_MAX-length)return FEISHU_ROOT_INVALID;length+=parts[i].size;}
    uint8_t bytes[512];uint32_t crc=UINT32_MAX;
    for(size_t at=0;at<length;at+=512) {
        size_t n=length-at;if(n>512)n=512;payload(parts,count,at,bytes,n);crc=update(crc,bytes,n);
    }
    memset(bytes,0,sizeof(bytes));crc^=UINT32_MAX;
    record_t records[2];unsigned current=0;int rc=choose(key,records,&current);
    if(rc!=FEISHU_ROOT_OK && rc!=FEISHU_ROOT_MISSING)return rc;
    bool exists=rc==FEISHU_ROOT_OK;
    uint64_t generation=exists?records[current].generation:0;
    bool same=exists && records[current].length==length && records[current].crc==crc && equal(key,current,records[current],parts,count);
    if(feishu_fdb_status()!=FEISHU_FDB_OK)return FEISHU_ROOT_IO;
    if(same && records[current^1u].status==FEISHU_ROOT_OK && records[current^1u].generation==generation)return FEISHU_ROOT_OK;
    if(!same){if(generation==UINT64_MAX)return FEISHU_ROOT_INVALID;++generation;}
    record_t next={generation,(uint32_t)length,crc,FEISHU_ROOT_OK};
    unsigned target=exists?current^1u:0;
    rc=write_copy(key,target,next,parts,count);if(rc)return rc;
    /* A fully verified new copy is durable even if redundancy repair fails. */
    (void)write_copy(key,target^1u,next,parts,count);
    return FEISHU_ROOT_OK;
}
typedef struct {char name[64];size_t size;int error;} garbage_t;
static bool obsolete(const char *name,size_t size,void *context)
{
    unsigned key=0,seq=0;char slot_char=0;unsigned long long generation=0;int consumed=0;
    if(sscanf(name,"r%2x%c/%16llx/%2x%n",&key,&slot_char,&generation,&seq,&consumed)!=4 ||
       !consumed || name[consumed] || (slot_char!='a' && slot_char!='b'))return false;
    if(writing.active && writing.key==key && writing.slot==(unsigned)(slot_char=='b') &&
       writing.generation==generation)return false;
    garbage_t *g=context;record_t r=header(key,slot_char=='b');
    if(r.status==FEISHU_ROOT_IO){g->error=r.status;return true;}
    /* An unreadable or missing manifest cannot authorize destructive cleanup.
     * Once a valid replacement exists, its superseded chunks can be removed. */
    if(r.status!=FEISHU_ROOT_OK)return false;
    if(r.generation!=generation || seq*512u>=r.length) {
        snprintf(g->name,sizeof(g->name),"%s",name);g->size=size;return true;
    }
    return false;
}
int feishu_root_flashdb_collect(size_t *bytes)
{
    if(!bytes)return FEISHU_ROOT_INVALID;
    *bytes=0;
    if(feishu_fdb_status()!=FEISHU_FDB_OK)return FEISHU_ROOT_IO;
    uint64_t epoch=feishu_fdb_root_revision();
    if(!writing.active && garbage_clean && garbage_root_epoch==epoch)return FEISHU_ROOT_MISSING;
    garbage_t g={0};int rc=mapped(feishu_fdb_visit(obsolete,&g));if(rc)return rc;
    if(g.error)return g.error;
    if(!g.name[0]) {
        if(!writing.active){garbage_clean=true;garbage_root_epoch=epoch;}
        return FEISHU_ROOT_MISSING;
    }
    rc=mapped(feishu_fdb_delete(g.name));if(!rc)*bytes=g.size;return rc;
}

#include "feishu_root.h"
#include <stdbool.h>
#include <errno.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

typedef struct { uint64_t generation; uint32_t length,crc; int status; } record_t;
static uint32_t crc_update(uint32_t crc,const uint8_t *data,size_t size)
{
    while(size--) {
        crc^=*data++;
        for(unsigned i=0;i<8;++i)crc=(crc>>1)^((0u-(crc&1u))&0xedb88320u);
    }
    return crc;
}
static uint32_t get32(const uint8_t *p)
{ return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static void put32(uint8_t *p,uint32_t value)
{ for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(value>>(8*i)); }
static bool path_for(const feishu_root_t *r,unsigned key,unsigned slot,char path[128])
{
    if(!r || !r->directory || key>255)return false;
    int n=snprintf(path,128,"%s/cfg%02x%c",r->directory,key,slot ? 'b':'a');
    return n>0 && n<128;
}
static record_t inspect(const feishu_root_t *root,unsigned key,unsigned slot)
{
    record_t record={.status=FEISHU_ROOT_INVALID};
    char path[128];uint8_t header[32],block[512];
    if(!path_for(root,key,slot,path))return record;
    errno=0;FILE *f=fopen(path,"rb");
    if(!f) {record.status=errno==ENOENT ? FEISHU_ROOT_MISSING:FEISHU_ROOT_IO;return record;}
    record.status=FEISHU_ROOT_CORRUPT;
    if(fread(header,1,32,f)!=32)goto done;
    if(memcmp(header,"FRC1",4) || get32(header+4)!=key || get32(header+28) ||
       (crc_update(UINT32_MAX,header,24)^UINT32_MAX)!=get32(header+24))goto done;
    record.generation=(uint64_t)get32(header+8)|((uint64_t)get32(header+12)<<32);
    record.length=get32(header+16);record.crc=get32(header+20);
    if(!record.generation || record.length>FEISHU_ROOT_MAX)goto done;
    uint32_t crc=UINT32_MAX;
    for(size_t offset=0;offset<record.length;) {
        size_t size=record.length-offset;if(size>sizeof(block))size=sizeof(block);
        if(fread(block,1,size,f)!=size)goto done;
        crc=crc_update(crc,block,size);offset+=size;
    }
    if((crc^UINT32_MAX)!=record.crc || fread(block,1,1,f)!=0 || ferror(f))goto done;
    record.status=FEISHU_ROOT_OK;
done:
    if(ferror(f))record.status=FEISHU_ROOT_IO;
    if(fclose(f))record.status=FEISHU_ROOT_IO;
    memset(block,0,sizeof(block));return record;
}
static int choose(const feishu_root_t *root,unsigned key,record_t records[2],unsigned *slot)
{
    records[0]=inspect(root,key,0);records[1]=inspect(root,key,1);
    bool a=records[0].status==FEISHU_ROOT_OK,b=records[1].status==FEISHU_ROOT_OK;
    if(a || b) {
        if(a && b && records[0].generation==records[1].generation &&
           (records[0].crc!=records[1].crc || records[0].length!=records[1].length))return FEISHU_ROOT_CORRUPT;
        *slot=!a || (b && records[1].generation>records[0].generation) ? 1:0;
        return FEISHU_ROOT_OK;
    }
    if(records[0].status==FEISHU_ROOT_MISSING && records[1].status==FEISHU_ROOT_MISSING)return FEISHU_ROOT_MISSING;
    if(records[0].status==FEISHU_ROOT_IO || records[1].status==FEISHU_ROOT_IO)return FEISHU_ROOT_IO;
    return records[0].status==FEISHU_ROOT_INVALID ? FEISHU_ROOT_INVALID:FEISHU_ROOT_CORRUPT;
}
int feishu_root_read(const feishu_root_t *root,unsigned key,size_t offset,
                     void *data,size_t capacity,size_t *total)
{
    if(!total || (!data && capacity))return FEISHU_ROOT_INVALID;
    *total=0;record_t records[2];unsigned slot;
    int rc=choose(root,key,records,&slot);if(rc)return rc;
    *total=records[slot].length;
    if(offset>*total || capacity>*total-offset)return FEISHU_ROOT_INVALID;
    if(!capacity)return FEISHU_ROOT_OK;
    char path[128];if(!path_for(root,key,slot,path))return FEISHU_ROOT_INVALID;
    FILE *f=fopen(path,"rb");if(!f)return FEISHU_ROOT_IO;
    rc=fseek(f,(long)(32+offset),SEEK_SET) || fread(data,1,capacity,f)!=capacity ? FEISHU_ROOT_IO:FEISHU_ROOT_OK;
    if(fclose(f))rc=FEISHU_ROOT_IO;
    if(rc)memset(data,0,capacity);
    return rc;
}
static int disk_error(void)
{ return errno==ENOSPC ? FEISHU_ROOT_FULL:FEISHU_ROOT_IO; }
static int default_sync(FILE *f)
{
    if(fflush(f))return -1;
#ifdef _WIN32
    return _commit(_fileno(f));
#else
    return fsync(fileno(f));
#endif
}
static size_t default_write(const void *p,size_t n,FILE *f) { return fwrite(p,1,n,f); }
static int write_copy(const feishu_root_t *r,unsigned key,unsigned slot,const uint8_t header[32],
                       const feishu_root_part_t *parts,size_t count)
{
    char path[128];uint8_t zero[512]={0};
    if(!path_for(r,key,slot,path))return FEISHU_ROOT_INVALID;
    errno=0;FILE *f=fopen(path,"wb");if(!f)return disk_error();
    /* Do not hide a failed fsync behind a successful stdio buffer write. */
    setvbuf(f,NULL,_IONBF,0);
    size_t (*write_bytes)(const void *,size_t,FILE *)=r->write ? r->write:default_write;
    int rc=write_bytes(header,32,f)==32 ? FEISHU_ROOT_OK:disk_error();
    for(size_t i=0;rc==FEISHU_ROOT_OK && i<count;++i) {
        for(size_t offset=0;offset<parts[i].size;) {
            size_t n=parts[i].size-offset;if(n>sizeof(zero))n=sizeof(zero);
            const void *p=parts[i].data ? (const uint8_t *)parts[i].data+offset:zero;
            if(write_bytes(p,n,f)!=n) {rc=disk_error();break;}
            offset+=n;
        }
    }
    if(rc==FEISHU_ROOT_OK && (r->sync ? r->sync(f):default_sync(f)))rc=disk_error();
    if(fclose(f) && rc==FEISHU_ROOT_OK)rc=disk_error();
    if(rc==FEISHU_ROOT_OK) {
        record_t check=inspect(r,key,slot);
        if(check.status || check.generation!=((uint64_t)get32(header+8)|((uint64_t)get32(header+12)<<32)) ||
           check.crc!=get32(header+20) || check.length!=get32(header+16))rc=FEISHU_ROOT_IO;
    }
    return rc;
}
static bool equal_payload(const feishu_root_t *r,unsigned key,unsigned slot,
                           const feishu_root_part_t *parts,size_t count)
{
    char path[128];uint8_t block[512];
    if(!path_for(r,key,slot,path))return false;
    FILE *f=fopen(path,"rb");if(!f)return false;
    bool same=fseek(f,32,SEEK_SET)==0;
    for(size_t i=0;same && i<count;++i)for(size_t offset=0;offset<parts[i].size;) {
        size_t n=parts[i].size-offset;if(n>sizeof(block))n=sizeof(block);
        if(fread(block,1,n,f)!=n) {same=false;break;}
        if(parts[i].data)same=memcmp(block,(const uint8_t *)parts[i].data+offset,n)==0;
        else for(size_t j=0;j<n;++j)if(block[j]) {same=false;break;}
        if(!same)break;
        offset+=n;
    }
    if(fclose(f))same=false;
    memset(block,0,sizeof(block));return same;
}
int feishu_root_write(const feishu_root_t *root,unsigned key,const feishu_root_part_t *parts,size_t count)
{
    if(!root || (!parts && count) || key>255)return FEISHU_ROOT_INVALID;
    size_t length=0;uint32_t crc=UINT32_MAX;uint8_t zero[512]={0};
    for(size_t i=0;i<count;++i) {
        if(parts[i].size>FEISHU_ROOT_MAX-length)return FEISHU_ROOT_INVALID;
        length+=parts[i].size;
        for(size_t offset=0;offset<parts[i].size;) {
            size_t n=parts[i].size-offset;if(n>sizeof(zero))n=sizeof(zero);
            crc=crc_update(crc,parts[i].data ? (const uint8_t *)parts[i].data+offset:zero,n);offset+=n;
        }
    }
    record_t records[2];unsigned current=0;
    int rc=choose(root,key,records,&current);
    if(rc!=FEISHU_ROOT_OK && rc!=FEISHU_ROOT_MISSING)return rc;
    uint64_t generation=rc==FEISHU_ROOT_OK ? records[current].generation:0;
    if(generation==UINT64_MAX)return FEISHU_ROOT_INVALID;
    /* Avoid wearing Flash when a caller re-saves identical data. */
    bool unchanged=generation && records[current].length==length && records[current].crc==(crc^UINT32_MAX) &&
        equal_payload(root,key,current,parts,count);
    if(unchanged && records[current^1u].status==FEISHU_ROOT_OK &&
       records[current^1u].generation==generation)return FEISHU_ROOT_OK;
    if(!unchanged)++generation;
    uint8_t header[32]={0};memcpy(header,"FRC1",4);
    put32(header+4,key);put32(header+8,(uint32_t)generation);put32(header+12,(uint32_t)(generation>>32));
    put32(header+16,(uint32_t)length);put32(header+20,crc^UINT32_MAX);
    put32(header+24,crc_update(UINT32_MAX,header,24)^UINT32_MAX);
    bool creating=rc==FEISHU_ROOT_MISSING;
    unsigned target=creating ? 0:current^1u;
    rc=write_copy(root,key,target,header,parts,count);
    if(rc) {
        /* Only our failed first creation is disposable; never remove an old
         * readable generation or a pre-existing damaged record. */
        if(creating) {char path[128];if(path_for(root,key,target,path))remove(path);}
        return unchanged ? FEISHU_ROOT_OK:rc;
    }
    /* Once committed, a failed redundant copy cannot undo the valid new value. */
    (void)write_copy(root,key,target^1u,header,parts,count);
    return FEISHU_ROOT_OK;
}

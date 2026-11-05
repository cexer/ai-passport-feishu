#include "feishu_text_map.h"
#include <limits.h>
#include <string.h>

typedef struct {
    const feishu_text_map_io_t *io;
    uint8_t *buffer;
    uint64_t total,position,base;
    size_t available;
    uint8_t unit[1+FEISHU_TEXT_MAP_NAME_BYTES];
} reader_t;

static bool valid_utf8(const uint8_t *data,size_t size,size_t *scalar)
{
    if(!size || !data[0])return false;
    unsigned n;uint32_t cp,minimum;
    if(data[0]<0x80) {n=1;cp=data[0];minimum=0;}
    else if(data[0]>=0xc2 && data[0]<=0xdf) {n=2;cp=data[0]&31;minimum=0x80;}
    else if(data[0]>=0xe0 && data[0]<=0xef) {n=3;cp=data[0]&15;minimum=0x800;}
    else if(data[0]>=0xf0 && data[0]<=0xf4) {n=4;cp=data[0]&7;minimum=0x10000;}
    else return false;
    if(n>size)return false;
    for(unsigned i=1;i<n;++i) {
        if((data[i]&0xc0)!=0x80)return false;
        cp=(cp<<6)|(data[i]&63);
    }
    if(cp<minimum || cp>0x10ffff || (cp>=0xd800 && cp<=0xdfff))return false;
    *scalar=n;return true;
}
static bool valid_name(const uint8_t *value,size_t size)
{
    if(!size || size>FEISHU_TEXT_MAP_NAME_BYTES)return false;
    for(size_t at=0;at<size;) {
        size_t n;if(!valid_utf8(value+at,size-at,&n))return false;
        at+=n;
    }
    return true;
}
/* -1 IO error, 0 EOF, 1 byte. Arbitrary short source reads are supported. */
static int peek(reader_t *r,uint64_t offset,uint8_t *byte)
{
    if(offset>=r->total)return 0;
    uint64_t base=offset/FEISHU_TEXT_MAP_WORKSPACE*FEISHU_TEXT_MAP_WORKSPACE;
    if(r->base!=base) {
        size_t expected=(size_t)(r->total-base);
        if(r->total-base>FEISHU_TEXT_MAP_WORKSPACE)expected=FEISHU_TEXT_MAP_WORKSPACE;
        r->available=0;
        while(r->available<expected) {
            size_t got=0,remaining=expected-r->available;
            if(!r->io->read(r->io->context,base+r->available,r->buffer+r->available,remaining,&got) || !got || got>remaining)
                return -1;
            r->available+=got;
        }
        r->base=base;
    }
    *byte=r->buffer[(size_t)(offset-base)];return 1;
}
static bool token_byte(uint8_t byte)
{ return (byte>='a' && byte<='z') || (byte>='A' && byte<='Z') || (byte>='0' && byte<='9') || byte=='_'; }
static int next_unit(reader_t *r,size_t *size)
{
    if(r->io->cancelled && r->io->cancelled(r->io->context))return -1;
    uint8_t byte;int status=peek(r,r->position,&byte);if(status<=0)return status;
    if(byte=='@') {
        char key[32];size_t length=1;key[0]='@';++r->position;
        while(length<sizeof(key)-1) {
            status=peek(r,r->position,&byte);if(status<0)return -1;
            if(!status || !token_byte(byte))break;
            key[length++]=(char)byte;++r->position;
        }
        key[length]=0;
        status=peek(r,r->position,&byte);if(status<0)return -1;
        bool complete=!status || !token_byte(byte);
        if(complete && length>1 && r->io->lookup) {
            size_t got=0;
            int found=r->io->lookup(r->io->context,key,r->unit+1,FEISHU_TEXT_MAP_NAME_BYTES,&got);
            if(found<0 || (found && !valid_name(r->unit+1,got)))return -1;
            if(found) {r->unit[0]='@';*size=got+1;return 1;}
        }
        memcpy(r->unit,key,length);*size=length;return 1;
    }
    unsigned count=byte<0x80 ? 1:byte<0xe0 ? 2:byte<0xf0 ? 3:4;
    for(unsigned i=0;i<count;++i) {
        if(peek(r,r->position+i,r->unit+i)!=1)return -1;
    }
    size_t scalar=0;if(!valid_utf8(r->unit,count,&scalar) || scalar!=count)return -1;
    r->position+=count;*size=count;return 1;
}
static bool initialize(reader_t *reader,const feishu_text_map_io_t *io,uint64_t total,
                         uint64_t start,void *workspace,size_t capacity)
{
    if(!io || !io->read || !workspace || capacity<FEISHU_TEXT_MAP_WORKSPACE || start>total)return false;
    *reader=(reader_t){.io=io,.buffer=workspace,.total=total,.position=start,.base=UINT64_MAX};return true;
}
bool feishu_text_map_scan(const feishu_text_map_io_t *io,uint64_t raw_size,
                           void *workspace,size_t workspace_size,char *preview,size_t preview_capacity,
                           uint64_t *logical_size)
{
    if(!logical_size || (preview_capacity && !preview))return false;
    *logical_size=0;size_t used=0;
    if(preview_capacity)preview[0]=0;
    reader_t reader;if(!initialize(&reader,io,raw_size,0,workspace,workspace_size))return false;
    uint64_t logical=0,last_checkpoint=0,last_raw=0;size_t size;int status;
    while((status=next_unit(&reader,&size))>0) {
        if(logical>UINT64_MAX-size)return false;
        if(preview_capacity && used<preview_capacity-1) {
            size_t n=preview_capacity-1-used;if(n>size)n=size;
            memcpy(preview+used,reader.unit,n);used+=n;preview[used]=0;
        }
        logical+=size;
        if(logical-last_checkpoint>=FEISHU_TEXT_MAP_WORKSPACE || reader.position-last_raw>=FEISHU_TEXT_MAP_WORKSPACE) {
            if(io->checkpoint && !io->checkpoint(io->context,reader.position,logical))return false;
            last_checkpoint=logical;last_raw=reader.position;
        }
    }
    if(status<0)return false;
    if(preview_capacity) {
        size_t valid=0,n;
        while(valid<used && valid_utf8((const uint8_t *)preview+valid,used-valid,&n))valid+=n;
        preview[valid]=0;
    }
    *logical_size=logical;return true;
}
bool feishu_text_map_read(const feishu_text_map_io_t *io,uint64_t raw_size,
                           uint64_t start_raw,uint64_t start_logical,uint64_t offset,
                           void *data,size_t capacity,void *workspace,size_t workspace_size,size_t *size)
{
    if(!size || (capacity && !data) || start_logical>offset)return false;
    *size=0;reader_t reader;
    if(!initialize(&reader,io,raw_size,start_raw,workspace,workspace_size))return false;
    uint64_t logical=start_logical;size_t unit;int status=0;
    while(*size<capacity && (status=next_unit(&reader,&unit))>0) {
        if(logical>UINT64_MAX-unit)return false;
        uint64_t skip=offset>logical ? offset-logical:0;
        if(skip<unit) {
            size_t n=unit-(size_t)skip;if(n>capacity-*size)n=capacity-*size;
            memcpy((uint8_t *)data+*size,reader.unit+(size_t)skip,n);*size+=n;
        }
        logical+=unit;
    }
    return status>=0;
}

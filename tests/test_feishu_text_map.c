#include "feishu_text_map.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {uint64_t raw,logical;} point_t;
typedef struct {
    const char *raw;size_t length,fragment,fail_read;
    bool fail_lookup,fail_checkpoint,cancel,bad_name;
    point_t points[128];size_t count,largest_read;
} source_t;
static bool read_at(void *context,uint64_t offset,void *data,size_t capacity,size_t *size)
{
    source_t *s=context;assert(capacity<=1024);if(capacity>s->largest_read)s->largest_read=capacity;
    *size=0;if(s->fail_read && offset>=s->fail_read)return false;
    if(offset>=s->length)return true;
    size_t n=s->length-(size_t)offset;if(n>capacity)n=capacity;
    if(s->fragment && n>s->fragment)n=s->fragment;
    memcpy(data,s->raw+offset,n);*size=n;return true;
}
static int lookup(void *context,const char *key,void *value,size_t capacity,size_t *size)
{
    source_t *s=context;if(s->fail_lookup)return -1;
    if(s->bad_name) {assert(capacity>=3);memcpy(value,"\xed\xa0\x80",3);*size=3;return 1;}
    const char *name=!strcmp(key,"@_user_1") ? "甲":!strcmp(key,"@_user_10") ? "乙乙":NULL;
    if(!strcmp(key,"@x")) {assert(capacity>=128);memset(value,'N',128);*size=128;return 1;}
    if(!name)return 0;
    *size=strlen(name);assert(*size<=capacity);memcpy(value,name,*size);return 1;
}
static bool checkpoint(void *context,uint64_t raw,uint64_t logical)
{
    source_t *s=context;if(s->fail_checkpoint)return false;
    assert(s->count<128 && raw<=s->length);
    if(s->count)assert(raw>s->points[s->count-1].raw && logical>s->points[s->count-1].logical);
    if(raw<s->length)assert(((uint8_t)s->raw[raw]&0xc0)!=0x80);
    s->points[s->count++]=(point_t){raw,logical};return true;
}
static bool cancelled(void *context) {return ((source_t *)context)->cancel;}
static feishu_text_map_io_t operations(source_t *source)
{ return (feishu_text_map_io_t){read_at,lookup,checkpoint,cancelled,source}; }
static char raw[20000],expected[70000];
int main(void)
{
    memset(raw,'a',1022);memset(expected,'a',1022);size_t nr=1022,ne=1022;
    const char *input="@_user_1 / @_user_10 / @_user_100 / @_user_1_extra 😀 段落 @x\n";
    char output[1024];size_t n=(size_t)snprintf(output,sizeof(output),"@甲 / @乙乙 / @_user_100 / @_user_1_extra 😀 段落 @");
    memset(output+n,'N',128);n+=128;output[n++]='\n';output[n]=0;
    for(unsigned i=0;i<250;++i) {
        assert(nr+strlen(input)<sizeof(raw) && ne+n<sizeof(expected));
        memcpy(raw+nr,input,strlen(input));nr+=strlen(input);
        memcpy(expected+ne,output,n);ne+=n;
    }
    raw[nr]=expected[ne]=0;
    source_t source={.raw=raw,.length=nr,.fragment=17};
    feishu_text_map_io_t io=operations(&source);uint8_t workspace[1024];char preview[256];uint64_t logical=0;
    assert(feishu_text_map_scan(&io,nr,workspace,sizeof(workspace),preview,sizeof(preview),&logical));
    assert(logical==ne && source.count>1 && !memcmp(preview,expected,strlen(preview)) && source.largest_read<=1024);
    for(size_t offset=0;offset<=ne+10;offset+=offset<50 ? 1:137) {
        point_t point={0};
        for(size_t i=0;i<source.count && source.points[i].logical<=offset;++i)point=source.points[i];
        for(size_t capacity=1;capacity<=256;capacity=capacity<7 ? capacity+1:capacity*2) {
            uint8_t bytes[256];size_t size=0;
            assert(feishu_text_map_read(&io,nr,point.raw,point.logical,offset,bytes,capacity,workspace,sizeof(workspace),&size));
            size_t want=offset<ne ? ne-offset:0;if(want>capacity)want=capacity;
            assert(size==want && (!want || !memcmp(bytes,expected+offset,want)));
        }
    }
    /* Complete-token matching also covers overlong token prefixes and EOF. */
    const char *edge="@_user_1 @x @_user_1abcdefghijklmnopqrstuvwxyz0123456789 @_user_10";
    source=(source_t){.raw=edge,.length=strlen(edge)};io=operations(&source);
    assert(feishu_text_map_scan(&io,source.length,workspace,sizeof(workspace),preview,sizeof(preview),&logical));
    assert(strstr(preview,"@_user_1abcdefghijklmnopqrstuvwxyz0123456789") && strstr(preview,"@乙乙"));
    for(unsigned fault=0;fault<5;++fault) {
        source=(source_t){.raw=raw,.length=nr,.fragment=17};
        source.fail_read=fault==0 ? 40:0;source.fail_lookup=fault==1;
        source.fail_checkpoint=fault==2;source.cancel=fault==3;source.bad_name=fault==4;io=operations(&source);
        assert(!feishu_text_map_scan(&io,nr,workspace,sizeof(workspace),preview,sizeof(preview),&logical));
        source=(source_t){.raw=raw,.length=nr};io=operations(&source);
        assert(feishu_text_map_scan(&io,nr,workspace,sizeof(workspace),preview,sizeof(preview),&logical) && logical==ne);
    }
    const char invalid[]="bad\xf0\x80\x80\x80";
    source=(source_t){.raw=invalid,.length=sizeof(invalid)-1};io=operations(&source);
    assert(!feishu_text_map_scan(&io,source.length,workspace,sizeof(workspace),preview,sizeof(preview),&logical));
    source=(source_t){.raw="",.length=0};io=operations(&source);
    assert(feishu_text_map_scan(&io,0,workspace,sizeof(workspace),preview,sizeof(preview),&logical) && !logical && !preview[0]);
    puts("text mention iterator: exact tokens, UTF-8, block/page boundaries, sparse seeks and IO/cancel recovery PASS; 1 KiB source workspace");
}

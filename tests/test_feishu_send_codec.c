#include "feishu_send_codec.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint8_t *data;size_t length,reads;bool fail; } source_t;
static bool read_bytes(void *context,uint64_t offset,void *data,size_t capacity,size_t *size)
{
    source_t *s=context;assert(capacity<=1024 && offset<=s->length);
    *size=0;if(s->fail)return false;
    ++s->reads;size_t fragment=1+s->reads*37%1024;
    if(capacity>fragment)capacity=fragment;
    if(capacity>s->length-offset)capacity=s->length-(size_t)offset;
    memcpy(data,s->data+(size_t)offset,capacity);*size=capacity;return true;
}
static bool write_bytes(void *context,const void *data,size_t size)
{ assert(size<=512);return fwrite(data,1,size,context)==size; }
static bool fail_write(void *context,const void *data,size_t size)
{ (void)context;(void)data;(void)size;return false; }
int main(int argc,char **argv)
{
    assert(argc==2);source_t value={0};
    value.data=malloc(300*1024);assert(value.data);uint8_t *allocation=value.data;
    static const uint8_t pattern[]={0,1,2,3,8,9,10,12,13,31,'"','\\','X',
        0xe4,0xb8,0xad,0xf0,0x9f,0x98,0x80};
    while(value.length+sizeof(pattern)<=300*1024) {
        memcpy(value.data+value.length,pattern,sizeof(pattern));value.length+=sizeof(pattern);
    }
    char path[256];snprintf(path,sizeof(path),"%s/send-source.bin",argv[1]);
    FILE *raw=fopen(path,"wb");assert(raw);assert(fwrite(value.data,1,value.length,raw)==value.length);assert(!fclose(raw));
    feishu_send_source_t source={read_bytes,&value,value.length};
    uint64_t offset=0;unsigned part_count=0;feishu_send_part_t part;
    while(offset<source.length) {
        char uuid[41];snprintf(uuid,sizeof(uuid),"fake-uuid-%u",part_count);
        assert(feishu_send_plan(&source,offset,"oc_fake_chat",uuid,false,&part));
        assert(part.offset==offset && part.length && part.body_size<=FEISHU_SEND_BODY_LIMIT);
        snprintf(path,sizeof(path),"%s/send-%02u.json",argv[1],part_count++);
        FILE *f=fopen(path,"wb");assert(f);assert(feishu_send_emit(&source,&part,write_bytes,f));
        assert(!fseek(f,0,SEEK_END) && (size_t)ftell(f)==part.body_size);assert(!fclose(f));
        offset+=part.length;
        if(offset<source.length)assert((value.data[offset]&0xc0)!=0x80);
    }
    assert(part_count>1);
    assert(!feishu_send_plan(&source,0,"oc_\"injection","fake",false,&part));
    assert(!feishu_send_plan(&source,0,"oc_fake","bad/uuid",false,&part));
    assert(!feishu_send_plan(&source,source.length,"oc_fake","fake",false,&part));
    value.fail=true;assert(!feishu_send_plan(&source,0,"om_fake","fake",true,&part));value.fail=false;
    assert(feishu_send_plan(&source,0,"om_fake","fake",true,&part));
    assert(!feishu_send_emit(&source,&part,fail_write,NULL));
    snprintf(path,sizeof(path),"%s/send-reply.json",argv[1]);
    FILE *reply=fopen(path,"wb");assert(reply);assert(feishu_send_emit(&source,&part,write_bytes,reply));assert(!fclose(reply));
    uint8_t invalid[]={0xf0,0x9f,0x98};value.data[0]=0xc0;
    assert(!feishu_send_plan(&source,0,"oc_fake","fake",false,&part));
    value.data=invalid;value.length=sizeof(invalid);source.length=value.length;
    assert(!feishu_send_plan(&source,0,"oc_fake","fake",false,&part));
    printf("send codec: %u bounded JSON parts; fragments, UTF-8 and failure recovery: PASS\n",part_count);
    free(allocation);
    return 0;
}

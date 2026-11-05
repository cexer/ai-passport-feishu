#include "feishu_send_codec.h"
#include <stdio.h>
#include <string.h>

static const char suffix[]="\\\"}\"}";
typedef struct {
    const feishu_send_source_t *source;
    uint64_t next;
    size_t used,size;
    uint8_t bytes[1024];
} reader_t;
static bool byte(reader_t *r,uint8_t *value)
{
    if(r->used==r->size) {
        if(r->next>=r->source->length)return false;
        size_t n=sizeof(r->bytes);
        if(r->source->length-r->next<n)n=(size_t)(r->source->length-r->next);
        r->size=0;
        if(!r->source->read(r->source->context,r->next,r->bytes,n,&r->size) || !r->size || r->size>n)return false;
        r->next+=r->size;r->used=0;
    }
    *value=r->bytes[r->used++];return true;
}
static size_t escape(uint8_t value,uint8_t out[12])
{
    uint8_t inner[6];size_t n=1;inner[0]=value;
    if(value=='"' || value=='\\') {inner[0]='\\';inner[1]=value;n=2;}
    else if(value<32) {
        static const char hex[]="0123456789abcdef";
        inner[0]='\\';
        switch(value) {
        case '\n':inner[1]='n';n=2;break;
        case '\r':inner[1]='r';n=2;break;
        case '\t':inner[1]='t';n=2;break;
        case '\b':inner[1]='b';n=2;break;
        case '\f':inner[1]='f';n=2;break;
        default:inner[1]='u';inner[2]='0';inner[3]='0';inner[4]=hex[value>>4];inner[5]=hex[value&15];n=6;
        }
    }
    size_t used=0;
    for(size_t i=0;i<n;++i) {if(inner[i]=='"' || inner[i]=='\\')out[used++]='\\';out[used++]=inner[i];}
    return used;
}
static bool identifier(const char *s,size_t max)
{
    if(!s || !*s)return false;
    size_t n=0;
    for(;s[n];++n) {
        if(n>=max)return false;
        unsigned char c=(unsigned char)s[n];
        if(!((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='_' || c=='-'))return false;
    }
    return true;
}
bool feishu_send_plan(const feishu_send_source_t *source,uint64_t offset,const char *destination,
                      const char *uuid,bool reply,feishu_send_part_t *part)
{
    if(!source || !source->read || !part || offset>=source->length ||
       !identifier(destination,63) || !identifier(uuid,40))return false;
    memset(part,0,sizeof(*part));part->offset=offset;
    char receive[96]={0};
    if(!reply)snprintf(receive,sizeof(receive),"\"receive_id\":\"%s\",",destination);
    int n=snprintf(part->header,sizeof(part->header),
        "{\"msg_type\":\"text\",\"uuid\":\"%s\",%s\"content\":\"{\\\"text\\\":\\\"",uuid,receive);
    if(n<=0 || (size_t)n>=sizeof(part->header))return false;
    part->header_size=(size_t)n;part->body_size=(size_t)n+sizeof(suffix)-1;
    reader_t reader={.source=source,.next=offset};
    while(offset+part->length<source->length) {
        uint8_t scalar[4];if(!byte(&reader,scalar))return false;
        size_t size=1;uint32_t value=scalar[0],minimum=0;
        if(scalar[0]>=0xc2 && scalar[0]<=0xdf) {size=2;value&=31;minimum=0x80;}
        else if(scalar[0]>=0xe0 && scalar[0]<=0xef) {size=3;value&=15;minimum=0x800;}
        else if(scalar[0]>=0xf0 && scalar[0]<=0xf4) {size=4;value&=7;minimum=0x10000;}
        else if(scalar[0]>=0x80)return false;
        for(size_t i=1;i<size;++i) {
            if(!byte(&reader,scalar+i) || (scalar[i]&0xc0)!=0x80)return false;
            value=(value<<6)|(scalar[i]&63);
        }
        if(value<minimum || value>0x10ffff || (value>=0xd800 && value<=0xdfff))return false;
        size_t encoded=0;uint8_t escaped[12];
        for(size_t i=0;i<size;++i)encoded+=escape(scalar[i],escaped);
        if(encoded>FEISHU_SEND_BODY_LIMIT-part->body_size)break;
        part->length+=size;part->body_size+=encoded;
    }
    return part->length!=0;
}
typedef struct { feishu_send_sink_t sink;void *context;size_t used,total;uint8_t bytes[512]; } output_t;
static bool flush(output_t *out)
{
    if(!out->used)return true;
    if(!out->sink(out->context,out->bytes,out->used))return false;
    out->total+=out->used;out->used=0;return true;
}
static bool emit(output_t *out,const void *bytes,size_t size)
{
    const uint8_t *p=bytes;
    while(size) {
        size_t n=sizeof(out->bytes)-out->used;if(n>size)n=size;
        memcpy(out->bytes+out->used,p,n);out->used+=n;p+=n;size-=n;
        if(out->used==sizeof(out->bytes) && !flush(out))return false;
    }
    return true;
}
bool feishu_send_emit(const feishu_send_source_t *source,const feishu_send_part_t *part,
                      feishu_send_sink_t sink,void *context)
{
    if(!source || !source->read || !part || !sink || !part->length || part->offset>source->length ||
       part->length>source->length-part->offset || part->header_size>=sizeof(part->header) ||
       part->body_size>FEISHU_SEND_BODY_LIMIT)return false;
    output_t out={.sink=sink,.context=context};reader_t reader={.source=source,.next=part->offset};
    if(!emit(&out,part->header,part->header_size))return false;
    for(uint64_t n=0;n<part->length;++n) {
        uint8_t value,escaped[12];if(!byte(&reader,&value))return false;
        if(!emit(&out,escaped,escape(value,escaped)))return false;
    }
    return emit(&out,suffix,sizeof(suffix)-1) && flush(&out) && out.total==part->body_size;
}

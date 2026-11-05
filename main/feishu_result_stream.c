#include "feishu_result_stream.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static bool equals(const uint8_t *p,size_t n,const char *value)
{ return p && strlen(value)==n && !memcmp(p,value,n); }
static bool flush(feishu_result_stream_t *s)
{
    if(s->used && s->sink && !s->sink(s->context,s->block,s->used))return false;
    s->used=0;return true;
}
static bool event(void *context,feishu_json_event_t type,unsigned depth,const uint8_t *p,size_t n)
{
    feishu_result_stream_t *s=context;
    if(type==FEISHU_JSON_OBJECT_BEGIN && depth==1)s->root=true;
    if(type==FEISHU_JSON_KEY && depth==2) {
        s->key=equals(p,n,"code") ? 1:equals(p,n,"data") ? 2:0;
        if(s->key==1) {if(s->code_seen)return false;s->code_seen=true;}
        if(s->key==2) {if(s->data_seen)return false;s->data_seen=true;}
        return true;
    }
    if(depth==2 && s->key==1) {
        if(type!=FEISHU_JSON_ATOM || !p || !n || n>11)return false;
        char text[12],*end;memcpy(text,p,n);text[n]=0;errno=0;
        long code=strtol(text,&end,10);
        if(errno || end!=text+n || code<INT_MIN || code>INT_MAX)return false;
        s->code=(int)code;s->key=0;return true;
    }
    if(depth==2 && s->key==2 && type==FEISHU_JSON_OBJECT_BEGIN) {
        s->in_data=true;s->key=0;return true;
    }
    if(depth==2 && s->key==2)return false;
    if(type==FEISHU_JSON_OBJECT_END && depth==2 && s->in_data) {s->in_data=false;s->selected=false;return true;}
    if(type==FEISHU_JSON_KEY && depth==3 && s->in_data) {
        s->selected=equals(p,n,s->field);
        if(s->selected && s->field_seen)return false;
        return true;
    }
    if(depth!=3 || !s->in_data || !s->selected)return true;
    if(type==FEISHU_JSON_STRING_BEGIN) {s->field_seen=true;s->in_string=true;return true;}
    if(type==FEISHU_JSON_STRING_END) {s->in_string=false;s->selected=false;return flush(s);}
    if(type!=FEISHU_JSON_STRING_PART || !s->in_string || n>s->limit-s->length || memchr(p,0,n))return false;
    s->length+=n;
    if(s->preview && !s->preview_full) {
        if(n>=s->preview_capacity-s->preview_size)s->preview_full=true;
        else {memcpy(s->preview+s->preview_size,p,n);s->preview_size+=n;s->preview[s->preview_size]=0;}
    }
    while(n) {
        size_t take=sizeof(s->block)-s->used;if(take>n)take=n;
        memcpy(s->block+s->used,p,take);s->used+=take;p+=take;n-=take;
        if(s->used==sizeof(s->block) && !flush(s))return false;
    }
    return true;
}
void feishu_result_stream_init(feishu_result_stream_t *s,const char *field,uint64_t limit,
                                char *preview,size_t capacity,feishu_result_sink_t sink,void *context)
{
    memset(s,0,sizeof(*s));s->code=-1;s->limit=limit;s->sink=sink;s->context=context;
    s->preview=capacity ? preview:NULL;s->preview_capacity=capacity;
    if(s->preview)s->preview[0]=0;
    if(!field || !*field || strlen(field)>=sizeof(s->field))s->failed=true;
    else memcpy(s->field,field,strlen(field)+1);
    feishu_json_init(&s->json,event,s);
}
bool feishu_result_stream_feed(feishu_result_stream_t *s,const void *data,size_t size)
{ if(s->failed)return false;return !(s->failed=!feishu_json_feed(&s->json,data,size)); }
bool feishu_result_stream_finish(feishu_result_stream_t *s)
{
    return !s->failed && feishu_json_finish(&s->json) && s->root && s->code_seen && s->code==0 &&
        s->data_seen && s->field_seen && !s->in_string;
}

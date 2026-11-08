#pragma once
#include "feishu_json.h"
#include <limits.h>
#include <string.h>
/* Error envelopes never enter a message/media storage sink. */
typedef struct {feishu_json_t json;size_t bytes;int code;bool root,seen,selected;} feishu_api_error_t;
static inline bool feishu_api_error_event(void *context,feishu_json_event_t event,unsigned depth,const uint8_t *data,size_t size)
{
    feishu_api_error_t *p=context;
    if(event==FEISHU_JSON_OBJECT_BEGIN && depth==1)p->root=true;
    if(event==FEISHU_JSON_KEY && depth==2){
        p->selected=size==4 && !memcmp(data,"code",4);
        if(p->selected){if(p->seen)return false;p->seen=true;}
    }
    if(depth!=2 || !p->selected)return true;
    if(event==FEISHU_JSON_STRING_BEGIN || event==FEISHU_JSON_OBJECT_BEGIN || event==FEISHU_JSON_ARRAY_BEGIN)return false;
    if(event==FEISHU_JSON_ATOM){
        if(!size || size>10)return false;
        int code=0;for(size_t i=0;i<size;++i){if(data[i]<'0' || data[i]>'9' || code>(INT_MAX-(data[i]-'0'))/10)return false;code=code*10+data[i]-'0';}
        p->code=code;
    }
    return true;
}
static inline void feishu_api_error_init(feishu_api_error_t *p)
{memset(p,0,sizeof(*p));p->code=-1;feishu_json_init(&p->json,feishu_api_error_event,p);}
static inline bool feishu_api_error_feed(feishu_api_error_t *p,const void *data,size_t size)
{if(size>4096-p->bytes)return false;p->bytes+=size;return feishu_json_feed(&p->json,data,size);}
static inline bool feishu_api_error_finish(feishu_api_error_t *p)
{return feishu_json_finish(&p->json) && p->root && p->seen && p->code>=0;}

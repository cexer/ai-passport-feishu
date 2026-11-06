#include "feishu_xiaozhi_protocol.h"
#include <string.h>
int feishu_xiao_pairing_classify(bool activation,const char *code,bool websocket_valid,char output[17])
{
    output[0]=0;
    if(!activation)return websocket_valid ? 0:-1;
    if(code) {
        size_t n=strlen(code);if(!n || n>16)return -1;
        for(size_t i=0;i<n;++i)if(code[i]<'0' || code[i]>'9')return -1;
        memcpy(output,code,n+1);
    }
    return 1;
}
size_t feishu_xiao_binary_frame(uint8_t *buffer,size_t capacity,size_t size,const uint8_t mask[4])
{
    size_t header=size<126 ? 6:8;
    if(!buffer || !mask || size>65535 || capacity<header || size>capacity-header)return 0;
    uint8_t key[4];memcpy(key,mask,4);
    memmove(buffer+header,buffer,size);
    buffer[0]=0x82;buffer[1]=(uint8_t)(0x80|(size<126 ? size:126));
    if(size>=126){buffer[2]=(uint8_t)(size>>8);buffer[3]=(uint8_t)size;}
    memcpy(buffer+header-4,key,4);
    for(size_t i=0;i<size;++i)buffer[header+i]^=key[i%4];
    return header+size;
}
static bool equal(const uint8_t *data,size_t size,const char *s)
{return size==strlen(s) && !memcmp(data,s,size);}
static bool field(void *context,feishu_json_event_t event,unsigned depth,const uint8_t *data,size_t size)
{
    feishu_xiao_message_t *p=context;
    if(event==FEISHU_JSON_OBJECT_BEGIN && depth==1)p->root=true;
    if(event==FEISHU_JSON_KEY && depth==2) {
        p->selected=equal(data,size,"type") ? 1:equal(data,size,"session_id") ? 2:equal(data,size,"text") ? 3:0;
        if(p->selected) {
            unsigned mask=1u<<p->selected;
            if(p->seen&mask){p->invalid=true;return false;}
            p->seen|=mask;
        }
    }
    if(depth!=2 || !p->selected)return true;
    unsigned i=p->selected-1;
    if(event==FEISHU_JSON_STRING_BEGIN)p->strings|=1u<<p->selected;
    if(event==FEISHU_JSON_STRING_PART) {
        char *outputs[]={p->type,p->session,p->text};
        const size_t capacities[]={sizeof(p->type),sizeof(p->session),sizeof(p->text)};
        if(size>=capacities[i]-p->lengths[i]){p->invalid=true;return false;}
        for(size_t j=0;j<size;++j)if(!data[j] || (i<2 && (data[j]<32 || data[j]>126))){p->invalid=true;return false;}
        memcpy(outputs[i]+p->lengths[i],data,size);p->lengths[i]+=size;outputs[i][p->lengths[i]]=0;
    }
    return true;
}
void feishu_xiao_message_init(feishu_xiao_message_t *p)
{memset(p,0,sizeof(*p));feishu_json_init(&p->json,field,p);}
bool feishu_xiao_message_feed(feishu_xiao_message_t *p,const void *data,size_t size)
{return !p->invalid && feishu_json_feed(&p->json,data,size);}
int feishu_xiao_message_finish(feishu_xiao_message_t *p)
{
    if(p->invalid || !p->root || !(p->strings&2) || !feishu_json_finish(&p->json))return -1;
    if(!strcmp(p->type,"hello"))return XIAO_MESSAGE_HELLO;
    if(!strcmp(p->type,"stt"))return (p->strings&8) ? XIAO_MESSAGE_STT:-1;
    if(!strcmp(p->type,"alert"))return XIAO_MESSAGE_ALERT;
    return XIAO_MESSAGE_OTHER;
}

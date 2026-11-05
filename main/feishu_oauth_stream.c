#include "feishu_oauth_stream.h"
#include <string.h>

static bool equals(const uint8_t *data,size_t size,const char *name)
{ return data && size==strlen(name) && !memcmp(data,name,size); }
static bool value(void *context,feishu_json_event_t event,unsigned depth,const uint8_t *data,size_t size)
{
    feishu_oauth_stream_t *p=context;
    if(event==FEISHU_JSON_OBJECT_BEGIN && depth==1)p->root_object=true;
    if(event==FEISHU_JSON_KEY && depth==2) {
        p->selected=equals(data,size,"access_token") ? 1 : equals(data,size,"refresh_token") ? 2 : equals(data,size,"error") ? 3 : 0;
        if(p->selected) {
            unsigned flag=1u<<p->selected;
            if(p->seen&flag) {p->invalid=true;return false;}
            p->seen|=flag;
        }
    }
    if(depth!=2 || !p->selected)return true;
    unsigned i=p->selected-1;
    if(event==FEISHU_JSON_STRING_PART && p->outputs[i]) {
        if(size>=p->capacities[i]-p->lengths[i]) {p->overflow=true;return false;}
        for(size_t j=0;j<size;++j) {
            if(!data[j] || (i<2 && (data[j]<0x21 || data[j]>0x7e))) {p->invalid=true;return false;}
        }
        memcpy(p->outputs[i]+p->lengths[i],data,size);p->lengths[i]+=size;p->outputs[i][p->lengths[i]]=0;
    }
    return true;
}
void feishu_oauth_stream_init(feishu_oauth_stream_t *p,char *access,size_t access_capacity,
                               char *refresh,size_t refresh_capacity,char *error,size_t error_capacity)
{
    memset(p,0,sizeof(*p));
    p->outputs[0]=access;p->outputs[1]=refresh;p->outputs[2]=error;
    p->capacities[0]=access_capacity;p->capacities[1]=refresh_capacity;p->capacities[2]=error_capacity;
    for(unsigned i=0;i<3;++i) {
        if(p->outputs[i] && p->capacities[i])p->outputs[i][0]=0;
        else p->outputs[i]=NULL;
    }
    feishu_json_init(&p->json,value,p);
}
bool feishu_oauth_stream_feed(feishu_oauth_stream_t *p,const void *data,size_t size)
{ return !p->invalid && !p->overflow && feishu_json_feed(&p->json,data,size); }
bool feishu_oauth_stream_finish(feishu_oauth_stream_t *p)
{ return !p->invalid && !p->overflow && p->root_object && feishu_json_finish(&p->json); }

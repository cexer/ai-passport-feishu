#include "feishu_registration_protocol.h"
#include "feishu_registration_config.h"
#include "feishu_json.h"
#include <string.h>
#include <stdio.h>

typedef struct {
    feishu_json_t json;
    feishu_register_result_t *result;
    char error[64],brand[16];
    unsigned selected,seen;
    size_t used;
    bool root,user_info;
} parser_t;
static bool equal(const uint8_t *data,size_t size,const char *value)
{ return size==strlen(value) && !memcmp(data,value,size); }
static bool event(void *context,feishu_json_event_t type,unsigned depth,const uint8_t *data,size_t size)
{
    parser_t *p=context;
    static const char *keys[]={"device_code","verification_uri_complete","client_id","client_secret",
                               "error","expires_in","interval","tenant_brand"};
    if(type==FEISHU_JSON_OBJECT_BEGIN && depth==1)p->root=true;
    if(type==FEISHU_JSON_KEY && depth==2) {
        p->user_info=equal(data,size,"user_info");p->selected=0;p->used=0;
        for(unsigned i=0;i<7;++i)if(equal(data,size,keys[i]))p->selected=i+1;
    } else if(type==FEISHU_JSON_KEY && depth==3 && p->user_info) {
        p->selected=equal(data,size,keys[7]) ? 8:0;p->used=0;
    } else if(type==FEISHU_JSON_KEY)return true;
    if(type==FEISHU_JSON_KEY && p->selected) {
        unsigned bit=1u<<p->selected;if(p->seen&bit)return false;p->seen|=bit;
    }
    if(!p->selected || (depth!=2 && !(depth==3 && p->user_info)))return true;
    unsigned field=p->selected;
    if(type==FEISHU_JSON_KEY || type==FEISHU_JSON_STRING_BEGIN)return true;
    if(field==6 || field==7) {
        if(type!=FEISHU_JSON_ATOM || !size || size>5)return false;
        uint32_t number=0;
        for(size_t i=0;i<size;++i) {if(data[i]<'0'||data[i]>'9')return false;number=number*10+data[i]-'0';}
        if(!number || number>3600)return false;
        if(field==6)p->result->expires_in=number;else p->result->interval=number;
        p->selected=0;return true;
    }
    if(type==FEISHU_JSON_STRING_END) {p->selected=0;return true;}
    if(type!=FEISHU_JSON_STRING_PART)return false;
    char *outputs[]={p->result->device_code,p->result->verification_url,p->result->app_id,
                    p->result->app_secret,p->error,NULL,NULL,p->brand};
    size_t caps[]={sizeof(p->result->device_code),sizeof(p->result->verification_url),
                   sizeof(p->result->app_id),sizeof(p->result->app_secret),sizeof(p->error),0,0,sizeof(p->brand)};
    if(size>=caps[field-1]-p->used)return false;
    for(size_t i=0;i<size;++i)if(data[i]<0x21 || data[i]>0x7e)return false;
    memcpy(outputs[field-1]+p->used,data,size);p->used+=size;outputs[field-1][p->used]=0;
    return true;
}
static bool app_valid(const char *app)
{
    if(strncmp(app,"cli_",4) || !app[4])return false;
    for(const unsigned char *p=(const unsigned char *)app+4;*p;++p)
        if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')))return false;
    return strlen(app)<64;
}
bool feishu_register_decode(const void *json,size_t length,bool beginning,feishu_register_result_t *r)
{
    if(!r)return false;
    memset(r,0,sizeof(*r));r->status=FEISHU_REGISTER_FAILED;
    if(!json || !length || length>4096)return false;
    parser_t p={.result=r};feishu_json_init(&p.json,event,&p);
    bool valid=feishu_json_feed(&p.json,json,length) && feishu_json_finish(&p.json) && p.root;
    if(!valid)goto failed;
    if(p.brand[0] && strcmp(p.brand,"feishu")) {r->status=FEISHU_REGISTER_UNSUPPORTED;return true;}
    if(p.error[0]) {
        if(!strcmp(p.error,"authorization_pending"))r->status=FEISHU_REGISTER_PENDING;
        else if(!strcmp(p.error,"slow_down"))r->status=FEISHU_REGISTER_SLOW;
        else if(!strcmp(p.error,"expired_token"))r->status=FEISHU_REGISTER_EXPIRED;
        else if(!strcmp(p.error,"access_denied"))r->status=FEISHU_REGISTER_DENIED;
        return true;
    }
    if(beginning) {
        char url[FEISHU_REGISTER_URL_MAX];
        if(!r->device_code[0] || !feishu_register_url(url,sizeof(url),r->verification_url,NULL))goto failed;
        if(!r->expires_in)r->expires_in=600;
        if(!r->interval)r->interval=5;
        if(r->interval<2)r->interval=2;
        if(r->interval>60)goto failed;
        r->status=FEISHU_REGISTER_PENDING;return true;
    }
    if(app_valid(r->app_id) && r->app_secret[0]) {r->status=FEISHU_REGISTER_COMPLETE;return true;}
failed:
    memset(r,0,sizeof(*r));r->status=FEISHU_REGISTER_FAILED;return false;
}
bool feishu_register_url(char *out,size_t capacity,const char *base,const char *app)
{
    if(!out || !capacity || !base)return false;
    out[0]=0;
    const char *hosts[]={"https://open.feishu.cn/","https://accounts.feishu.cn/"};
    bool allowed=false;for(unsigned i=0;i<2;++i)if(!strncmp(base,hosts[i],strlen(hosts[i])))allowed=true;
    if(!allowed || strlen(base)>=FEISHU_REGISTER_URL_MAX || strchr(base,'#'))return false;
    for(const unsigned char *p=(const unsigned char *)base;*p;++p)if(*p<0x21 || *p>0x7e)return false;
    if(app && *app && !app_valid(app))return false;
    int n=snprintf(out,capacity,"%s%caddons=%s%s%s",base,strchr(base,'?')?'&':'?',
                   FEISHU_APP_ADDONS,app&&*app ? "&clientID=":"",app&&*app ? app:"");
    if(n<0 || (size_t)n>=capacity) {out[0]=0;return false;}return true;
}
bool feishu_register_poll_form(char *out,size_t capacity,const char *code)
{
    static const char prefix[]="action=poll&device_code=";
    if(!out || capacity<=sizeof(prefix) || !code || !*code || strlen(code)>=FEISHU_REGISTER_CODE_MAX)return false;
    memcpy(out,prefix,sizeof(prefix)-1);size_t used=sizeof(prefix)-1;
    const char *hex="0123456789ABCDEF";
    for(const unsigned char *p=(const unsigned char *)code;*p;++p) {
        bool plain=(*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||strchr("-._~",*p);
        size_t n=plain?1:3;if(n>=capacity-used) {out[0]=0;return false;}
        if(plain)out[used++]=(char)*p;else {out[used++]='%';out[used++]=hex[*p>>4];out[used++]=hex[*p&15];}
    }
    out[used]=0;return true;
}
size_t feishu_register_poll_form_size(const char *code)
{
    if(!code || !*code || strlen(code)>=FEISHU_REGISTER_CODE_MAX)return 0;
    size_t size=sizeof("action=poll&device_code=");
    for(const unsigned char *p=(const unsigned char *)code;*p;++p) {
        bool plain=(*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||strchr("-._~",*p);
        size+=plain?1:3;
    }
    return size;
}

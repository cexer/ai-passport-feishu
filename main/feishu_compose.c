#include "feishu_compose.h"
#include "feishu_outbox.h"
#include <stdio.h>
#include <string.h>

static bool utf8(const char *s,size_t maximum)
{
    if(!s)return false;
    size_t length=0;
    for(;length<=maximum && s[length];++length){}
    if(length>maximum)return false;
    for(size_t i=0;i<length;){
        unsigned char a=(unsigned char)s[i++];unsigned n;uint32_t code,min;
        if(a<0x80){if(a<32 && a!='\n' && a!='\t')return false;continue;}
        if(a>=0xc2 && a<=0xdf){n=1;code=a&31;min=0x80;}
        else if(a>=0xe0 && a<=0xef){n=2;code=a&15;min=0x800;}
        else if(a>=0xf0 && a<=0xf4){n=3;code=a&7;min=0x10000;}
        else return false;
        if(n>length-i)return false;
        while(n--){unsigned char b=(unsigned char)s[i++];if((b&0xc0)!=0x80)return false;code=(code<<6)|(b&63);}
        if(code<min || code>0x10ffff || (code>=0xd800 && code<=0xdfff))return false;
    }
    return true;
}
bool feishu_compose_editable(const feishu_compose_snapshot_t *d)
{
    return d && d->state==FEISHU_OUTBOX_READY && !d->offset &&
        memchr(d->chat,0,sizeof(d->chat)) && d->chat[0] &&
        memchr(d->message,0,sizeof(d->message)) &&
        memchr(d->uuid,0,sizeof(d->uuid)) && d->uuid[0] &&
        utf8(d->text,sizeof(d->text)-1) && d->length==strlen(d->text);
}
static uint64_t hash(uint64_t h,const char *text)
{
    do {h^=(unsigned char)*text;h*=UINT64_C(1099511628211);}while(*text++);
    return h;
}
bool feishu_compose_revision(const feishu_compose_snapshot_t *d,char revision[FEISHU_COMPOSE_REV_CAP])
{
    if(!revision)return false;
    revision[0]=0;if(!feishu_compose_editable(d))return false;
    uint64_t h=hash(UINT64_C(14695981039346656037),d->uuid);
    h=hash(hash(hash(h,d->chat),d->message),d->text);
    snprintf(revision,FEISHU_COMPOSE_REV_CAP,"d%016llx",(unsigned long long)h);return true;
}
bool feishu_compose_replace(const feishu_compose_snapshot_t *d,const char *revision,
    const char *text,feishu_compose_snapshot_t *out)
{
    char actual[FEISHU_COMPOSE_REV_CAP];
    if(!out || !revision || !feishu_compose_revision(d,actual) || strcmp(actual,revision) ||
       !utf8(text,FEISHU_COMPOSE_CAP-1))return false;
    *out=*d;memmove(out->text,text,strlen(text)+1);out->length=strlen(out->text);return true;
}
bool feishu_compose_undo(const feishu_compose_snapshot_t *d,const char *revision,
    const feishu_compose_snapshot_t *previous,feishu_compose_snapshot_t *out)
{
    if(!feishu_compose_editable(previous) || !feishu_compose_editable(d) ||
       strcmp(d->chat,previous->chat) || strcmp(d->message,previous->message))return false;
    return feishu_compose_replace(d,revision,previous->text,out);
}

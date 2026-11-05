#include "feishu_outbox.h"
#include <stdio.h>
#include <string.h>
static bool field(const char *s,size_t capacity,bool empty)
{
    const char *end=memchr(s,0,capacity);if(!end || (!empty && end==s))return false;
    for(const char *p=s;p<end;++p)if(!((*p>='a' && *p<='z') || (*p>='A' && *p<='Z') ||
        (*p>='0' && *p<='9') || *p=='_' || *p=='-'))return false;
    return true;
}
bool feishu_outbox_valid(const feishu_outbox_t *p)
{
    if(!p || p->version!=1 || p->reserved || p->state>FEISHU_OUTBOX_UNCERTAIN || p->segment>9999)return false;
    if(p->state==FEISHU_OUTBOX_EMPTY)return true;
    return p->object_id>0 && p->length>0 && p->length<=300u*1024u && p->offset<p->length &&
        field(p->body_name,sizeof(p->body_name),false) && field(p->chat,sizeof(p->chat),false) &&
        field(p->message,sizeof(p->message),true) && field(p->uuid,sizeof(p->uuid),false);
}
bool feishu_outbox_create(feishu_outbox_t *p,int64_t object,uint64_t length,
                           const char *name,const char *chat,const char *message,const char *uuid)
{
    if(!p || !name || !chat || !message || !uuid || strlen(name)>=sizeof(p->body_name) ||
       strlen(chat)>=sizeof(p->chat) || strlen(message)>=sizeof(p->message) || strlen(uuid)>=sizeof(p->uuid))return false;
    memset(p,0,sizeof(*p));p->version=1;p->state=FEISHU_OUTBOX_READY;p->object_id=object;p->length=length;
    strcpy(p->body_name,name);strcpy(p->chat,chat);strcpy(p->message,message);strcpy(p->uuid,uuid);
    return feishu_outbox_valid(p);
}
bool feishu_outbox_uuid(const feishu_outbox_t *p,char uuid[41])
{
    if(!feishu_outbox_valid(p) || p->state==FEISHU_OUTBOX_EMPTY || !uuid)return false;
    int n=snprintf(uuid,41,"%s-%04u",p->uuid,(unsigned)p->segment);return n>0 && n<=40;
}
bool feishu_outbox_attempt(feishu_outbox_t *p,bool confirm_uncertain)
{
    if(!feishu_outbox_valid(p) || p->state==FEISHU_OUTBOX_EMPTY ||
       (p->state!=FEISHU_OUTBOX_READY && !confirm_uncertain))return false;
    p->state=FEISHU_OUTBOX_SENDING;return true;
}
bool feishu_outbox_ack(feishu_outbox_t *p,uint64_t bytes)
{
    if(!feishu_outbox_valid(p) || p->state!=FEISHU_OUTBOX_SENDING || !bytes || bytes>p->length-p->offset)return false;
    p->offset+=bytes;++p->segment;
    p->state=p->offset==p->length ? FEISHU_OUTBOX_EMPTY:FEISHU_OUTBOX_READY;
    return p->segment<=9999;
}
void feishu_outbox_failed(feishu_outbox_t *p,bool rejected)
{ if(feishu_outbox_valid(p) && p->state==FEISHU_OUTBOX_SENDING)p->state=rejected ? FEISHU_OUTBOX_READY:FEISHU_OUTBOX_UNCERTAIN; }

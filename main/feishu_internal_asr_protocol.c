#include "feishu_internal_asr_protocol.h"
#include <string.h>
enum {TAG,VALUE,LENGTH,BYTES};
enum {IGNORE,TEXT,SOURCE,CID};
static bool put_var(uint8_t *b,size_t cap,size_t *used,uint64_t n)
{
    do {if(*used>=cap)return false;uint8_t v=n&127;n>>=7;b[(*used)++]=v|(n?128:0);}while(n);
    return true;
}
static bool put_num(uint8_t *b,size_t cap,size_t *used,unsigned f,uint64_t n)
{return put_var(b,cap,used,(uint64_t)f<<3)&&put_var(b,cap,used,n);}
static bool put_str(uint8_t *b,size_t cap,size_t *used,unsigned f,const char *v)
{
    size_t n=strlen(v);
    if(!put_var(b,cap,used,((uint64_t)f<<3)|2)||!put_var(b,cap,used,n)||n>cap-*used)return false;
    memcpy(b+*used,v,n);*used+=n;return true;
}
size_t feishu_internal_asr_header(uint8_t *out,size_t cap,const char *source,const char *cid,const char *context_id,
    uint32_t seq,unsigned action,size_t pcm_size,size_t *body_size)
{
    if(body_size)*body_size=0;
    if(!out||!source||!cid||!context_id||!context_id[0]||strlen(context_id)>64||!body_size||!source[0]||strlen(source)>64||!cid[0]||strlen(cid)>64||
       !seq||seq>INT32_MAX||(action!=0&&action!=1&&action!=2&&action!=4)||pcm_size>FEISHU_INTERNAL_ASR_PCM_LIMIT||pcm_size%2)return 0;
    uint8_t inner[256];size_t n=0,used=0;
    if(!put_str(inner,sizeof(inner),&n,1,source)||!put_num(inner,sizeof(inner),&n,2,seq)||
       !put_str(inner,sizeof(inner),&n,3,"zh_cn")||!put_num(inner,sizeof(inner),&n,5,action)||
       !put_num(inner,sizeof(inner),&n,6,16000)||!put_str(inner,sizeof(inner),&n,7,"pcm")||
       !put_num(inner,sizeof(inner),&n,8,1)||!put_str(inner,sizeof(inner),&n,9,"zh_cn")||
       !put_str(inner,sizeof(inner),&n,10,"")||!put_num(inner,sizeof(inner),&n,11,0)||
       !put_str(inner,sizeof(inner),&n,12,context_id)||!put_num(inner,sizeof(inner),&n,13,0)||
       !put_num(inner,sizeof(inner),&n,14,1)||
       !put_var(inner,sizeof(inner),&n,34)||!put_var(inner,sizeof(inner),&n,pcm_size))return 0;
    if(!put_num(out,cap,&used,2,1)||!put_num(out,cap,&used,3,FEISHU_INTERNAL_ASR_COMMAND)||
       !put_str(out,cap,&used,6,cid)||!put_var(out,cap,&used,42)||!put_var(out,cap,&used,n+pcm_size)||n>cap-used)return 0;
    memcpy(out+used,inner,n);used+=n;*body_size=used+pcm_size;return used;
}
static bool flush(feishu_internal_asr_parser_t *p)
{
    if(p->used&&p->sink&&!p->sink(p->context,p->block,p->used))return false;
    p->used=0;return true;
}
static bool scalar(feishu_internal_asr_parser_t *p,const uint8_t *v,size_t n)
{
    if(n>FEISHU_INTERNAL_ASR_LIMIT-p->text_length)return false;
    p->text_length+=n;
    if(p->preview&&!p->preview_full) {
        if(n>=p->preview_capacity-p->preview_size)p->preview_full=true;
        else {memcpy(p->preview+p->preview_size,v,n);p->preview_size+=n;p->preview[p->preview_size]=0;}
    }
    if(n>sizeof(p->block)-p->used&&!flush(p))return false;
    memcpy(p->block+p->used,v,n);p->used+=n;return true;
}
static bool utf8(feishu_internal_asr_parser_t *p,uint8_t v)
{
    if(!p->utf8_need) {
        if(!v)return false;
        if(v<128)return scalar(p,&v,1);
        p->utf8_size=1;p->utf8[0]=v;
        if(v>=0xc2&&v<=0xdf)p->utf8_need=1;
        else if(v>=0xe0&&v<=0xef)p->utf8_need=2;
        else if(v>=0xf0&&v<=0xf4)p->utf8_need=3;
        else return false;
        return true;
    }
    if(v<128||v>191)return false;
    p->utf8[p->utf8_size++]=v;
    if(--p->utf8_need)return true;
    uint32_t code=p->utf8[0]&(p->utf8_size==2?31:p->utf8_size==3?15:7);
    for(unsigned i=1;i<p->utf8_size;++i)code=(code<<6)|(p->utf8[i]&63);
    if((p->utf8_size==2&&code<128)||(p->utf8_size==3&&code<2048)||(p->utf8_size==4&&code<65536)||
       code>0x10ffff||(code>=0xd800&&code<=0xdfff))return false;
    return scalar(p,p->utf8,p->utf8_size);
}
static bool known(feishu_internal_asr_parser_t *p)
{
    unsigned f=p->field;bool selected=p->inner?(f==1||f==2||f==3||f==4||f==6||f==7):(f==2||f==3||f==4||f==5||f==6);
    if(!selected)return true;
    unsigned expected=p->inner?((f==1||f==3)?2:0):((f==5||f==6)?2:0);
    uint32_t *seen=p->inner?&p->inner_seen:&p->top_seen;
    if(p->wire!=expected||(*seen&(1u<<f)))return false;
    *seen|=1u<<f;return true;
}
static bool number(feishu_internal_asr_parser_t *p,uint64_t v)
{
    bool selected=p->inner?(p->field==2||p->field==4||p->field==6||p->field==7):(p->field==2||p->field==3||p->field==4);
    if(!selected)return true;
    if(v>UINT32_MAX)return false;
    if(p->inner) {
        /* Live response sequenceId tracks recognition revisions, not request
         * sequenceId. Zero/repeated values are normal; correlate by cid/source. */
        if(p->field==2){if(v>INT32_MAX)return false;p->response_sequence=(uint32_t)v;}
        if(p->field==4){if(v>1)return false;p->is_end=v!=0;}
        if(p->field==6)p->error_code=(uint32_t)v;
        if(p->field==7)p->handle_action=(uint32_t)v;
    } else {
        if(p->field==2&&v!=1)return false;
        if(p->field==3&&v!=1)return false;
        if(p->field==4)p->status=(uint32_t)v;
    }
    return true;
}
static bool field_end(feishu_internal_asr_parser_t *p)
{
    if(p->route==TEXT&&p->utf8_need)return false;
    if(p->route==SOURCE||p->route==CID) {
        p->small[p->small_size]=0;
        if(strcmp(p->small,p->route==SOURCE?p->source:p->cid))return false;
    }
    p->state=TAG;p->route=IGNORE;return true;
}
static bool byte(feishu_internal_asr_parser_t *p,uint8_t v)
{
    if(++p->total>FEISHU_INTERNAL_ASR_LIMIT+4096)return false;
    bool in=p->inner;
    if(in) {if(!p->payload_left)return false;--p->payload_left;}
    if(p->state==BYTES) {
        if(p->route==TEXT&&!utf8(p,v))return false;
        if(p->route==SOURCE||p->route==CID) {if(!v||p->small_size>=64)return false;p->small[p->small_size++]=(char)v;}
        if(!--p->remaining&&!field_end(p))return false;
    } else {
        if(p->shift==63&&(v&0xfe))return false;
        p->value|=(uint64_t)(v&127)<<p->shift;
        if(v&128) {if(p->shift>=63)return false;p->shift+=7;}
        else {
            uint64_t value=p->value;p->value=0;p->shift=0;
            if(p->state==TAG) {
                if(!value||value>UINT32_MAX)return false;
                p->field=(uint32_t)(value>>3);p->wire=value&7;
                if(!p->field||!known(p))return false;
                if(p->wire==0)p->state=VALUE;
                else if(p->wire==2)p->state=LENGTH;
                else if(p->wire==1||p->wire==5){p->remaining=p->wire==1?8:4;p->state=BYTES;p->route=IGNORE;}
                else return false;
            } else if(p->state==VALUE) {if(!number(p,value))return false;p->state=TAG;}
            else {
                if(value>FEISHU_INTERNAL_ASR_LIMIT+2048||(p->inner&&value>p->payload_left))return false;
                if(!p->inner&&p->field==5) {p->payload_seen=true;p->inner=true;p->payload_left=value;p->state=TAG;}
                else {
                    p->remaining=value;p->state=BYTES;p->small_size=0;
                    p->route=p->inner?(p->field==3?TEXT:p->field==1?SOURCE:IGNORE):(p->field==6?CID:IGNORE);
                    if(p->route==TEXT)p->text_seen=true;
                    if((p->route==SOURCE||p->route==CID)&&value>64)return false;
                    if(!value&&!field_end(p))return false;
                }
            }
        }
    }
    if(p->inner&&!p->payload_left) {if(p->state!=TAG||p->shift||!flush(p))return false;p->inner=false;}
    return true;
}
void feishu_internal_asr_init(feishu_internal_asr_parser_t *p,const char *source,const char *cid,uint32_t seq,
    char *preview,size_t capacity,feishu_internal_asr_sink_t sink,void *context)
{
    memset(p,0,sizeof(*p));p->sequence=seq;p->preview=capacity?preview:NULL;p->preview_capacity=capacity;p->sink=sink;p->context=context;
    if(p->preview)p->preview[0]=0;
    if(!source||!cid||!source[0]||!cid[0]||strlen(source)>64||strlen(cid)>64)p->failed=true;
    else {strcpy(p->source,source);strcpy(p->cid,cid);}
}
bool feishu_internal_asr_feed(feishu_internal_asr_parser_t *p,const void *data,size_t size)
{
    if(p->failed||(!data&&size))return false;
    const uint8_t *b=data;for(size_t i=0;i<size;++i)if(!byte(p,b[i])){p->failed=true;return false;}
    return true;
}
bool feishu_internal_asr_finish(feishu_internal_asr_parser_t *p)
{
    return !p->failed&&!p->inner&&p->state==TAG&&!p->shift&&p->payload_seen&&
        (p->top_seen&(1u<<6))&&(p->inner_seen&(1u<<1))&&
        !p->status&&(!p->error_code||p->error_code==10000)&&!p->handle_action&&flush(p);
}

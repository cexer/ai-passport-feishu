#include "feishu_json.h"
#include <limits.h>
#include <string.h>

static jsonsl_t lexer(feishu_json_t *p) { return (jsonsl_t)(void *)p->work.bytes; }
static bool fail(feishu_json_t *p)
{ p->failed=true;jsonsl_stop(lexer(p));return false; }
static bool event(feishu_json_t *p,feishu_json_event_t type,unsigned depth,const uint8_t *data,size_t size)
{
    if(p->failed) return false;
    if(p->callback && !p->callback(p->context,type,depth,data,size)) return fail(p);
    return true;
}
static void small_append(feishu_json_t *p,const uint8_t *data,size_t size)
{
    if(p->small_overflow) return;
    if(size>sizeof(p->small)-p->small_size) p->small_overflow=true;
    else {memcpy(p->small+p->small_size,data,size);p->small_size+=size;}
}
static bool string_bytes(feishu_json_t *p,const uint8_t *data,size_t size)
{
    if(p->key_active) {small_append(p,data,size);return true;}
    return event(p,FEISHU_JSON_STRING_PART,p->string_depth,data,size);
}
static bool codepoint(feishu_json_t *p,uint32_t value)
{
    if(value>0x10ffff || (value>=0xd800 && value<=0xdfff))return fail(p);
    uint8_t out[4];size_t size;
    if(value<=0x7f) {out[0]=(uint8_t)value;size=1;}
    else if(value<=0x7ff) {out[0]=0xc0|(value>>6);out[1]=0x80|(value&63);size=2;}
    else if(value<=0xffff && (value<0xd800 || value>0xdfff)) {
        out[0]=0xe0|(value>>12);out[1]=0x80|((value>>6)&63);out[2]=0x80|(value&63);size=3;
    } else if(value<=0x10ffff) {
        out[0]=0xf0|(value>>18);out[1]=0x80|((value>>12)&63);out[2]=0x80|((value>>6)&63);out[3]=0x80|(value&63);size=4;
    } else return fail(p);
    return string_bytes(p,out,size);
}

/* Decode a scalar at a time, across any HTTP boundary. Surrogate halves and
 * malformed UTF-8 are rejected before they can become a displayed C string. */
static bool string_byte(feishu_json_t *p,uint8_t ch)
{
    if(p->escaped==3) {if(ch!='\\')return fail(p);p->escaped=4;return true;}
    if(p->escaped==4) {if(ch!='u')return fail(p);p->escaped=5;p->hex_count=0;p->unicode=0;return true;}
    if(p->escaped==2 || p->escaped==5) {
        unsigned digit;
        if(ch>='0' && ch<='9')digit=ch-'0';
        else if(ch>='a' && ch<='f')digit=ch-'a'+10;
        else if(ch>='A' && ch<='F')digit=ch-'A'+10;
        else return fail(p);
        p->unicode=(p->unicode<<4)|digit;
        if(++p->hex_count!=4)return true;
        uint32_t value=p->unicode;
        if(p->escaped==5) {
            if(value<0xdc00 || value>0xdfff)return fail(p);
            value=0x10000+(((uint32_t)p->high_surrogate-0xd800)<<10)+(value-0xdc00);
            p->high_surrogate=0;
        } else if(value>=0xd800 && value<=0xdbff) {
            p->high_surrogate=(uint16_t)value;p->escaped=3;return true;
        } else if(value>=0xdc00 && value<=0xdfff)return fail(p);
        p->escaped=0;
        return codepoint(p,value);
    }
    if(p->escaped==1) {
        p->escaped=0;
        switch(ch) {
        case 'u':p->escaped=2;p->hex_count=0;p->unicode=0;return true;
        case '"':case '\\':case '/':break;
        case 'b':ch='\b';break;case 'f':ch='\f';break;
        case 'n':ch='\n';break;case 'r':ch='\r';break;case 't':ch='\t';break;
        default:return fail(p);
        }
        return string_bytes(p,&ch,1);
    }
    if(p->utf8_need) {
        if(ch<0x80 || ch>0xbf)return fail(p);
        p->utf8[p->utf8_size++]=ch;
        p->utf8_value=(p->utf8_value<<6)|(ch&63);
        if(--p->utf8_need)return true;
        uint32_t value=p->utf8_value;
        if(value<p->utf8_min || value>0x10ffff || (value>=0xd800 && value<=0xdfff))return fail(p);
        return string_bytes(p,p->utf8,p->utf8_size);
    }
    if(ch=='\\') {p->escaped=1;return true;}
    if(ch<0x20)return fail(p);
    if(ch<0x80)return string_bytes(p,&ch,1);
    if(ch>=0xc2 && ch<=0xdf) {p->utf8_need=1;p->utf8_value=ch&31;p->utf8_min=0x80;}
    else if(ch>=0xe0 && ch<=0xef) {p->utf8_need=2;p->utf8_value=ch&15;p->utf8_min=0x800;}
    else if(ch>=0xf0 && ch<=0xf4) {p->utf8_need=3;p->utf8_value=ch&7;p->utf8_min=0x10000;}
    else return fail(p);
    p->utf8[0]=ch;p->utf8_size=1;
    return true;
}

static int lexical_error(jsonsl_t jsn,jsonsl_error_t error,struct jsonsl_state_st *state,jsonsl_char_t *at)
{ (void)error;(void)state;(void)at;fail(jsn->data);return 0; }
static void action(jsonsl_t jsn,jsonsl_action_t kind,struct jsonsl_state_st *state,const jsonsl_char_t *at)
{
    feishu_json_t *p=jsn->data;
    if(p->failed)return;
    bool push=kind==JSONSL_ACTION_PUSH;
    if(state->type==JSONSL_T_OBJECT || state->type==JSONSL_T_LIST) {
        event(p,state->type==JSONSL_T_OBJECT ?
            (push ? FEISHU_JSON_OBJECT_BEGIN : FEISHU_JSON_OBJECT_END) :
            (push ? FEISHU_JSON_ARRAY_BEGIN : FEISHU_JSON_ARRAY_END),state->level,NULL,0);
    } else if(state->type & JSONSL_Tf_STRINGY) {
        if(push) {
            p->string_active=true;p->key_active=state->type==JSONSL_T_HKEY;
            p->string_depth=state->level;p->small_size=0;p->small_overflow=false;
            p->escaped=p->utf8_need=p->utf8_size=0;p->high_surrogate=0;
            if(!p->key_active)event(p,FEISHU_JSON_STRING_BEGIN,state->level,NULL,0);
        } else {
            p->string_popped=true;p->string_active=false;
            if(p->escaped || p->utf8_need) {fail(p);return;}
            event(p,p->key_active ? FEISHU_JSON_KEY : FEISHU_JSON_STRING_END,state->level,
                  p->key_active && !p->small_overflow ? p->small : NULL,
                  p->key_active && !p->small_overflow ? p->small_size : 0);
        }
    } else if(state->type==JSONSL_T_SPECIAL) {
        if(push) {
            p->atom_active=true;p->atom_depth=state->level;p->small_size=0;p->small_overflow=false;
            small_append(p,(const uint8_t *)at,1);
        } else {
            p->atom_popped=true;p->atom_active=false;
            event(p,FEISHU_JSON_ATOM,state->level,p->small_overflow ? NULL : p->small,
                  p->small_overflow ? 0 : p->small_size);
        }
    }
    if(!push && state->level==1)p->complete=true;
}

void feishu_json_reset(feishu_json_t *p)
{
    feishu_json_callback_t callback=p->callback;void *context=p->context;
    memset(p,0,sizeof(*p));p->callback=callback;p->context=context;
    jsonsl_t jsn=lexer(p);
    jsn->levels_max=FEISHU_JSON_DEPTH;jsn->max_callback_level=UINT_MAX;
    for(unsigned i=0;i<FEISHU_JSON_DEPTH;++i)jsn->stack[i].level=i;
    jsonsl_reset(jsn);
    jsn->call_HKEY=jsn->call_STRING=jsn->call_OBJECT=jsn->call_LIST=jsn->call_SPECIAL=1;
    jsn->action_callback=action;jsn->error_callback=lexical_error;jsn->data=p;
}
void feishu_json_init(feishu_json_t *p,feishu_json_callback_t callback,void *context)
{ memset(p,0,sizeof(*p));p->callback=callback;p->context=context;feishu_json_reset(p); }
bool feishu_json_feed(feishu_json_t *p,const void *data,size_t size)
{
    if(!p || p->failed || (!data && size))return false;
    const uint8_t *bytes=data;
    for(size_t i=0;i<size;++i) {
        uint8_t ch=bytes[i];
        if(++p->bytes_seen>64u*1024u*1024u)return fail(p);
        if(p->complete) {
            if(ch!=' ' && ch!='\t' && ch!='\r' && ch!='\n')return fail(p);
            continue;
        }
        bool was_string=p->string_active,was_atom=p->atom_active;
        p->string_popped=p->atom_popped=false;
        jsonsl_feed(lexer(p),(const char *)&ch,1);
        if(p->failed)return false;
        if(was_string && !p->string_popped && !string_byte(p,ch))return false;
        if(was_atom && !p->atom_popped)small_append(p,&ch,1);
    }
    return !p->failed;
}
bool feishu_json_finish(feishu_json_t *p)
{
    if(!p || p->failed)return false;
    if(!p->complete && p->atom_active)jsonsl_feed(lexer(p)," ",1);
    return !p->failed && p->complete && lexer(p)->level==0 && !p->string_active;
}

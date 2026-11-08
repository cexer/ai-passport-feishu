#include "feishu_outgoing_mentions.h"
#include "feishu_member_search.h"
#include <string.h>
#include <stdio.h>
#include <limits.h>
enum {R_ROOT=1,R_DATA,R_ITEMS,R_ITEM};
enum {K_CODE=1,K_DATA,K_ITEMS,K_ID,K_NAME,K_TOKEN,K_MORE};
static bool equal(const uint8_t *b,size_t n,const char *v){return n==strlen(v) && !memcmp(b,v,n);}
static unsigned key(const uint8_t *b,size_t n){
    if(equal(b,n,"code"))return K_CODE;
    if(equal(b,n,"data"))return K_DATA;
    if(equal(b,n,"items"))return K_ITEMS;
    if(equal(b,n,"member_id") || equal(b,n,"bot_id") || equal(b,n,"open_id"))return K_ID;
    if(equal(b,n,"name") || equal(b,n,"bot_name"))return K_NAME;
    if(equal(b,n,"page_token"))return K_TOKEN;
    if(equal(b,n,"has_more"))return K_MORE;
    return 0;
}
static unsigned fold(unsigned c){return c>='A' && c<='Z' ? c+32:c;}
static bool same(const char *a,const char *b){while(*a && *b){if(fold((unsigned char)*a++)!=fold((unsigned char)*b++))return false;}return *a==*b;}
static bool contains(const char *a,const char *b){
    for(;*a;++a){const char *x=a,*y=b;while(*x && *y && fold((unsigned char)*x)==fold((unsigned char)*y)){++x;++y;}if(!*y)return true;}return false;
}
static bool identifier(const char *id){
    if(!strcmp(id,"all"))return true;
    if(strncmp(id,"ou_",3) || !id[3] || strlen(id)>63)return false;
    for(const unsigned char *p=(const unsigned char *)id;*p;++p)
        if(!((*p>='a'&&*p<='z') || (*p>='A'&&*p<='Z') || (*p>='0'&&*p<='9') || *p=='_'))return false;
    return true;
}
static void candidate(feishu_mention_t *out,unsigned *count,const char *id,const char *name){
    if(*count && !strcmp(out->id,id))return;
    if(*count<2)++*count;
    if(*count==1){snprintf(out->id,sizeof(out->id),"%s",id);
        size_t n=strlen(name);if(n>=sizeof(out->name))n=sizeof(out->name)-1;
        while(n && ((unsigned char)name[n]&0xc0)==0x80)--n;
        memcpy(out->name,name,n);out->name[n]=0;}
}
static bool event(void *context,feishu_json_event_t ev,unsigned d,const uint8_t *b,size_t n){
    feishu_member_search_t *s=context;if(d>FEISHU_JSON_DEPTH)return false;
    if(ev==FEISHU_JSON_KEY){s->key[d]=(unsigned char)key(b,n);return true;}
    if(ev==FEISHU_JSON_OBJECT_BEGIN){
        s->role[d]=0;
        if(d==1)s->role[d]=R_ROOT;
        else if(s->role[d-1]==R_ROOT && s->key[d]==K_DATA){if(s->data_seen)return false;s->role[d]=R_DATA;s->data_seen=true;}
        else if(s->role[d-1]==R_ITEMS){s->role[d]=R_ITEM;s->id[0]=s->name[0]=0;s->seen=0;}
    }
    if(ev==FEISHU_JSON_ARRAY_BEGIN){s->role[d]=0;if(d && s->role[d-1]==R_DATA && s->key[d]==K_ITEMS){if(s->items_seen)return false;s->role[d]=R_ITEMS;s->items_seen=true;}}
    if(ev==FEISHU_JSON_OBJECT_END && s->role[d]==R_ITEM){
        if(s->overflow || !identifier(s->id) || !s->name[0])return false;
        if(same(s->name,s->query))candidate(&s->exact,&s->exact_count,s->id,s->name);
        else if(contains(s->name,s->query))candidate(&s->partial,&s->partial_count,s->id,s->name);
    }
    if(ev==FEISHU_JSON_STRING_BEGIN){
        s->selected=0;s->used=0;s->overflow=false;
        if(d && ((s->role[d-1]==R_ROOT && s->key[d]==K_CODE) ||
            (s->role[d-1]==R_DATA && s->key[d]==K_MORE)))return false;
        if(d && s->role[d-1]==R_ITEM && (s->key[d]==K_ID || s->key[d]==K_NAME)){
            s->selected=s->key[d];unsigned bit=1u<<s->selected;if(s->seen&bit)return false;s->seen|=bit;
        } else if(d && s->role[d-1]==R_DATA && s->key[d]==K_TOKEN)s->selected=K_TOKEN;
    }
    if(ev==FEISHU_JSON_STRING_PART && s->selected){
        char *out=s->selected==K_ID ? s->id:s->selected==K_NAME ? s->name:s->token;
        size_t cap=s->selected==K_ID ? sizeof(s->id):s->selected==K_NAME ? sizeof(s->name):sizeof(s->token);
        if(memchr(b,0,n) || n>=cap-s->used){s->overflow=true;return false;}
        memcpy(out+s->used,b,n);s->used+=n;out[s->used]=0;
    }
    if(ev==FEISHU_JSON_STRING_END)s->selected=0;
    if(ev==FEISHU_JSON_ATOM && d && s->role[d-1]==R_ROOT && s->key[d]==K_CODE){
        if(s->code_seen || !n || n>10)return false;
        int code=0;for(size_t i=0;i<n;++i){if(b[i]<'0'||b[i]>'9' || code>(INT_MAX-(b[i]-'0'))/10)return false;code=code*10+b[i]-'0';}
        s->code=code;s->code_seen=true;
    }
    if(ev==FEISHU_JSON_ATOM && d && s->role[d-1]==R_DATA && s->key[d]==K_MORE){
        if(!equal(b,n,"true") && !equal(b,n,"false"))return false;
        s->more=equal(b,n,"true");
    }
    return true;
}
bool feishu_member_search_init(feishu_member_search_t *s,const char *q){
    if(!s || !q || !*q || strlen(q)>=sizeof(s->query))return false;
    memset(s,0,sizeof(*s));strcpy(s->query,q);feishu_member_search_page(s);return true;
}
void feishu_member_search_page(feishu_member_search_t *s){
    memset(s->role,0,sizeof(s->role));memset(s->key,0,sizeof(s->key));s->token[0]=s->id[0]=s->name[0]=0;
    s->code=-1;s->code_seen=s->more=s->data_seen=s->items_seen=s->overflow=false;s->selected=s->seen=0;s->used=0;
    feishu_json_init(&s->json,event,s);
}
bool feishu_member_search_feed(feishu_member_search_t *s,const void *b,size_t n){return s && feishu_json_feed(&s->json,b,n);}
bool feishu_member_search_finish(feishu_member_search_t *s){return s && feishu_json_finish(&s->json) && s->code_seen && !s->code && s->data_seen && s->items_seen && (!s->more || s->token[0]);}
bool feishu_member_search_result(const feishu_member_search_t *s,feishu_mention_t *out){
    if(!s || !out)return false;
    if(s->exact_count==1){*out=s->exact;return true;}
    if(!s->exact_count && s->partial_count==1){*out=s->partial;return true;}return false;
}
bool feishu_mention_plain_at(const char *s){
    if(!s)return false;
    for(size_t i=0;s[i];++i)if(s[i]=='@' && s[i+1] && s[i+1]!=' ' && s[i+1]!='\n'){
        unsigned char c=i ? (unsigned char)s[i-1]:0;
        if(!((c>='0'&&c<='9') || (c>='A'&&c<='Z') || (c>='a'&&c<='z') || c=='_' || c=='.' || c=='-'))return true;
    }
    return false;
}
static bool add(char *out,size_t cap,size_t *at,const char *b,size_t n){if(n>=cap-*at)return false;memcpy(out+*at,b,n);*at+=n;out[*at]=0;return true;}
bool feishu_mentions_compose(const char *text,const feishu_mention_t *m,size_t count,char *out,size_t cap){
    if(!text || !out || !cap || count>FEISHU_MENTION_MAX || (count&&!m))return false;
    out[0]=0;if(feishu_mention_plain_at(text) || strstr(text,"<at"))return false;
    size_t at=0;
    for(size_t i=0;i<count;++i){
        if(!identifier(m[i].id) || !m[i].name[0])return false;
        for(size_t j=0;j<i;++j)if(!strcmp(m[i].id,m[j].id))return false;
        if(!add(out,cap,&at,"<at user_id=\"",13) || !add(out,cap,&at,m[i].id,strlen(m[i].id)) || !add(out,cap,&at,"\">",2))return false;
        for(const char *p=m[i].name;*p;++p){const char *e=*p=='&' ? "&amp;":*p=='<' ? "&lt;":*p=='>' ? "&gt;":NULL;if(!add(out,cap,&at,e?e:p,e?strlen(e):1))return false;}
        if(!add(out,cap,&at,"</at> ",6))return false;
    }
    return add(out,cap,&at,text,strlen(text));
}
static size_t scalar(const char *p){
    unsigned char c=(unsigned char)*p;
    if(c<0x80)return c ? 1:0;
    size_t n=c>=0xf0 ? 4:c>=0xe0 ? 3:c>=0xc2 ? 2:0;
    for(size_t i=1;i<n;++i)if(((unsigned char)p[i]&0xc0)!=0x80)return 0;
    return n;
}
bool feishu_mentions_split(const char *text,feishu_mention_t m[FEISHU_MENTION_MAX],size_t *count,const char **body){
    if(!text || !m || !count || !body)return false;
    *count=0;
    while(!strncmp(text,"<at user_id=\"",13)){
        if(*count>=FEISHU_MENTION_MAX)return false;
        const char *id=text+13,*quote=strchr(id,'"');
        if(!quote || quote[1]!='>' || quote-id>=64 || quote==id)return false;
        feishu_mention_t *out=&m[*count];memset(out,0,sizeof(*out));
        memcpy(out->id,id,(size_t)(quote-id));if(!identifier(out->id))return false;
        const char *p=quote+2,*end=strstr(p,"</at> ");if(!end)return false;
        size_t at=0;
        while(p<end){
            const char *entities[]={"&amp;","&lt;","&gt;"};const char values[]="&<>";bool decoded=false;
            for(unsigned i=0;i<3;++i)if(!strncmp(p,entities[i],strlen(entities[i]))){
                if(at+1>=sizeof(out->name))return false;
                out->name[at++]=values[i];p+=strlen(entities[i]);decoded=true;break;
            }
            if(!decoded){size_t n=scalar(p);if(!n || n>=sizeof(out->name)-at || n>(size_t)(end-p))return false;
                memcpy(out->name+at,p,n);at+=n;p+=n;}
        }
        out->name[at]=0;if(!at)return false;
        ++*count;text=end+6;
    }
    *body=text;return true;
}
void feishu_mentions_display(const char *s,char *out,size_t cap){
    if(!out || !cap)return;
    size_t at=0;out[0]=0;if(!s)return;
    while(*s && at+1<cap){
        if(!strncmp(s,"<at user_id=\"",13)){
            const char *begin=strchr(s,'>'),*end=begin?strstr(begin+1,"</at>"):NULL;
            if(begin && end){out[at++]='@';s=begin+1;while(s<end){
                const char *names[]={"&amp;","&lt;","&gt;"};const char values[]="&<>";bool decoded=false;
                for(unsigned i=0;i<3;++i)if(!strncmp(s,names[i],strlen(names[i]))){
                    if(at+1>=cap)goto done;
                    out[at++]=values[i];s+=strlen(names[i]);decoded=true;break;
                }
                if(!decoded){size_t n=scalar(s);if(!n || n>=cap-at || n>(size_t)(end-s))goto done;memcpy(out+at,s,n);s+=n;at+=n;}
            }s=end+5;continue;}
            if(!end){out[at++]='@';break;}
        }
        size_t n=scalar(s);if(!n || n>=cap-at)break;memcpy(out+at,s,n);s+=n;at+=n;
    }
done:
    out[at]=0;
}

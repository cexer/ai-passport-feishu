#include "feishu_page.h"
#include <stdio.h>
#include <string.h>

bool feishu_page_token_valid(const char *token)
{
    if(!token)return false;
    size_t n=0;
    while(token[n]) {
        unsigned char c=(unsigned char)token[n];
        if(n>=FEISHU_PAGE_TOKEN_MAX || c<33 || c>126)return false;
        ++n;
    }
    return true;
}
bool feishu_page_valid(const feishu_page_cursor_t *p,size_t capacity)
{
    return p && p->version==1 && p->count<=capacity &&
        memchr(p->token,0,sizeof(p->token)) && memchr(p->next,0,sizeof(p->next)) &&
        memchr(p->previous,0,sizeof(p->previous)) && feishu_page_token_valid(p->token) &&
        feishu_page_token_valid(p->next) && feishu_page_token_valid(p->previous) &&
        (!p->more || (p->next[0] && strcmp(p->token,p->next))) &&
        (!p->previous_known || p->number>0);
}
bool feishu_page_response(feishu_page_cursor_t *p,const char *token,const char *next,bool more,size_t count)
{
    if(!p || !feishu_page_token_valid(token) || !feishu_page_token_valid(next) || count>UINT32_MAX ||
       (more && (!*next || !strcmp(token,next))))return false;
    char current[FEISHU_PAGE_TOKEN_MAX+1],following[FEISHU_PAGE_TOKEN_MAX+1];
    strcpy(current,token);strcpy(following,next); /* Inputs may alias the old page. */
    memset(p,0,sizeof(*p));p->version=1;p->count=(uint32_t)count;p->more=more;
    strcpy(p->token,current);if(more)strcpy(p->next,following);
    return true;
}
static bool unreserved(unsigned char c)
{return (c>='A' && c<='Z') || (c>='a' && c<='z') || (c>='0' && c<='9') || c=='-' || c=='_' || c=='.' || c=='~';}
bool feishu_page_url(const char *base,const char *token,char *out,size_t capacity)
{
    if(!base || !out || !feishu_page_token_valid(token))return false;
    size_t n=strlen(base),needed=n;
    if(*token) {
        needed+=12; /* &page_token= */
        for(const unsigned char *p=(const unsigned char *)token;*p;++p)needed+=unreserved(*p) ? 1:3;
    }
    if(needed>=capacity)return false;
    memcpy(out,base,n);
    if(*token) {
        static const char hex[]="0123456789ABCDEF";
        memcpy(out+n,"&page_token=",12);n+=12;
        for(const unsigned char *p=(const unsigned char *)token;*p;++p) {
            if(unreserved(*p))out[n++]=(char)*p;
            else {out[n++]='%';out[n++]=hex[*p>>4];out[n++]=hex[*p&15];}
        }
    }
    out[n]=0;return true;
}
void feishu_page_key(const char *kind,const char *scope,const char *token,char output[64])
{
    uint64_t hash=UINT64_C(14695981039346656037);
    const char *parts[]={kind,scope,token};
    for(unsigned i=0;i<3;++i) {
        for(const unsigned char *p=(const unsigned char *)parts[i];*p;++p) {hash^=*p;hash*=UINT64_C(1099511628211);}
        hash^=0;hash*=UINT64_C(1099511628211);
    }
    snprintf(output,64,"page/%s/%016llx",kind,(unsigned long long)hash);
}

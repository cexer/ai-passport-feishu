#include "feishu_page.h"
#include <assert.h>
#include <string.h>

int main(void)
{
    feishu_page_cursor_t p={0};
    assert(feishu_page_response(&p,"","a+b/=",true,10));
    assert(feishu_page_valid(&p,10));
    assert(feishu_page_response(&p,p.next,"tail",true,10));
    assert(!strcmp(p.token,"a+b/="));
    assert(feishu_page_response(&p,"","a+b/=",true,10));
    char url[128]="guard";
    assert(feishu_page_url("https://example.invalid/?page_size=10",p.next,url,sizeof(url)));
    assert(!strcmp(url,"https://example.invalid/?page_size=10&page_token=a%2Bb%2F%3D"));
    assert(!feishu_page_url("http://x/?q=1","a+b",url,10));
    assert(!strcmp(url,"https://example.invalid/?page_size=10&page_token=a%2Bb%2F%3D"));
    feishu_page_cursor_t saved=p;
    assert(!feishu_page_response(&p,"a","a",true,10));
    assert(!memcmp(&p,&saved,sizeof(p)));
    assert(!feishu_page_response(&p,"a","",true,10));
    assert(!feishu_page_response(&p,"","bad\nvalue",true,10));
    char large[FEISHU_PAGE_TOKEN_MAX+2];memset(large,'X',sizeof(large));large[sizeof(large)-1]=0;
    assert(!feishu_page_response(&p,"",large,true,10));
    assert(!memcmp(&p,&saved,sizeof(p)));
    large[FEISHU_PAGE_TOKEN_MAX]=0;
    assert(feishu_page_response(&p,"",large,true,10));
    assert(feishu_page_valid(&p,10));
    assert(!feishu_page_valid(&p,9));
    p.previous_known=true;assert(!feishu_page_valid(&p,10));
    p.number=1;assert(feishu_page_valid(&p,10));
    char a[64],b[64],c[64];
    feishu_page_key("chats","owner","a+b/=",a);
    feishu_page_key("chats","other","a+b/=",b);
    feishu_page_key("chats","owner","a+b/==",c);
    assert(strcmp(a,b) && strcmp(a,c) && strcmp(b,c));
    assert(feishu_page_response(&p,"","",false,0) && feishu_page_valid(&p,10));
    return 0;
}

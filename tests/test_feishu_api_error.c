#include "feishu_api_error.h"
#include "feishu_auth_policy.h"
#include <assert.h>
#include <stdio.h>
static bool parse(feishu_api_error_t *p,const char *s)
{feishu_api_error_init(p);for(size_t i=0;i<strlen(s);++i)if(!feishu_api_error_feed(p,s+i,1))return false;return feishu_api_error_finish(p);}
int main(void){
 feishu_api_error_t p;
 assert(parse(&p,"{\"code\":99991663,\"msg\":\"Invalid token\"}") && feishu_auth_access_rejected(400,p.code));
 assert(parse(&p,"{\"data\":{\"code\":99991663},\"code\":232033}") && !feishu_auth_access_rejected(403,p.code));
 assert(!parse(&p,"{\"code\":99991663,\"code\":0}"));
 assert(!parse(&p,"{\"code\":\"99991663\"}"));
 assert(!parse(&p,"{\"code\":-1}"));assert(!parse(&p,"{\"code\":2147483648}"));
 assert(!parse(&p,"{\"data\":{\"code\":99991663}}"));
 assert(!parse(&p,"{\"code\":99991663"));assert(!parse(&p,"[1,2]"));
 feishu_api_error_init(&p);char large[4097];memset(large,' ',sizeof(large));assert(!feishu_api_error_feed(&p,large,sizeof(large)));
 assert(parse(&p,"{\"msg\":\"recovered\",\"code\":99991664}") && feishu_auth_access_rejected(400,p.code));
 printf("API errors: fragmented root code, HTTP400 expiry, nested/duplicate/type/overflow/truncation/size and recovery PASS; workspace=%zu\n",sizeof(p));
}

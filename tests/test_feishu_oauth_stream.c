#include "feishu_oauth_stream.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    char access[8192],refresh[8192],error[64];
    feishu_oauth_stream_t p;
    const char *good="{\"access_token\":\"user-access\",\"refresh_token\":\"user-refresh\",\"expires_in\":7200}";
    feishu_oauth_stream_init(&p,access,sizeof(access),refresh,sizeof(refresh),error,sizeof(error));
    for(size_t i=0;i<strlen(good);++i)assert(feishu_oauth_stream_feed(&p,good+i,1));
    assert(feishu_oauth_stream_finish(&p));
    assert(!strcmp(access,"user-access") && !strcmp(refresh,"user-refresh"));
    const char *nested="{\"debug\":{\"error\":\"invalid_grant\"},\"error\":\"invalid_scope\"}";
    feishu_oauth_stream_init(&p,access,sizeof(access),refresh,sizeof(refresh),error,sizeof(error));
    assert(feishu_oauth_stream_feed(&p,nested,strlen(nested)) && feishu_oauth_stream_finish(&p));
    assert(!strcmp(error,"invalid_scope"));
    const char *truncated="{\"error\":\"invalid_grant\"";
    feishu_oauth_stream_init(&p,access,sizeof(access),refresh,sizeof(refresh),error,sizeof(error));
    assert(feishu_oauth_stream_feed(&p,truncated,strlen(truncated)));
    assert(!feishu_oauth_stream_finish(&p));
    const char *duplicate="{\"error\":\"invalid_grant\",\"error\":\"invalid_scope\"}";
    feishu_oauth_stream_init(&p,access,sizeof(access),refresh,sizeof(refresh),error,sizeof(error));
    assert(!feishu_oauth_stream_feed(&p,duplicate,strlen(duplicate)));
    /* Large opaque tokens are copied into caller-owned buffers in fixed chunks. */
    feishu_oauth_stream_init(&p,access,sizeof(access),refresh,sizeof(refresh),error,sizeof(error));
    assert(feishu_oauth_stream_feed(&p,"{\"access_token\":\"",17));
    for(unsigned i=0;i<7500;++i)assert(feishu_oauth_stream_feed(&p,"A",1));
    assert(feishu_oauth_stream_feed(&p,"\",\"refresh_token\":\"",19));
    for(unsigned i=0;i<7500;++i)assert(feishu_oauth_stream_feed(&p,"R",1));
    assert(feishu_oauth_stream_feed(&p,"\"}",2) && feishu_oauth_stream_finish(&p));
    assert(strlen(access)==7500 && strlen(refresh)==7500);
    feishu_oauth_stream_init(&p,access,4,refresh,sizeof(refresh),error,sizeof(error));
    assert(!feishu_oauth_stream_feed(&p,good,strlen(good)) && p.overflow);
    puts("OAuth streaming envelope: PASS");
    return 0;
}

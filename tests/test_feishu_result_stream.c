#include "feishu_result_stream.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static size_t stored,maximum;
static bool fail_sink;
static bool sink(void *unused,const void *p,size_t n)
{ (void)unused;assert(p && n<=1024);if(fail_sink)return false;stored+=n;if(n>maximum)maximum=n;return true; }
static bool parse(const char *json,char *preview,size_t size)
{
    feishu_result_stream_t parser;feishu_result_stream_init(&parser,"recognition_text",300*1024,preview,size,sink,NULL);
    stored=0;for(size_t i=0;i<strlen(json);++i)if(!feishu_result_stream_feed(&parser,json+i,1))return false;
    return feishu_result_stream_finish(&parser);
}
int main(void)
{
    char preview[512];
    assert(parse("{\"data\":{\"recognition_text\":\"hi\\n\\u4e2d\\ud83d\\ude00\"},\"code\":0}",preview,sizeof(preview)));
    assert(!strcmp(preview,"hi\n\xe4\xb8\xad\xf0\x9f\x98\x80") && stored==10);
    assert(!parse("{\"code\":0,\"debug\":{\"recognition_text\":\"wrong\"},\"data\":{}}",preview,sizeof(preview)));
    assert(!parse("{\"code\":0,\"data\":{\"recognition_text\":\"x\"},\"code\":0}",preview,sizeof(preview)));
    assert(!parse("{\"code\":0,\"data\":{\"recognition_text\":\"x\",\"recognition_text\":\"y\"}}",preview,sizeof(preview)));
    assert(!parse("{\"code\":0,\"data\":{\"recognition_text\":\"x\"}",preview,sizeof(preview)));
    assert(!parse("{\"code\":0,\"data\":{\"recognition_text\":\"\\u0000\"}}",preview,sizeof(preview)));
    assert(!parse("{\"data\":{\"recognition_text\":\"looks-valid\"},\"code\":999}",preview,sizeof(preview)));
    assert(!parse("{\"code\":\"0\",\"data\":{\"recognition_text\":\"wrong-type\"}}",preview,sizeof(preview)));
    fail_sink=true;assert(!parse("{\"code\":0,\"data\":{\"recognition_text\":\"disk full\"}}",preview,sizeof(preview)));fail_sink=false;
    assert(parse("{\"code\":0,\"data\":{\"recognition_text\":\"recovered\"}}",preview,sizeof(preview)));
    feishu_result_stream_t p;stored=0;
    feishu_result_stream_init(&p,"recognition_text",300*1024,preview,sizeof(preview),sink,NULL);
    const char *head="{\"data\":{\"recognition_text\":\"";assert(feishu_result_stream_feed(&p,head,strlen(head)));
    char block[1024];memset(block,'a',sizeof(block));
    for(unsigned i=0;i<300;++i)assert(feishu_result_stream_feed(&p,block,sizeof(block)));
    const char *tail="\"},\"code\":0}";assert(feishu_result_stream_feed(&p,tail,strlen(tail)));
    assert(feishu_result_stream_finish(&p) && stored==300*1024 && maximum==1024 && strlen(preview)==511);
    feishu_result_stream_init(&p,"recognition_text",3,NULL,0,NULL,NULL);
    assert(feishu_result_stream_feed(&p,head,strlen(head)));
    assert(!feishu_result_stream_feed(&p,"four",4));
    printf("result envelope: normal, 300 KiB, late errors and storage recovery PASS; fixed %zu bytes\n",sizeof(p));
    return 0;
}

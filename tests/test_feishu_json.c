#include "feishu_json.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct { uint8_t text[128];size_t size,total;unsigned atoms,keys;bool collect,stop; } sink_t;
static bool receive(void *context,feishu_json_event_t event,unsigned depth,const uint8_t *data,size_t size)
{
    (void)depth;
    sink_t *s=context;
    if(event==FEISHU_JSON_KEY) ++s->keys;
    if(event==FEISHU_JSON_ATOM) ++s->atoms;
    if(event==FEISHU_JSON_STRING_PART) {
        s->total+=size;
        if(s->collect) {
            assert(size<=sizeof(s->text)-s->size);
            memcpy(s->text+s->size,data,size);s->size+=size;
        }
        if(s->stop && s->total>100)return false;
    }
    return true;
}

static void reject_then_recover(feishu_json_t *p,const void *data,size_t size)
{
    feishu_json_reset(p);
    bool fed=feishu_json_feed(p,data,size);
    assert(!fed || !feishu_json_finish(p));
    feishu_json_reset(p);
    assert(feishu_json_feed(p,"{}",2) && feishu_json_finish(p));
}

int main(void)
{
    feishu_json_t parser;
    sink_t sink={0};
    const char input[]="{\"text\":\"A\\n\\u4e2d\\ud83d\\ude00\",\"n\":-12.5e+2,\"b\":true,\"v\":[null,false]}";
    const uint8_t expected[]={'A','\n',0xe4,0xb8,0xad,0xf0,0x9f,0x98,0x80};
    for(size_t chunk=1;chunk<=sizeof(input);++chunk) {
        memset(&sink,0,sizeof(sink));sink.collect=true;
        feishu_json_init(&parser,receive,&sink);
        for(size_t pos=0;pos<strlen(input);) {
            size_t n=strlen(input)-pos;if(n>chunk)n=chunk;
            assert(feishu_json_feed(&parser,input+pos,n));pos+=n;
        }
        assert(feishu_json_finish(&parser));
        assert(sink.size==sizeof(expected) && !memcmp(sink.text,expected,sizeof(expected)));
        assert(sink.keys==4 && sink.atoms==4);
    }
    const char *bad[]={"{\"x\":\"\\ud800\"}","{\"x\":\"\\udc00\"}","{\"x\":\"\\ud800X\"}",
        "{\"x\":\"\\uqqqq\"}","{\"x\":\"unterminated","[1,,2]","{\"a\":1]","{}{}","{\"x\":01}",""};
    for(size_t i=0;i<sizeof(bad)/sizeof(bad[0]);++i)reject_then_recover(&parser,bad[i],strlen(bad[i]));
    const uint8_t invalid_utf8[]={'"',0xed,0xa0,0x80,'"'};
    reject_then_recover(&parser,invalid_utf8,sizeof(invalid_utf8));
    char deep[80];memset(deep,'[',sizeof(deep));
    reject_then_recover(&parser,deep,sizeof(deep));
    /* A multi-megabyte string is never materialized or allocated by the SAX layer. */
    memset(&sink,0,sizeof(sink));feishu_json_init(&parser,receive,&sink);
    assert(feishu_json_feed(&parser,"{\"text\":\"",9));
    char block[256];memset(block,'x',sizeof(block));
    for(unsigned i=0;i<8192;++i)assert(feishu_json_feed(&parser,block,sizeof(block)));
    assert(feishu_json_feed(&parser,"\"}",2) && feishu_json_finish(&parser));
    assert(sink.total==2*1024*1024);
    sink.stop=true;sink.total=0;feishu_json_reset(&parser);
    assert(feishu_json_feed(&parser,"{\"text\":\"",9));
    assert(!feishu_json_feed(&parser,block,sizeof(block)));
    sink.stop=false;feishu_json_reset(&parser);
    assert(feishu_json_feed(&parser,"{\"ok\":true}",11) && feishu_json_finish(&parser));
    printf("streaming JSON: PASS; fixed parser workspace %zu bytes\n",sizeof(parser));
    return 0;
}

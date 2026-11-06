#include "feishu_xiaozhi_protocol.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
static void frames(void)
{
    const size_t sizes[]={0,1,125,126,640,656,65535};
    uint8_t key[]={0x01,0xa7,0x00,0xff};
    for(unsigned k=0;k<sizeof(sizes)/sizeof(sizes[0]);++k) {
        size_t n=sizes[k],header=n<126?6:8;
        uint8_t *b=malloc(n+header);assert(b);
        for(size_t i=0;i<n;++i)b[i]=(uint8_t)i;
        assert(feishu_xiao_binary_frame(b,n+header,n,key)==n+header);
        assert(b[0]==0x82 && b[1]==(uint8_t)(0x80|(n<126?n:126)));
        if(n>=126)assert(b[2]==n>>8 && b[3]==(n&255));
        assert(!memcmp(b+header-4,key,4));
        for(size_t i=0;i<n;++i)assert((b[header+i]^key[i%4])==(uint8_t)i);
        free(b);
    }
    uint8_t b[20],before[20];memset(b,0x37,sizeof(b));memcpy(before,b,sizeof(b));
    assert(!feishu_xiao_binary_frame(b,sizeof(b),15,key) && !memcmp(b,before,sizeof(b)));
    assert(!feishu_xiao_binary_frame(b,sizeof(b),65536,key) && !memcmp(b,before,sizeof(b)));
    assert(!feishu_xiao_binary_frame(b,sizeof(b),1,NULL) && !memcmp(b,before,sizeof(b)));
    assert(!feishu_xiao_binary_frame(NULL,sizeof(b),1,key));
    memcpy(b,key,4);assert(feishu_xiao_binary_frame(b,sizeof(b),4,b)==10);
    for(unsigned i=0;i<4;++i)assert((b[6+i]^key[i])==key[i]);
}
static int parse(const char *s,unsigned stride,feishu_xiao_message_t *p)
{
    feishu_xiao_message_init(p);size_t n=strlen(s);
    for(size_t i=0;i<n;i+=stride)if(!feishu_xiao_message_feed(p,s+i,n-i<stride?n-i:stride))return -1;
    return feishu_xiao_message_finish(p);
}
int main(void)
{
    char code[17];
    assert(feishu_xiao_pairing_classify(true,"123456",true,code)==1 && !strcmp(code,"123456"));
    assert(feishu_xiao_pairing_classify(false,NULL,true,code)==0 && !*code);
    assert(feishu_xiao_pairing_classify(false,NULL,false,code)==-1);
    assert(feishu_xiao_pairing_classify(true,NULL,false,code)==1 && !*code);
    assert(feishu_xiao_pairing_classify(true,"12345678901234567",true,code)==-1 && !*code);
    assert(feishu_xiao_pairing_classify(true,"12x456",true,code)==-1 && !*code);
    assert(feishu_xiao_pairing_classify(true,"654321",false,code)==1 && !strcmp(code,"654321"));
    frames();
    feishu_xiao_message_t p;
    for(unsigned stride=1;stride<=80;++stride) {
        assert(parse("{\"type\":\"hello\",\"session_id\":\"abc\"}",stride,&p)==XIAO_MESSAGE_HELLO && !strcmp(p.session,"abc"));
        assert(parse("{\"text\":\"这是一条真机消息，今天天气不错。\",\"type\":\"stt\"}",stride,&p)==XIAO_MESSAGE_STT);
        assert(!strcmp(p.text,"这是一条真机消息，今天天气不错。"));
        assert(parse("{\"type\":\"stt\",\"text\":null}",stride,&p)==-1);
        assert(parse("{\"type\":\"stt\",\"text\":\"a\",\"text\":\"b\"}",stride,&p)==-1);
        assert(parse("{\"type\":\"stt\",\"text\":\"\\u0000bad\"}",stride,&p)==-1);
        assert(parse("{\"type\":\"stt\",\"text\":\"bad",stride,&p)==-1);
        assert(parse("{\"type\":\"alert\"}",stride,&p)==XIAO_MESSAGE_ALERT);
        assert(parse("{\"type\":\"tts\",\"state\":\"start\"}",stride,&p)==XIAO_MESSAGE_OTHER);
    }
    char too_long[XIAO_TEXT_CAP+80];memset(too_long,'x',sizeof(too_long));
    const char *head="{\"type\":\"stt\",\"text\":\"";memcpy(too_long,head,strlen(head));memcpy(too_long+sizeof(too_long)-3,"\"}",3);
    assert(parse(too_long,7,&p)==-1);
    assert(parse("{\"type\":\"stt\",\"text\":\"恢复正常\"}",1,&p)==XIAO_MESSAGE_STT);
    puts("XiaoZhi protocol: fragmented UTF8, independent text, duplicate/wrong/oversized fields and recovery PASS");return 0;
}

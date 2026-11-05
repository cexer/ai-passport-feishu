#include "feishu_registration_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static feishu_register_result_t r;
static bool decode(const char *json,bool beginning)
{ return feishu_register_decode(json,strlen(json),beginning,&r); }
static void bad(const char *json,bool beginning)
{
    memset(&r,0x55,sizeof(r));assert(!decode(json,beginning));
    assert(!r.device_code[0] && !r.verification_url[0] && !r.app_id[0] && !r.app_secret[0]);
}
int main(void)
{
    const char *begin="{\"device_code\":\"opaque+/=&\",\"verification_uri_complete\":\"https://open.feishu.cn/page/launcher?user_code=FAKE-TEST\",\"expires_in\":600,\"interval\":5}";
    assert(decode(begin,true) && r.status==FEISHU_REGISTER_PENDING && r.expires_in==600 && r.interval==5);
    assert(!strcmp(r.device_code,"opaque+/=&"));
    for(size_t i=0;i<strlen(begin);++i)assert(!feishu_register_decode(begin,i,true,&r));
    assert(decode("{\"client_id\":\"cli_fake123\",\"client_secret\":\"fake-secret\",\"user_info\":{\"tenant_brand\":\"feishu\"}}",false));
    assert(r.status==FEISHU_REGISTER_COMPLETE && !strcmp(r.app_secret,"fake-secret"));
    assert(decode("{\"client_id\":\"cli_fake123\",\"client_secret\":\"fake-secret\",\"user_info\":{\"tenant_brand\":\"lark\"}}",false));
    assert(r.status==FEISHU_REGISTER_UNSUPPORTED);
    const char *errors[]={"authorization_pending","slow_down","expired_token","access_denied","unknown_error"};
    feishu_register_status_t states[]={FEISHU_REGISTER_PENDING,FEISHU_REGISTER_SLOW,FEISHU_REGISTER_EXPIRED,FEISHU_REGISTER_DENIED,FEISHU_REGISTER_FAILED};
    char json[5000];
    for(unsigned i=0;i<5;++i) {
        snprintf(json,sizeof(json),"{\"error\":\"%s\"}",errors[i]);assert(decode(json,false) && r.status==states[i]);
    }
    const char *invalid[]={"[]","{}","{\"nested\":{\"client_id\":\"cli_fake\",\"client_secret\":\"fake\"}}",
        "{\"client_id\":\"cli_fake\",\"client_secret\":\"x\",\"client_secret\":\"y\"}",
        "{\"client_id\":\"cli_fake\",\"client_secret\":true}",
        "{\"client_id\":\"cli_fake\",\"client_secret\":{}}",
        "{\"client_id\":\"cli_fake\",\"client_secret\":\"x\\u0000y\"}",
        "{\"client_id\":\"bad\",\"client_secret\":\"x\"}",
        "{\"client_id\":\"cli_\",\"client_secret\":\"x\"}",
        "{\"client_id\":\"cli_fake\",\"client_secret\":\"\"}",
        "{\"client_id\":\"cli_fake\"}","{\"client_secret\":\"x\"}",
        "{\"error\":true}","{\"error\":{\"message\":\"authorization_pending\"}}"};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);++i)bad(invalid[i],false);
    bad("{\"device_code\":\"x\",\"verification_uri_complete\":\"https://open.feishu.cn.evil.invalid/test\"}",true);
    bad("{\"device_code\":\"x\",\"verification_uri_complete\":\"http://open.feishu.cn/test\"}",true);
    bad("{\"device_code\":\"x\",\"verification_uri_complete\":\"https://open.feishu.cn/test\",\"interval\":0}",true);
    bad("{\"device_code\":\"x\",\"verification_uri_complete\":\"https://open.feishu.cn/test\",\"interval\":61}",true);
    bad("{\"device_code\":\"x\",\"verification_uri_complete\":\"https://open.feishu.cn/test\",\"expires_in\":1.5}",true);
    memset(json,'a',4097);json[4097]=0;bad(json,false);
    static const char start[]="{\"client_id\":\"cli_fake\",\"client_secret\":\"";
    memcpy(json,start,sizeof(start));size_t prefix=strlen(json);
    memset(json+prefix,'s',128);memcpy(json+prefix+128,"\"}",3);bad(json,false);
    char url[512],guard[8];memset(guard,'G',sizeof(guard));
    assert(feishu_register_url(url,sizeof(url),"https://open.feishu.cn/page/launcher?user_code=FAKE", "cli_original"));
    assert(strstr(url,"&addons=") && strstr(url,"&clientID=cli_original") && strlen(url)<320);
    assert(!feishu_register_url(guard,4,"https://open.feishu.cn/test",NULL) && guard[0]==0 && guard[4]=='G');
    assert(!feishu_register_url(url,sizeof(url),"https://open.feishu.cn/test#fragment",NULL));
    assert(!feishu_register_url(url,sizeof(url),"https://open.feishu.cn/test\r\n",NULL));
    assert(!feishu_register_url(url,sizeof(url),"https://open.feishu.cn/test","cli_x&attacker=y"));
    char form[820];assert(feishu_register_poll_form(form,sizeof(form),"a+b/&=%"));
    assert(!strcmp(form,"action=poll&device_code=a%2Bb%2F%26%3D%25"));
    assert(!feishu_register_poll_form(guard,sizeof(guard),"x"));
    memset(json,'%',255);json[255]=0;assert(feishu_register_poll_form(form,sizeof(form),json));
    /* Current service responses contain a 514-byte opaque device code. */
    char code[FEISHU_REGISTER_CODE_MAX+1],large_form[FEISHU_REGISTER_CODE_MAX*3+32];
    for(unsigned length=514;length<FEISHU_REGISTER_CODE_MAX;length=FEISHU_REGISTER_CODE_MAX-1) {
        memset(code,'a',length);code[length]=0;
        snprintf(json,sizeof(json),"{\"device_code\":\"%s\",\"verification_uri_complete\":\"https://open.feishu.cn/test\",\"expires_in\":3600}",code);
        assert(decode(json,true) && strlen(r.device_code)==length);
        size_t needed=feishu_register_poll_form_size(code);
        assert(needed==strlen("action=poll&device_code=")+length+1);
        assert(feishu_register_poll_form(large_form,needed,code) && strlen(large_form)+1==needed);
        if(length==FEISHU_REGISTER_CODE_MAX-1)break;
    }
    memset(code,'%',FEISHU_REGISTER_CODE_MAX-1);code[FEISHU_REGISTER_CODE_MAX-1]=0;
    size_t encoded=feishu_register_poll_form_size(code);
    assert(encoded==strlen("action=poll&device_code=")+3*(FEISHU_REGISTER_CODE_MAX-1)+1);
    assert(feishu_register_poll_form(large_form,encoded,code));
    assert(!feishu_register_poll_form(large_form,encoded-1,code));
    memset(code,'a',FEISHU_REGISTER_CODE_MAX);code[FEISHU_REGISTER_CODE_MAX]=0;
    assert(!feishu_register_poll_form_size(code));
    snprintf(json,sizeof(json),"{\"device_code\":\"%s\",\"verification_uri_complete\":\"https://open.feishu.cn/test\"}",code);
    bad(json,true);
    for(unsigned i=0;i<2000;++i)assert(decode(begin,true));
    puts("registration protocol: normal, bounded stress, malformed/duplicate/nested/type/secret/URL errors, encoding PASS");
    return 0;
}

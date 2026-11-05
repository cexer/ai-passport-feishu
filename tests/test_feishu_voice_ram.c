#include "feishu_api.h"
#include "feishu_http.h"
#include "feishu_cache.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned begins,appends,aborts;
static const char *reply="hello";
static int http_status=200;
static unsigned error_code=10000;
static bool end=true;
uint32_t esp_random(void) {return 0x12345678;}
int64_t esp_timer_get_time(void) {return 1;}
uint32_t esp_get_free_heap_size(void) {return 32000;}
/* Deliberately model a full/unavailable draft database. RAM recognition must
 * work without calling any of these functions. */
esp_err_t feishu_cache_draft_begin(feishu_data_writer_t *w) {(void)w;++begins;return ESP_FAIL;}
esp_err_t feishu_cache_text_append(feishu_data_writer_t *w,const void *p,size_t n)
{(void)w;(void)p;(void)n;++appends;return ESP_FAIL;}
void feishu_cache_text_abort(feishu_data_writer_t *w) {(void)w;++aborts;}
static size_t var(uint8_t *p,uint64_t n)
{size_t i=0;do{p[i++]=(uint8_t)((n&127)|(n>=128?128:0));n>>=7;}while(n);return i;}
static size_t bytes(uint8_t *p,unsigned f,const void *v,size_t n)
{size_t i=var(p,((uint64_t)f<<3)|2);i+=var(p+i,n);memcpy(p+i,v,n);return i+n;}
static size_t number(uint8_t *p,unsigned f,uint64_t n)
{size_t i=var(p,(uint64_t)f<<3);return i+var(p+i,n);}
static size_t written;
static bool upload_write(void *unused,const void *p,size_t n)
{(void)unused;assert(p||!n);written+=n;return true;}
esp_err_t feishu_http_post_headers_stream(const char *url,const feishu_http_header_t *headers,size_t count,
    size_t body_size,feishu_http_upload_t upload,void *upload_context,bool keepalive,size_t response_limit,
    feishu_http_sink_t sink,void *sink_context,feishu_http_cancel_t cancel,void *cancel_context,
    int *status,size_t *response_size)
{
    assert(strstr(url,"/im/gateway/") && keepalive && response_limit>=300u*1024u);
    *status=http_status;*response_size=0;
    if(cancel && cancel(cancel_context))return FEISHU_HTTP_ERR_CANCELLED;
    const char *cid=NULL;
    for(size_t i=0;i<count;++i)if(!strcmp(headers[i].name,"x-request-id"))cid=headers[i].value;
    assert(cid);written=0;assert(upload(upload_context,upload_write,NULL) && written==body_size);
    uint8_t inner[2048],packet[2304];size_t n=0,p=0;
    n+=bytes(inner+n,1,"test-source",11);n+=number(inner+n,2,0);
    n+=bytes(inner+n,3,reply,strlen(reply));n+=number(inner+n,4,end);n+=number(inner+n,6,error_code);
    p+=number(packet+p,2,1);p+=number(packet+p,3,1);p+=bytes(packet+p,6,cid,strlen(cid));p+=bytes(packet+p,5,inner,n);
    for(size_t at=0;at<p;) {
        size_t take=p-at;if(take>7)take=7;
        esp_err_t err=sink(sink_context,packet+at,take);if(err!=ESP_OK)return err;at+=take;
    }
    *response_size=p;return ESP_OK;
}
static esp_err_t request(unsigned action,char *preview,size_t cap,uint64_t *length,feishu_data_writer_t *draft)
{return feishu_api_internal_asr_packet("session=fake","test-source","test-context",1,action,NULL,0,preview,cap,length,draft,NULL,NULL);}
int main(void)
{
    char preview[16]="previous";uint64_t length=0;
    assert(request(2,preview,sizeof(preview),&length,NULL)==ESP_OK && !strcmp(preview,"hello") && length==5);
    assert(!begins && !appends && !aborts);
    reply="hello world";end=false;assert(request(0,preview,sizeof(preview),&length,NULL)==ESP_OK);
    assert(!strcmp(preview,"hello world") && length==11);
    reply="";end=true;assert(request(2,preview,sizeof(preview),&length,NULL)==ESP_OK && !strcmp(preview,"hello world"));
    char large[1025];memset(large,'a',sizeof(large)-1);large[sizeof(large)-1]=0;reply=large;
    assert(request(2,preview,sizeof(preview),&length,NULL)==FEISHU_ERR_ASR_TOO_LONG && !strcmp(preview,"hello world"));
    end=false;assert(request(0,preview,sizeof(preview),&length,NULL)==ESP_OK && length==1024);
    reply="";end=true;assert(request(2,preview,sizeof(preview),&length,NULL)==FEISHU_ERR_ASR_TOO_LONG);
    /* Five UTF-8 characters exactly fit 15 bytes plus the terminator. */
    reply="\xe4\xb8\xad\xe4\xb8\xad\xe4\xb8\xad\xe4\xb8\xad\xe4\xb8\xad";
    assert(request(2,preview,sizeof(preview),&length,NULL)==ESP_OK && length==15 && strlen(preview)==15);
    assert(request(2,preview,15,&length,NULL)==FEISHU_ERR_ASR_TOO_LONG);
    reply="bad";error_code=42;assert(request(2,preview,sizeof(preview),&length,NULL)==ESP_ERR_INVALID_RESPONSE);
    error_code=10000;http_status=401;assert(request(2,preview,sizeof(preview),&length,NULL)==FEISHU_ERR_VOICE_LOGIN);
    http_status=429;assert(request(2,preview,sizeof(preview),&length,NULL)==FEISHU_ERR_ASR_LIMITED);
    http_status=200;reply="\xc0\x80";assert(request(2,preview,sizeof(preview),&length,NULL)==ESP_ERR_INVALID_RESPONSE);
    reply="hello";assert(request(2,preview,sizeof(preview),&length,NULL)==ESP_OK);
    assert(!begins && !appends && !aborts);
    feishu_data_writer_t draft={0};assert(request(2,preview,sizeof(preview),&length,&draft)==ESP_FAIL && begins==1 && aborts==1);
    puts("voice RAM: full-database independence, cumulative/empty-END, UTF8 capacity, overflow, auth/error and recovery PASS");
    return 0;
}

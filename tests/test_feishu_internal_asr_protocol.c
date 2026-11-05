#include "feishu_internal_asr_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static size_t received,largest;
static bool fail_sink;
static bool sink(void *unused,const void *data,size_t n)
{(void)unused;assert(data&&n&&n<=1024);if(fail_sink)return false;received+=n;if(n>largest)largest=n;return true;}
static size_t vi(uint8_t *b,uint64_t n)
{size_t i=0;do{b[i++]=(n&127)|(n>=128?128:0);n>>=7;}while(n);return i;}
static size_t field(uint8_t *b,unsigned id,const void *data,size_t n)
{size_t i=vi(b,((uint64_t)id<<3)|2);i+=vi(b+i,n);memcpy(b+i,data,n);return i+n;}
static size_t response(uint8_t *b,const void *text,size_t n,uint32_t status,uint32_t error)
{
 uint8_t inner[512];size_t p=0;p+=field(inner+p,1,"test",4);inner[p++]=16;inner[p++]=1;p+=field(inner+p,3,text,n);inner[p++]=32;inner[p++]=1;inner[p++]=48;p+=vi(inner+p,error);
 size_t i=0;b[i++]=16;b[i++]=1;b[i++]=24;i+=vi(b+i,1);i+=field(b+i,6,"req-0",5);i+=field(b+i,5,inner,p);
 b[i++]=32;i+=vi(b+i,status);return i;
}
static void init(feishu_internal_asr_parser_t *p,char *preview,size_t cap)
{received=largest=0;feishu_internal_asr_init(p,"test","req-0",1,preview,cap,sink,NULL);}
int main(int argc,char **argv)
{
 uint8_t request[256];size_t body=0,n=feishu_internal_asr_header(request,sizeof(request),"test","req-0","00000000-0000-4000-8000-000000000000",1,1,4,&body);
 assert(n&&body==n+4&&request[0]==16&&request[1]==1);
 if(argc==2){FILE *f=fopen(argv[1],"wb");assert(f);assert(fwrite(request,1,n,f)==n);const uint8_t pcm[]={0,1,2,3};assert(fwrite(pcm,1,4,f)==4);assert(!fclose(f));}
 assert(!feishu_internal_asr_header(request,2,"test","req-0","00000000-0000-4000-8000-000000000000",1,1,4,&body)&&!body);
 assert(!feishu_internal_asr_header(request,sizeof(request),"test","req-0","00000000-0000-4000-8000-000000000000",1,3,4,&body));
 assert(!feishu_internal_asr_header(request,sizeof(request),"test","req-0","00000000-0000-4000-8000-000000000000",1,1,3,&body));
 assert(feishu_internal_asr_header(request,sizeof(request),"test","req-0","00000000-0000-4000-8000-000000000000",1,1,19200,&body));
 assert(!feishu_internal_asr_header(request,sizeof(request),"test","req-0","00000000-0000-4000-8000-000000000000",1,1,FEISHU_INTERNAL_ASR_PCM_LIMIT+2,&body));
 uint8_t packet[1024];char preview[16];feishu_internal_asr_parser_t p;
 const char *text="\xe4\xb8\xad\xe6\x96\x87\xf0\x9f\x98\x80";n=response(packet,text,strlen(text),0,0);
 for(size_t chunk=1;chunk<65;++chunk){init(&p,preview,sizeof(preview));for(size_t pos=0;pos<n;){size_t take=n-pos;if(take>chunk)take=chunk;assert(feishu_internal_asr_feed(&p,packet+pos,take));pos+=take;}assert(feishu_internal_asr_finish(&p)&&p.is_end&&received==strlen(text)&&!strcmp(preview,text));}
 /* Real gateway returns recognition revisions, including repeated zero, not
  * the request sequence. cid and source remain mandatory correlation keys. */
 feishu_internal_asr_init(&p,"test","req-0",99,preview,sizeof(preview),sink,NULL);
 assert(feishu_internal_asr_feed(&p,packet,n)&&feishu_internal_asr_finish(&p)&&p.response_sequence==1);
 uint8_t zero_revision[1024];memcpy(zero_revision,packet,n);
 for(size_t i=0;i+7<n;++i)if(zero_revision[i]==10&&zero_revision[i+1]==4&&!memcmp(zero_revision+i+2,"test",4)&&zero_revision[i+6]==16){zero_revision[i+7]=0;break;}
 init(&p,preview,sizeof(preview));assert(feishu_internal_asr_feed(&p,zero_revision,n)&&feishu_internal_asr_finish(&p)&&!p.response_sequence);
 init(&p,preview,sizeof(preview));feishu_internal_asr_init(&p,"test","wrong-cid",1,preview,sizeof(preview),sink,NULL);
 assert(!feishu_internal_asr_feed(&p,packet,n));
 uint8_t no_cid[1024];memcpy(no_cid,packet,n);assert(no_cid[4]==50);no_cid[4]=58;
 init(&p,preview,sizeof(preview));assert(feishu_internal_asr_feed(&p,no_cid,n)&&!feishu_internal_asr_finish(&p));
 for(size_t cut=0;cut<n;++cut){init(&p,preview,sizeof(preview));feishu_internal_asr_feed(&p,packet,cut);if(cut==n-2)assert(feishu_internal_asr_finish(&p));else assert(!feishu_internal_asr_finish(&p));}
 n=response(packet,text,strlen(text),5,0);init(&p,preview,sizeof(preview));assert(feishu_internal_asr_feed(&p,packet,n));assert(!feishu_internal_asr_finish(&p)); /* Late status must not publish staging. */
 n=response(packet,text,strlen(text),0,10000);init(&p,preview,sizeof(preview));assert(feishu_internal_asr_feed(&p,packet,n)&&feishu_internal_asr_finish(&p));
 n=response(packet,text,strlen(text),0,12);init(&p,preview,sizeof(preview));assert(feishu_internal_asr_feed(&p,packet,n));assert(!feishu_internal_asr_finish(&p));
 const char *bad[]={"\xc0\xaf","\xed\xa0\x80","\xf4\x90\x80\x80","\xe4\xb8"};
 for(unsigned i=0;i<4;++i){n=response(packet,bad[i],strlen(bad[i]),0,0);init(&p,preview,sizeof(preview));assert(!feishu_internal_asr_feed(&p,packet,n));}
 const uint8_t nul[]={65,0,66};n=response(packet,nul,sizeof(nul),0,0);init(&p,preview,sizeof(preview));assert(!feishu_internal_asr_feed(&p,packet,n));
 n=response(packet,"normal",6,0,0);init(&p,preview,sizeof(preview));fail_sink=true;assert(!feishu_internal_asr_feed(&p,packet,n));fail_sink=false;
 init(&p,preview,sizeof(preview));assert(feishu_internal_asr_feed(&p,packet,n)&&feishu_internal_asr_finish(&p)&&!strcmp(preview,"normal"));
 /* Unknown 64-bit fields are skipped; known duplicates and wrong streams rejected. */
 uint8_t extra[12];size_t m=vi(extra,96);m+=vi(extra+m,UINT64_MAX);assert(feishu_internal_asr_feed(&p,extra,m)&&feishu_internal_asr_finish(&p));
 init(&p,preview,sizeof(preview));assert(feishu_internal_asr_feed(&p,packet,n));const uint8_t duplicate[]={32,0};assert(!feishu_internal_asr_feed(&p,duplicate,sizeof(duplicate)));
 init(&p,preview,sizeof(preview));feishu_internal_asr_init(&p,"wrong","req-0",1,preview,sizeof(preview),sink,NULL);assert(!feishu_internal_asr_feed(&p,packet,n));
 init(&p,preview,sizeof(preview));const uint8_t overflow[]={0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x02};assert(!feishu_internal_asr_feed(&p,overflow,sizeof(overflow)));
 /* A 300 KiB cumulative result streams into bounded storage, never a full RAM string. */
 uint8_t inner_head[32],head[64];size_t ih=field(inner_head,1,"test",4);inner_head[ih++]=16;inner_head[ih++]=1;inner_head[ih++]=26;ih+=vi(inner_head+ih,FEISHU_INTERNAL_ASR_LIMIT);
 size_t h=0;head[h++]=16;head[h++]=1;head[h++]=24;h+=vi(head+h,1);h+=field(head+h,6,"req-0",5);head[h++]=42;h+=vi(head+h,ih+FEISHU_INTERNAL_ASR_LIMIT+2);memcpy(head+h,inner_head,ih);h+=ih;
 init(&p,preview,sizeof(preview));assert(feishu_internal_asr_feed(&p,head,h));uint8_t block[1024];memset(block,'a',sizeof(block));for(unsigned i=0;i<300;++i)assert(feishu_internal_asr_feed(&p,block,sizeof(block)));
 const uint8_t end[]={32,1};assert(feishu_internal_asr_feed(&p,end,sizeof(end))&&feishu_internal_asr_finish(&p)&&received==FEISHU_INTERNAL_ASR_LIMIT&&largest==1024&&strlen(preview)==15);
 init(&p,preview,sizeof(preview));assert(feishu_internal_asr_feed(&p,packet,n)&&feishu_internal_asr_finish(&p));
 printf("internal ASR protocol: fragmented, UTF8, late failure, 300KiB, overflow and recovery PASS; parser=%zu\n",sizeof(p));return 0;
}

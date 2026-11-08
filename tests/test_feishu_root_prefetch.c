#include "feishu_root_flashdb.h"
#include "feishu_fdb_host.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static const feishu_root_t root={.directory="raw"};
static unsigned char data[7168],out[7168],noise[512];
int main(void){
 const feishu_fdb_io_t *io=feishu_fdb_host_reset();assert(feishu_fdb_open(io,true)==FEISHU_FDB_OK);
 for(unsigned i=0;i<sizeof(data);++i)data[i]=(unsigned char)(i*19+7);
 feishu_root_part_t part={data,sizeof(data)};assert(feishu_root_write(&root,1,&part,1)==FEISHU_ROOT_OK);
 memset(noise,0x5a,sizeof(noise));
 for(unsigned i=0;i<180;++i){char key[32];snprintf(key,sizeof(key),"noise.%03u",i);assert(feishu_fdb_put(key,noise,sizeof(noise))==FEISHU_FDB_OK);}
 feishu_fdb_close();assert(feishu_fdb_open(io,false)==FEISHU_FDB_OK);
 feishu_fdb_host_fault(-1,-1,false);size_t total=0;
 assert(feishu_root_read(&root,1,0,out,sizeof(out),&total)==FEISHU_ROOT_OK && total==sizeof(out) && !memcmp(data,out,sizeof(out)));
 unsigned cold=feishu_fdb_host_reads();printf("Root cold read: payload=%zu noise=180 reads=%u writes=%u\n",sizeof(data),cold,feishu_fdb_host_writes());
 assert(!feishu_fdb_host_writes());
#ifndef FEISHU_BENCH_ALLOW_OLD
 assert(cold<500);
#endif
#ifdef FEISHU_ASSISTANT_AUTOTEST
 unsigned writes=feishu_fdb_host_writes(),reads=feishu_fdb_host_reads();
 uint64_t revision=feishu_fdb_revision(),root_revision=feishu_fdb_root_revision();
 assert(feishu_fdb_probe_cold()==FEISHU_FDB_OK);
 assert(feishu_fdb_host_writes()==writes && feishu_fdb_host_reads()==reads);
 assert(feishu_fdb_revision()==revision && feishu_fdb_root_revision()==root_revision);
 feishu_root_flashdb_probe_cold();
 assert(feishu_root_read(&root,1,0,out,sizeof(out),&total)==FEISHU_ROOT_OK && !memcmp(data,out,sizeof(out)));
 unsigned after_drop=feishu_fdb_host_reads()-reads;
 printf("Explicit cold hints/proofs: reads=%u writes=%u\n",after_drop,feishu_fdb_host_writes()-writes);
 assert(after_drop>100 && after_drop<500 && feishu_fdb_host_writes()==writes);
 puts("RAM-only cold preparation: zero IO/epoch change; cold read preserved: PASS");
#endif
 feishu_fdb_close();
}

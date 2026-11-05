#include "feishu_recovery.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

static long budget=-1;
static unsigned rename_calls,fail_rename;
static size_t limited_write(const void *bytes,size_t size,FILE *file)
{
    if(budget>=0 && size>(size_t)budget)size=(size_t)budget;
    size_t written=fwrite(bytes,1,size,file);
    if(budget>=0) {budget-=(long)written;if(!budget)errno=ENOSPC;}
    return written;
}
static int rename_once(const char *from,const char *to)
{
    if(++rename_calls==fail_rename) {errno=EIO;return -1;}
    return rename(from,to);
}
static void file_path(const char *dir,const char *name,char out[256])
{int n=snprintf(out,256,"%s/%s",dir,name);assert(n>0 && n<256);}
static void write_file(const char *dir,const char *name,const char *value)
{
    char path[256];file_path(dir,name,path);FILE *f=fopen(path,"wb");assert(f);
    assert(fwrite(value,1,strlen(value),f)==strlen(value) && !fclose(f));
}
static void expect_file(const char *dir,const char *name,const char *value)
{
    char path[256],bytes[64]={0};file_path(dir,name,path);FILE *f=fopen(path,"rb");assert(f);
    size_t size=fread(bytes,1,sizeof(bytes),f);assert(!fclose(f));
    assert(size==strlen(value) && !memcmp(bytes,value,size));
}
static void setup(const char *base,unsigned index,char dir[128],feishu_recovery_t *r,bool baseline)
{
    int n=snprintf(dir,128,"%s/case-%03u",base,index);assert(n>0 && n<128);
#ifdef _WIN32
    assert(!_mkdir(dir));
#else
    assert(!mkdir(dir,0700));
#endif
    *r=(feishu_recovery_t){.root={.directory=dir,.write=limited_write},.rename_file=rename_once};
    budget=-1;rename_calls=fail_rename=0;
    feishu_root_part_t config={"essential credentials",21};assert(!feishu_root_write(&r->root,1,&config,1));
    write_file(dir,"objects.db","failed DB with protected draft");write_file(dir,"objects.db-journal","old journal bytes");
    if(baseline) {feishu_recovery_info_t info;assert(!feishu_recovery_resume(r,&info) && !info.held);}
}
static void preserved(const char *dir,const feishu_recovery_t *r)
{
    expect_file(dir,"objects.q00000001.db","failed DB with protected draft");
    expect_file(dir,"objects.q00000001.db-journal","old journal bytes");
    char config[21];size_t total=0;
    assert(!feishu_root_read(&r->root,1,0,config,sizeof(config),&total) && total==21 && !memcmp(config,"essential credentials",21));
}
int main(int argc,char **argv)
{
    assert(argc==2);unsigned cases=0;char dir[128];feishu_recovery_t r;feishu_recovery_info_t info;
    setup(argv[1],cases++,dir,&r,true);
    assert(!feishu_recovery_isolate(&r,UINT64_C(0x123456789),&info) && info.held && info.generation==1 && info.id_floor==UINT64_C(0x123456789));
    preserved(dir,&r);
    write_file(dir,"objects.db","fresh DB");write_file(dir,"objects.db-journal","fresh journal");
    assert(!feishu_recovery_resume(&r,&info));expect_file(dir,"objects.db","fresh DB");
    assert(!feishu_recovery_isolate(&r,UINT64_C(0x123456800),&info) && info.generation==2);
    expect_file(dir,"objects.q00000002.db","fresh DB");preserved(dir,&r);
    for(unsigned failure=1;failure<=2;++failure) {
        setup(argv[1],cases++,dir,&r,true);fail_rename=failure;
        assert(feishu_recovery_isolate(&r,77,&info)==FEISHU_ROOT_IO);
        fail_rename=0;assert(!feishu_recovery_resume(&r,&info) && info.generation==1 && info.id_floor==77);preserved(dir,&r);
    }
    for(long limit=0;limit<112;++limit) {
        setup(argv[1],cases++,dir,&r,true);budget=limit;
        assert(feishu_recovery_isolate(&r,123,&info)!=FEISHU_ROOT_OK);
        budget=-1;assert(!feishu_recovery_resume(&r,&info));
        if(!info.held)assert(!feishu_recovery_isolate(&r,123,&info));
        assert(info.held && info.generation==1 && info.id_floor==123);preserved(dir,&r);
    }
    for(long limit=0;limit<56;++limit) {
        setup(argv[1],cases++,dir,&r,false);budget=limit;
        assert(feishu_recovery_resume(&r,&info)!=FEISHU_ROOT_OK);
        budget=-1;assert(!feishu_recovery_resume(&r,&info) && !info.held);
        assert(!feishu_recovery_isolate(&r,9,&info));preserved(dir,&r);
    }
    setup(argv[1],cases++,dir,&r,true);write_file(dir,"objects.q00000001.db","do not overwrite");
    assert(feishu_recovery_isolate(&r,1,&info)==FEISHU_ROOT_CORRUPT);
    expect_file(dir,"objects.db","failed DB with protected draft");expect_file(dir,"objects.q00000001.db","do not overwrite");
    setup(argv[1],cases++,dir,&r,true);assert(!feishu_recovery_isolate(&r,1,&info));
    write_file(dir,"cfg7fa","partial marker");write_file(dir,"cfg7fb","partial marker");
    assert(feishu_recovery_resume(&r,&info)==FEISHU_ROOT_CORRUPT);preserved(dir,&r);
    printf("database isolation: %u normal/partial-write/interrupted-rename/conflict cases, preserved config and archives PASS\n",cases);
    return 0;
}

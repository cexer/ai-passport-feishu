#include "feishu_root.h"
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>

static long budget=-1;
static unsigned sync_calls,fail_sync;
static size_t limited_write(const void *data,size_t size,FILE *file)
{
    if(budget>=0 && size>(size_t)budget)size=(size_t)budget;
    size_t n=fwrite(data,1,size,file);
    if(budget>=0) {budget-=(long)n;if(!budget)errno=ENOSPC;}
    return n;
}
static int injected_sync(FILE *file)
{
    ++sync_calls;
    if(sync_calls==fail_sync) {errno=EIO;return -1;}
    return fflush(file);
}
static void corrupt(const char *dir,unsigned key,unsigned slot)
{
    char path[128];snprintf(path,sizeof(path),"%s/cfg%02x%c",dir,key,slot ? 'b':'a');
    FILE *f=fopen(path,"r+b");assert(f);assert(!fseek(f,33,SEEK_SET));
    int c=fgetc(f);assert(c!=EOF);assert(!fseek(f,33,SEEK_SET));fputc(c^1,f);assert(!fclose(f));
}
int main(int argc,char **argv)
{
    assert(argc==2);feishu_root_t root={.directory=argv[1]};size_t total=0;
    uint8_t value[8192],out[8192];memset(value,'A',sizeof(value));
    assert(feishu_root_read(&root,1,0,NULL,0,&total)==FEISHU_ROOT_MISSING);
    feishu_root_part_t part={value,sizeof(value)};
    assert(!feishu_root_write(&root,1,&part,1));
    assert(!feishu_root_read(&root,1,0,out,sizeof(out),&total));
    assert(total==sizeof(value) && !memcmp(out,value,total));
    corrupt(argv[1],1,0);
    assert(!feishu_root_read(&root,1,0,out,sizeof(out),&total));
    assert(!memcmp(value,out,total));
    assert(!feishu_root_write(&root,1,&part,1));
    corrupt(argv[1],1,1); /* Re-saving identical data repairs the other copy. */
    assert(!feishu_root_read(&root,1,0,out,sizeof(out),&total));
    assert(!memcmp(value,out,total));
    memset(value,'B',sizeof(value));assert(!feishu_root_write(&root,1,&part,1));
    corrupt(argv[1],1,1);
    assert(!feishu_root_read(&root,1,7900,out,292,&total));
    for(unsigned i=0;i<292;++i)assert(out[i]=='B');
    assert(feishu_root_read(&root,1,8191,out,2,&total)==FEISHU_ROOT_INVALID);
    puts("root mirrored copies, corruption recovery and bounded slices: PASS");

    root.write=limited_write;
    /* Every boundary below interrupts a different part of an 8 KiB update.
     * The old generation remains valid and a later normal update recovers. */
    for(unsigned i=0;i<20;++i) {
        unsigned key=10+i;memset(value,'O',sizeof(value));budget=-1;
        assert(!feishu_root_write(&root,key,&part,1));
        memset(value,'N',sizeof(value));budget=(long)(i*431);
        assert(feishu_root_write(&root,key,&part,1)==FEISHU_ROOT_FULL);
        assert(!feishu_root_read(&root,key,0,out,sizeof(out),&total));
        for(size_t j=0;j<total;++j)assert(out[j]=='O');
        budget=-1;assert(!feishu_root_write(&root,key,&part,1));
        assert(!feishu_root_read(&root,key,0,out,sizeof(out),&total));
        assert(!memcmp(out,value,total));
    }
    puts("root twenty partial/full write failures preserve old state and recover: PASS");
    budget=17;
    assert(feishu_root_write(&root,99,&part,1)==FEISHU_ROOT_FULL);
    budget=-1;assert(!feishu_root_write(&root,99,&part,1));
    assert(!feishu_root_read(&root,99,0,out,sizeof(out),&total));assert(!memcmp(out,value,total));
    /* Full first copy, interrupted redundant copy: publication is successful. */
    memset(value,'R',sizeof(value));budget=32+sizeof(value)+97;
    assert(!feishu_root_write(&root,100,&part,1));
    assert(!feishu_root_read(&root,100,0,out,sizeof(out),&total));assert(!memcmp(value,out,total));
    budget=-1;root.sync=injected_sync;sync_calls=0;fail_sync=1;
    assert(feishu_root_write(&root,101,&part,1)==FEISHU_ROOT_IO);
    fail_sync=0;assert(!feishu_root_write(&root,101,&part,1));
    assert(!feishu_root_read(&root,101,0,out,sizeof(out),&total));
    assert(!memcmp(value,out,total));
    feishu_root_part_t parts[]={{"hi",2},{NULL,500},{"end",3}};
    assert(!feishu_root_write(&root,102,parts,3));
    assert(!feishu_root_read(&root,102,0,out,505,&total));
    assert(!memcmp(out,"hi",2) && !memcmp(out+502,"end",3));
    for(unsigned i=2;i<502;++i)assert(!out[i]);
    part.size=FEISHU_ROOT_MAX+1;
    assert(feishu_root_write(&root,102,&part,1)==FEISHU_ROOT_INVALID);
    corrupt(argv[1],102,0);corrupt(argv[1],102,1);
    assert(feishu_root_read(&root,102,0,out,505,&total)==FEISHU_ROOT_CORRUPT);
    assert(feishu_root_write(&root,102,parts,3)==FEISHU_ROOT_CORRUPT);
    puts("root redundancy, IO recovery, zero pieces and damaged-root preservation: PASS");
    return 0;
}

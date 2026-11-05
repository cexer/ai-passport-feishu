#include "feishu_data.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void write_value(feishu_data_t *s,const char *key,const char *text,bool required,feishu_data_writer_t *w)
{
    assert(feishu_data_begin(s,key,required?FEISHU_DATA_REQUIRED:FEISHU_DATA_TEXT,required,w)==FEISHU_DATA_OK);
    assert(feishu_data_append(s,w,(const uint8_t *)text,strlen(text)+1,true)==FEISHU_DATA_OK);
}
static feishu_data_object_t expect(feishu_data_t *s,const char *key,const char *text)
{
    feishu_data_object_t o;uint8_t bytes[1024];size_t size=0;
    assert(feishu_data_find(s,key,&o)==FEISHU_DATA_OK);
    assert(feishu_data_read(s,o.id,0,bytes,sizeof(bytes),&size)==FEISHU_DATA_OK);
    assert(size==strlen(text)+1 && !memcmp(bytes,text,size));
    return o;
}
int main(int argc,char **argv)
{
    assert(argc==3);
    feishu_data_io_begin();feishu_data_io_begin();feishu_data_io_end();feishu_data_io_end();
    feishu_data_t store={0},other={0};
    assert(!feishu_data_is_open(NULL) && !feishu_data_is_open(&store));
    assert(feishu_data_diagnostics(&store).native_code==0);
    assert(!strcmp(feishu_data_backend_name(),argv[2]));
    assert(feishu_data_open(&store,argv[1],NULL)==FEISHU_DATA_OK);
    assert(feishu_data_is_open(&store));
    assert(feishu_data_open(&other,argv[1],NULL)==FEISHU_DATA_BUSY);
    feishu_data_writer_t body,meta;
    write_value(&store,"draft/body","first body",true,&body);
    write_value(&store,"draft/meta","first state",true,&meta);
    assert(feishu_data_commit_pair(&store,&body,NULL,&meta,NULL,0)==FEISHU_DATA_OK);
    feishu_data_object_t first=expect(&store,"draft/body","first body");
    assert(first.protected_data);
    assert(feishu_data_pin(&store,first.id,true)==FEISHU_DATA_OK);
    write_value(&store,"stage/body","next body",true,&body);
    write_value(&store,"stage/meta","next state",true,&meta);
    assert(feishu_data_commit_pair(&store,&body,"draft/body",&body,"draft/meta",first.id)==FEISHU_DATA_MISUSE);
    expect(&store,"draft/body","first body");expect(&store,"draft/meta","first state");
    assert(feishu_data_commit_pair(&store,&body,"draft/body",&meta,"draft/meta",first.id)==FEISHU_DATA_OK);
    assert(!body.id && !meta.id);
    expect(&store,"draft/body","next body");expect(&store,"draft/meta","next state");
    for(unsigned i=0;i<12;++i){size_t bytes=0;int rc=feishu_data_reclaim(&store,&bytes);assert(rc==FEISHU_DATA_OK || rc==FEISHU_DATA_FULL);}
    uint8_t bytes[1024];size_t size;
    assert(feishu_data_read(&store,first.id,0,bytes,sizeof(bytes),&size)==FEISHU_DATA_OK);
    assert(feishu_data_pin(&store,first.id,false)==FEISHU_DATA_OK);
    for(unsigned i=0;i<12;++i){size_t count=0;int rc=feishu_data_reclaim(&store,&count);assert(rc==FEISHU_DATA_OK || rc==FEISHU_DATA_FULL);}
    assert(feishu_data_read(&store,first.id,0,bytes,sizeof(bytes),&size)==FEISHU_DATA_NOTFOUND);
    assert(feishu_data_close(&store)==FEISHU_DATA_OK && !feishu_data_is_open(&store));
    assert(feishu_data_open(&store,argv[1],NULL)==FEISHU_DATA_OK);
    expect(&store,"draft/body","next body");expect(&store,"draft/meta","next state");
    uint64_t floor=50000;assert(feishu_data_id_floor(&store,&floor)==FEISHU_DATA_OK && floor>=50000);
    feishu_data_writer_t pressure;
    assert(feishu_data_begin(&store,"pressure",FEISHU_DATA_METADATA,false,&pressure)==FEISHU_DATA_OK && (uint64_t)pressure.id>50000);
    memset(bytes,0xa5,sizeof(bytes));bool full=false;
    for(unsigned i=0;i<256;++i){feishu_data_writer_t before=pressure;int rc=feishu_data_append(&store,&pressure,bytes,sizeof(bytes),false);
        if(rc==FEISHU_DATA_FULL){assert(pressure.id==before.id && pressure.raw==before.raw && pressure.stored==before.stored && pressure.sequence==before.sequence);full=true;break;}assert(rc==FEISHU_DATA_OK);}
    assert(full);
    assert(feishu_data_abort(&store,&pressure)==FEISHU_DATA_OK);
    expect(&store,"draft/body","next body");expect(&store,"draft/meta","next state");
    for(unsigned i=0;i<400;++i){size_t count=0;int rc=feishu_data_reclaim(&store,&count);if(rc==FEISHU_DATA_FULL)break;assert(rc==FEISHU_DATA_OK);}
    write_value(&store,"after-full","recovered",false,&body);assert(feishu_data_commit(&store,&body)==FEISHU_DATA_OK);
    expect(&store,"after-full","recovered");expect(&store,"draft/body","next body");
    assert(feishu_data_close(&store)==FEISHU_DATA_OK);
    assert(feishu_data_diagnostics(&store).native_code==0);
    puts("Storage contract: atomic pair, protected/current readers, reopen, ID floor, quota failure and recovery PASS");
    return 0;
}

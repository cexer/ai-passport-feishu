#include "feishu_xiaozhi_downlink.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned calls;
static bool reject;
static bool sink(void *context,uint8_t *data,size_t size)
{assert(context==&calls);assert(size==3 && !memcmp(data,"abc",3));++calls;return !reject;}
int main(void)
{
    feishu_xiao_downlink_t p;
    const uint8_t v1[]={'a','b','c'},v2[]={0,2,0,0,0,0,0,0,0,0,0,42,0,0,0,3,'a','b','c'},v3[]={0,0,0,3,'a','b','c'};
    const uint8_t *packets[]={v1,v2,v3};const size_t sizes[]={sizeof(v1),sizeof(v2),sizeof(v3)};
    for(unsigned version=1;version<=3;++version)for(size_t stride=1;stride<=sizes[version-1];++stride) {
        assert(feishu_xiao_downlink_init(&p,version));
        unsigned before=calls;
        for(size_t offset=0;offset<sizes[version-1];offset+=stride) {
            size_t n=sizes[version-1]-offset;if(n>stride)n=stride;
            assert(feishu_xiao_downlink_feed(&p,packets[version-1]+offset,n));
            assert(feishu_xiao_downlink_end(&p,false,sink,&calls));assert(calls==before);
        }
        assert(feishu_xiao_downlink_end(&p,true,sink,&calls));assert(calls==before+1 && !p.used);
    }
    assert(!feishu_xiao_downlink_init(&p,0));assert(!feishu_xiao_downlink_feed(&p,v1,3));
    assert(feishu_xiao_downlink_init(&p,1));assert(!feishu_xiao_downlink_feed(&p,NULL,1));
    assert(feishu_xiao_downlink_init(&p,3));assert(!feishu_xiao_downlink_end(&p,true,sink,&calls));
    for(unsigned version=2;version<=3;++version) {
        size_t header=version==2?16:4;
        for(size_t i=0;i<sizes[version-1];++i) {
            assert(feishu_xiao_downlink_init(&p,version));
            assert(feishu_xiao_downlink_feed(&p,packets[version-1],i));
            assert(!feishu_xiao_downlink_end(&p,true,sink,&calls));
        }
        assert(feishu_xiao_downlink_init(&p,version));
        uint8_t oversized[FEISHU_XIAO_OPUS_CAP+17]={0};
        assert(!feishu_xiao_downlink_feed(&p,oversized,FEISHU_XIAO_OPUS_CAP+header+1));
        assert(!feishu_xiao_downlink_end(&p,true,sink,&calls));
        uint8_t bad[sizeof(v2)];memcpy(bad,packets[version-1],sizes[version-1]);bad[0]=1;
        assert(feishu_xiao_downlink_init(&p,version));assert(feishu_xiao_downlink_feed(&p,bad,sizes[version-1]));
        assert(!feishu_xiao_downlink_end(&p,true,sink,&calls));
    }
    reject=true;assert(feishu_xiao_downlink_init(&p,1));assert(feishu_xiao_downlink_feed(&p,v1,3));
    assert(!feishu_xiao_downlink_end(&p,true,sink,&calls));assert(!feishu_xiao_downlink_feed(&p,v1,3));
    reject=false;
    for(unsigned i=0;i<100000;++i) {
        assert(feishu_xiao_downlink_init(&p,1));assert(feishu_xiao_downlink_feed(&p,v1,3));
        assert(feishu_xiao_downlink_end(&p,true,sink,&calls));
    }
    printf("XiaoZhi downlink: v1/2/3 split/continuation, malformed/oversize, sink error and 100000 resets PASS; workspace=%zu\n",sizeof(p));
}

#include "feishu_xiaozhi_downlink.h"
#include <string.h>

bool feishu_xiao_downlink_init(feishu_xiao_downlink_t *p,unsigned version)
{
    if(!p)return false;
    memset(p,0,sizeof(*p));p->version=version;p->failed=version<1 || version>3;
    return !p->failed;
}
bool feishu_xiao_downlink_feed(feishu_xiao_downlink_t *p,const void *data,size_t size)
{
    if(!p || p->failed)return false;
    size_t header=p->version==2 ? 16:p->version==3 ? 4:0;
    if((!data && size) || size>FEISHU_XIAO_OPUS_CAP+header-p->used) {
        p->failed=true;return false;
    }
    if(size)memcpy(p->bytes+p->used,data,size);
    p->used+=size;return true;
}
bool feishu_xiao_downlink_end(feishu_xiao_downlink_t *p,bool fin,
                            feishu_xiao_opus_sink_t sink,void *context)
{
    if(!p || p->failed)return false;
    if(!fin)return true;
    size_t header=p->version==2 ? 16:p->version==3 ? 4:0;
    if(p->used<=header || !sink)goto invalid;
    size_t size=p->used-header;
    const uint8_t *b=p->bytes;
    if(p->version==2) {
        uint32_t declared=((uint32_t)b[12]<<24)|((uint32_t)b[13]<<16)|((uint32_t)b[14]<<8)|b[15];
        if(b[0] || b[1]!=2 || b[2] || b[3] || b[4] || b[5] || b[6] || b[7] || declared!=size)goto invalid;
    } else if(p->version==3) {
        if(b[0] || b[1] || (((size_t)b[2]<<8)|b[3])!=size)goto invalid;
    }
    if(!sink(context,p->bytes+header,size))goto invalid;
    p->used=0;return true;
invalid:
    p->failed=true;return false;
}

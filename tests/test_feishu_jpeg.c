#include "feishu_jpeg.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct {
    FILE *input,*output;
    size_t input_bytes,output_bytes,reads;
    int mode;
} io_t;
static int read_bytes(void *context,void *data,size_t capacity)
{
    io_t *p=context;assert(capacity<=512);
    if(p->mode==1 && p->input_bytes>=1024)return -1;
    size_t fragment=1+(++p->reads*37)%512;if(capacity>fragment)capacity=fragment;
    size_t n=fread(data,1,capacity,p->input);p->input_bytes+=n;return (int)n;
}
static bool begin(void *context,const feishu_image_info_t *info)
{ (void)context;assert(info->width<=FEISHU_PREVIEW_WIDTH && info->height<=FEISHU_PREVIEW_HEIGHT && info->workspace_peak<14000);return true; }
static bool write_bytes(void *context,const void *data,size_t size)
{
    io_t *p=context;assert(size<=216u*16u*2u);
    if(p->mode==2 && p->output_bytes>=1000)return false;
    p->output_bytes+=size;return fwrite(data,1,size,p->output)==size;
}
static bool cancelled(void *context) {return ((io_t *)context)->mode==3;}
int main(int argc,char **argv)
{
    feishu_image_info_t mapped;
    feishu_image_view_t requested={1,512,267};
    assert(feishu_jpeg_view_info(1024,768,&requested,&mapped) && mapped.top==0);
    feishu_preview_map_t source={215,161,0,0,1024,768};
    feishu_preview_map_t target={mapped.width,mapped.height,mapped.left,mapped.top,mapped.crop_width,mapped.crop_height};
    uint32_t sx,sy;
    assert(feishu_jpeg_map_pixel(&target,&source,54,132,&sx,&sy) && sx<source.width/2 && sy<source.height/2);
    target.left=0;
    assert(feishu_jpeg_map_pixel(&target,&source,162,72,&sx,&sy) && sx<source.width/2 && sy<source.height/2);
    assert(!feishu_jpeg_map_pixel(&target,&source,target.width,0,&sx,&sy));
    source.crop_width=0;
    assert(!feishu_jpeg_map_pixel(&target,&source,0,0,&sx,&sy));
    assert(argc==7 || argc==8);
    io_t context={.input=fopen(argv[1],"rb"),.output=fopen(argv[2],"wb"),.mode=argc==8 ? atoi(argv[7]):0};
    assert(context.input && context.output);
    feishu_jpeg_io_t io={read_bytes,begin,write_bytes,cancelled,&context};
    feishu_image_view_t view={(uint32_t)strtoul(argv[3],NULL,10),(int32_t)strtol(argv[4],NULL,10),(int32_t)strtol(argv[5],NULL,10)};
    feishu_image_info_t info;
    int status=feishu_jpeg_decode(&io,&view,&info),recovery=-1;
    if(context.mode) {
        context.mode=0;context.input_bytes=context.output_bytes=context.reads=0;
        assert(!fseek(context.input,0,SEEK_SET));
        context.output=freopen(argv[2],"wb",context.output);assert(context.output);
        recovery=feishu_jpeg_decode(&io,&view,&info);assert(recovery==FEISHU_JPEG_OK);
    }
    printf("{\"status\":%d,\"recovery\":%d,\"w\":%u,\"h\":%u,\"original_w\":%u,\"original_h\":%u,\"left\":%u,\"top\":%u,\"crop_w\":%u,\"crop_h\":%u,\"workspace\":%zu,\"bytes\":%zu}\n",
        status,recovery,(unsigned)info.width,(unsigned)info.height,(unsigned)info.original_width,(unsigned)info.original_height,
        (unsigned)info.left,(unsigned)info.top,(unsigned)info.crop_width,(unsigned)info.crop_height,info.workspace_peak,context.output_bytes);
    assert(!fclose(context.input));assert(!fclose(context.output));return status==atoi(argv[6]) ? 0:1;
}

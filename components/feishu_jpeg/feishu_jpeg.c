#include "feishu_jpeg.h"
#include "tjpgd.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    JDEC decoder;
    feishu_jpeg_io_t io;
    feishu_image_info_t info;
    uint16_t xmap[FEISHU_PREVIEW_WIDTH],ymap[FEISHU_PREVIEW_HEIGHT];
    _Alignas(4) uint8_t work[4096];
    uint8_t *strip;
    size_t input_size;
    unsigned next_y,rows,buffered,row_top;
    uint8_t previous,last;
    int error;
    bool eof,have_row;
} context_t;
static bool cancelled(context_t *c)
{
    if(c->io.cancel && c->io.cancel(c->io.context))c->error=FEISHU_JPEG_CANCELLED;
    return c->error!=FEISHU_JPEG_OK;
}
static size_t input(JDEC *decoder,uint8_t *buffer,size_t size)
{
    context_t *c=decoder->device;size_t done=0;uint8_t discard[512];
    while(done<size && !c->eof && !cancelled(c)) {
        size_t n=size-done;if(n>sizeof(discard))n=sizeof(discard);
        size_t available=FEISHU_JPEG_INPUT_LIMIT-c->input_size;
        if(n>available+1)n=available+1;
        uint8_t *p=buffer ? buffer+done:discard;
        int got=c->io.read(c->io.context,p,n);
        if(got<0 || (size_t)got>n) {c->error=FEISHU_JPEG_IO;break;}
        if(!got) {c->eof=true;break;}
        if((size_t)got>available) {c->error=FEISHU_JPEG_TOO_LARGE;break;}
        c->input_size+=(size_t)got;
        for(int i=0;i<got;++i) {c->previous=c->last;c->last=p[i];}
        done+=(size_t)got;
    }
    return done;
}
static bool flush(context_t *c)
{
    if(!c->buffered)return true;
    size_t bytes=(size_t)c->buffered*c->info.width*2;
    if(!c->io.write(c->io.context,c->strip,bytes)) {c->error=FEISHU_JPEG_STORAGE;return false;}
    c->next_y+=c->buffered;c->buffered=0;return true;
}
static unsigned lower_bound(const uint16_t *map,unsigned length,unsigned value)
{
    unsigned lo=0,hi=length;
    while(lo<hi) {unsigned mid=lo+(hi-lo)/2;if(map[mid]<value)lo=mid+1;else hi=mid;}
    return lo;
}
static int output(JDEC *decoder,void *bitmap,JRECT *rect)
{
    context_t *c=decoder->device;if(cancelled(c))return 0;
    if(!c->have_row || rect->top!=c->row_top) {
        if(c->have_row)c->buffered+=c->rows;
        if(c->buffered+(16u>>decoder->scale)>16u && !flush(c))return 0;
        c->have_row=true;c->row_top=rect->top;c->rows=0;
        while(c->next_y+c->buffered+c->rows<c->info.height &&
              c->ymap[c->next_y+c->buffered+c->rows]<=rect->bottom)++c->rows;
        if(c->rows>(16u>>decoder->scale)) {c->error=FEISHU_JPEG_INVALID;return 0;}
    }
    unsigned begin=lower_bound(c->xmap,c->info.width,rect->left);
    unsigned end=lower_bound(c->xmap,c->info.width,(unsigned)rect->right+1);
    unsigned stride=(unsigned)rect->right-rect->left+1;
    const uint16_t *pixels=bitmap;
    for(unsigned y=0;y<c->rows;++y) {
        unsigned source_y=c->ymap[c->next_y+c->buffered+y];
        if(source_y<rect->top || source_y>rect->bottom) {c->error=FEISHU_JPEG_INVALID;return 0;}
        for(unsigned x=begin;x<end;++x) {
            size_t source=(source_y-rect->top)*stride+c->xmap[x]-rect->left;
            uint16_t pixel=pixels[source];size_t dest=((size_t)(c->buffered+y)*c->info.width+x)*2;
            c->strip[dest]=(uint8_t)pixel;c->strip[dest+1]=(uint8_t)(pixel>>8);
        }
    }
    return 1;
}
static bool describe(uint32_t w,uint32_t h,const feishu_image_view_t *view,
                     feishu_image_info_t *info,uint32_t *actual_scale)
{
    if(!view || !info)return false;
    if(!w || !h || w>8192 || h>8192 || (uint64_t)w*h>FEISHU_JPEG_PIXEL_LIMIT)return false;
    uint32_t scale_x=(uint32_t)(((uint64_t)w<<16)+FEISHU_PREVIEW_WIDTH-1)/FEISHU_PREVIEW_WIDTH;
    uint32_t scale_y=(uint32_t)(((uint64_t)h<<16)+FEISHU_PREVIEW_HEIGHT-1)/FEISHU_PREVIEW_HEIGHT;
    uint32_t scale=scale_x>scale_y ? scale_x:scale_y;if(scale<65536)scale=65536;
    unsigned max_zoom=0;while(max_zoom<16 && (scale>>max_zoom)>65536)++max_zoom;
    unsigned zoom=view->zoom>max_zoom ? max_zoom:view->zoom;
    scale>>=zoom;if(scale<65536)scale=65536;
    uint32_t crop_w=(uint32_t)(((uint64_t)FEISHU_PREVIEW_WIDTH*scale)>>16);
    uint32_t crop_h=(uint32_t)(((uint64_t)FEISHU_PREVIEW_HEIGHT*scale)>>16);
    if(crop_w>w)crop_w=w;
    if(crop_h>h)crop_h=h;
    int64_t x=view->center_x<0 ? w/2:view->center_x,y=view->center_y<0 ? h/2:view->center_y;
    x-=crop_w/2;y-=crop_h/2;
    if(x<0)x=0;
    if(y<0)y=0;
    if(x>w-crop_w)x=w-crop_w;
    if(y>h-crop_h)y=h-crop_h;
    *info=(feishu_image_info_t){.original_width=w,.original_height=h,.left=(uint32_t)x,.top=(uint32_t)y,
        .crop_width=crop_w,.crop_height=crop_h,.width=(uint32_t)(((uint64_t)crop_w<<16)/scale),
        .height=(uint32_t)(((uint64_t)crop_h<<16)/scale),.zoom=zoom};
    if(!info->width || !info->height || info->width>FEISHU_PREVIEW_WIDTH || info->height>FEISHU_PREVIEW_HEIGHT) {
        return false;
    }
    if(actual_scale)*actual_scale=scale;
    return true;
}
bool feishu_jpeg_view_info(uint32_t width,uint32_t height,const feishu_image_view_t *view,
                            feishu_image_info_t *info)
{ return describe(width,height,view,info,NULL); }
bool feishu_jpeg_map_pixel(const feishu_preview_map_t *view,const feishu_preview_map_t *source,
                            uint32_t x,uint32_t y,uint32_t *source_x,uint32_t *source_y)
{
    if(!view || !source || !source_x || !source_y || !view->width || !view->height ||
       !source->width || !source->height || x>=view->width || y>=view->height ||
       !source->crop_width || !source->crop_height || !view->crop_width || !view->crop_height ||
       view->width>216 || view->height>226 || source->width>216 || source->height>226 ||
       view->left>8192 || view->top>8192 || source->left>8192 || source->top>8192 ||
       view->crop_width>8192-view->left || view->crop_height>8192-view->top ||
       source->crop_width>8192-source->left || source->crop_height>8192-source->top)return false;
    uint64_t original_x=view->left+(uint64_t)x*view->crop_width/view->width;
    uint64_t original_y=view->top+(uint64_t)y*view->crop_height/view->height;
    uint64_t sx=original_x>source->left ? (original_x-source->left)*source->width/source->crop_width:0;
    uint64_t sy=original_y>source->top ? (original_y-source->top)*source->height/source->crop_height:0;
    *source_x=sx>=source->width ? source->width-1:(uint32_t)sx;
    *source_y=sy>=source->height ? source->height-1:(uint32_t)sy;
    return true;
}
static bool geometry(context_t *c,const feishu_image_view_t *view)
{
    uint32_t w=c->decoder.width,h=c->decoder.height,scale;
    if(!describe(w,h,view,&c->info,&scale)) {c->error=FEISHU_JPEG_TOO_LARGE;return false;}
    unsigned native=0;while(native<3 && (UINT32_C(65536)<<(native+1))<=scale)++native;
    c->decoder.scale=(uint8_t)native;
    unsigned native_w=w>>native,native_h=h>>native;
    if(!native_w || !native_h) {c->error=FEISHU_JPEG_INVALID;return false;}
    for(unsigned i=0;i<c->info.width;++i) {
        uint32_t position=(c->info.left+(uint32_t)(((uint64_t)i*scale)>>16))>>native;
        c->xmap[i]=(uint16_t)(position>=native_w ? native_w-1:position);
    }
    for(unsigned i=0;i<c->info.height;++i) {
        uint32_t position=(c->info.top+(uint32_t)(((uint64_t)i*scale)>>16))>>native;
        c->ymap[i]=(uint16_t)(position>=native_h ? native_h-1:position);
    }
    /* Coalesce completed MCU rows within the existing worst-case strip bound.
     * Storage can commit several blocks without another frame-sized buffer. */
    size_t strip_size=(size_t)c->info.width*16u*2u;
    c->strip=malloc(strip_size);
    if(!c->strip) {c->error=FEISHU_JPEG_NO_MEM;return false;}
    c->info.workspace_peak=sizeof(*c)+strip_size;
    return true;
}
uint32_t feishu_jpeg_max_zoom(uint32_t width,uint32_t height)
{
    if(!width || !height || width>8192 || height>8192)return 0;
    uint32_t x=(uint32_t)((((uint64_t)width<<16)+FEISHU_PREVIEW_WIDTH-1)/FEISHU_PREVIEW_WIDTH);
    uint32_t y=(uint32_t)((((uint64_t)height<<16)+FEISHU_PREVIEW_HEIGHT-1)/FEISHU_PREVIEW_HEIGHT);
    uint32_t scale=x>y ? x:y;unsigned zoom=0;
    while(zoom<16 && (scale>>zoom)>65536)++zoom;
    return zoom;
}
int feishu_jpeg_decode(const feishu_jpeg_io_t *io,const feishu_image_view_t *view,feishu_image_info_t *info)
{
    if(!io || !io->read || !io->begin || !io->write || !view || !info)return FEISHU_JPEG_INVALID;
    memset(info,0,sizeof(*info));context_t *c=calloc(1,sizeof(*c));if(!c)return FEISHU_JPEG_NO_MEM;
    c->io=*io;
    JRESULT result=jd_prepare(&c->decoder,input,c->work,sizeof(c->work),c);
    if(result!=JDR_OK)c->error=c->error ? c->error:result==JDR_FMT3 || result==JDR_FMT2 ? FEISHU_JPEG_UNSUPPORTED:FEISHU_JPEG_INVALID;
    if(!c->error && geometry(c,view)) {
        if(!io->begin(io->context,&c->info))c->error=FEISHU_JPEG_STORAGE;
        if(!c->error) {
            result=jd_decomp(&c->decoder,output,c->decoder.scale);
            if(result!=JDR_OK)c->error=c->error ? c->error:FEISHU_JPEG_INVALID;
            if(!c->error) {
                c->buffered+=c->rows;c->rows=0;
                if(flush(c) && c->next_y!=c->info.height)c->error=FEISHU_JPEG_INVALID;
            }
        }
    }
    /* The decompressor may finish before the EOI reaches its input buffer.
     * Drain through the same bounded reader and reject truncated HTTP/JPEGs. */
    while(!c->error && !c->eof) {uint8_t tail[512];(void)input(&c->decoder,tail,sizeof(tail));}
    if(!c->error && (c->previous!=0xff || c->last!=0xd9))c->error=FEISHU_JPEG_INVALID;
    if(!c->error)*info=c->info;
    int error=c->error;free(c->strip);memset(c,0,sizeof(*c));free(c);return error;
}

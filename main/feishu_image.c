#include "feishu_image.h"
#include "feishu_cache.h"
#include "feishu_http.h"
#include "esp_timer.h"
#include "mbedtls/sha256.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

_Static_assert(sizeof(feishu_image_record_t)==48,"preview metadata version 1");
_Static_assert(sizeof(lv_image_header_t)==12,"LVGL binary preview format");
#if LV_BIN_DECODER_RAM_LOAD
#error "Image previews require the bounded scanline binary decoder"
#endif
#define ORIGINAL_CACHE_LIMIT (90u*1024u)
typedef struct {
    feishu_data_writer_t writer;
    uint8_t block[1024];
    size_t used;
    esp_err_t error;
    bool compress;
} stream_t;
typedef struct {
    stream_t preview,original;
    bool capture_original,timed_out;
    bool (*cancel)(void *);
    void *cancel_context;
    int64_t deadline,source_id,source_pin;
    uint64_t source_size,source_offset;
    unsigned source_chunks;
    uint8_t signature[8];
    size_t signature_size;
    char source_name[64];
    feishu_http_read_t network_read;
    void *network_context;
    feishu_image_view_t view;
    feishu_image_record_t record;
} image_context_t;
static bool stream_begin(stream_t *s,bool compress)
{
    memset(s,0,sizeof(*s));s->compress=compress;
    s->error=feishu_cache_blob_begin(FEISHU_DATA_IMAGE,&s->writer);return s->error==ESP_OK;
}
static bool stream_write(stream_t *s,const void *data,size_t size)
{
    const uint8_t *p=data;
    while(size && s->error==ESP_OK) {
        if(!s->used && size>=sizeof(s->block)) {
            size_t n=size/sizeof(s->block)*sizeof(s->block);
            if(n>8*sizeof(s->block))n=8*sizeof(s->block);
            s->error=feishu_cache_blob_append(&s->writer,p,n,s->compress);
            p+=n;size-=n;continue;
        }
        size_t n=sizeof(s->block)-s->used;if(n>size)n=size;
        memcpy(s->block+s->used,p,n);s->used+=n;p+=n;size-=n;
        if(s->used==sizeof(s->block)) {s->error=feishu_cache_blob_append(&s->writer,s->block,s->used,s->compress);s->used=0;}
    }
    return s->error==ESP_OK;
}
static bool stream_finish(stream_t *s)
{
    if(s->error==ESP_OK && s->used) {s->error=feishu_cache_blob_append(&s->writer,s->block,s->used,s->compress);s->used=0;}
    return s->error==ESP_OK;
}
static void streams_abort(image_context_t *c)
{
    feishu_cache_text_abort(&c->preview.writer);feishu_cache_text_abort(&c->original.writer);
    feishu_cache_unpin(&c->source_pin);
    memset(&c->preview,0,sizeof(c->preview));memset(&c->original,0,sizeof(c->original));
    c->capture_original=false;
}
static bool cancel_image(void *context)
{
    image_context_t *c=context;
    if(esp_timer_get_time()>c->deadline) {c->timed_out=true;return true;}
    return c->cancel && c->cancel(c->cancel_context);
}
static int jpeg_read(void *context,void *data,size_t capacity)
{
    image_context_t *c=context;int got;
    if(c->network_read)got=c->network_read(c->network_context,data,capacity);
    else {
        /* The original-file tee is inactive for a cached source. Reuse its
         * existing 1 KiB buffer rather than reading/CRC-checking the same SQL
         * chunk twice for the decoder's smaller reads. No extra read buffer. */
        size_t skip=(size_t)(c->source_offset%sizeof(c->original.block));
        if(!skip || !c->original.used) {
            c->original.used=0;
            esp_err_t err=feishu_cache_blob_read(c->source_name,c->source_id,c->source_offset,
                c->original.block,sizeof(c->original.block),&c->original.used);
            if(err!=ESP_OK)return -1;
        }
        if(c->original.used<skip)return -1;
        size_t size=c->original.used-skip;if(size>capacity)size=capacity;
        memcpy(data,c->original.block+skip,size);c->source_offset+=size;got=(int)size;
        if(++c->source_chunks%8==0)vTaskDelay(1);
    }
    if(got>0 && c->capture_original && !stream_write(&c->original,data,(size_t)got)) {
        /* Original retention is optional. A failed cache tee must not turn a
         * decodable large image into an error or consume a whole-file buffer. */
        feishu_cache_text_abort(&c->original.writer);c->capture_original=false;
    }
    if(got>0 && c->signature_size<sizeof(c->signature)) {
        size_t n=sizeof(c->signature)-c->signature_size;if(n>(size_t)got)n=(size_t)got;
        memcpy(c->signature+c->signature_size,data,n);c->signature_size+=n;
    }
    return got;
}
static bool jpeg_begin(void *context,const feishu_image_info_t *info)
{
    image_context_t *c=context;
    c->record=(feishu_image_record_t){FEISHU_IMAGE_RECORD_MAGIC,1,info->original_width,info->original_height,
        info->left,info->top,info->crop_width,info->crop_height,info->width,info->height,info->zoom,(uint32_t)info->workspace_peak};
    lv_image_header_t header={.magic=LV_IMAGE_HEADER_MAGIC,.cf=LV_COLOR_FORMAT_RGB565,
        .w=info->width,.h=info->height,.stride=info->width*2};
    return stream_begin(&c->preview,true) && stream_write(&c->preview,&c->record,sizeof(c->record)) &&
        stream_write(&c->preview,&header,sizeof(header));
}
static bool jpeg_write(void *context,const void *data,size_t size)
{ return stream_write(&((image_context_t *)context)->preview,data,size); }
static esp_err_t decode(image_context_t *c)
{
    feishu_jpeg_io_t io={jpeg_read,jpeg_begin,jpeg_write,cancel_image,c};feishu_image_info_t info;
    int status=feishu_jpeg_decode(&io,&c->view,&info);
    if(c->timed_out)return ESP_ERR_TIMEOUT;
    if(c->preview.error!=ESP_OK)return c->preview.error;
    if(status==FEISHU_JPEG_CANCELLED)return FEISHU_HTTP_ERR_CANCELLED;
    if(status==FEISHU_JPEG_NO_MEM)return ESP_ERR_NO_MEM;
    if(status==FEISHU_JPEG_TOO_LARGE)return ESP_ERR_INVALID_SIZE;
    if(status==FEISHU_JPEG_UNSUPPORTED)return ESP_ERR_NOT_SUPPORTED;
    if(status!=FEISHU_JPEG_OK) {
        if((c->signature_size>=8 && !memcmp(c->signature,"\x89PNG\r\n\x1a\n",8)) ||
           (c->signature_size>=3 && !memcmp(c->signature,"GIF",3)))return ESP_ERR_NOT_SUPPORTED;
        return ESP_ERR_INVALID_RESPONSE;
    }
    if(!stream_finish(&c->preview))return c->preview.error;
    if(c->capture_original && !stream_finish(&c->original)) {
        feishu_cache_text_abort(&c->original.writer);c->capture_original=false;
    }
    return ESP_OK;
}
static esp_err_t consume_image(void *context,feishu_http_read_t read,void *read_context,int64_t expected)
{
    image_context_t *c=context;c->network_read=read;c->network_context=read_context;
    c->signature_size=0;
    if(expected>0 && expected<=ORIGINAL_CACHE_LIMIT)c->capture_original=stream_begin(&c->original,false);
    return decode(c);
}
static bool record_valid(const feishu_image_record_t *r,uint64_t size)
{
    return r->magic==FEISHU_IMAGE_RECORD_MAGIC && r->version==1 && r->width && r->height &&
        r->width<=FEISHU_PREVIEW_WIDTH && r->height<=FEISHU_PREVIEW_HEIGHT && r->zoom<=16 &&
        r->original_width && r->original_height && r->original_width<=8192 && r->original_height<=8192 &&
        (uint64_t)r->original_width*r->original_height<=FEISHU_JPEG_PIXEL_LIMIT && r->crop_width && r->crop_height &&
        (uint64_t)r->left+r->crop_width<=r->original_width && (uint64_t)r->top+r->crop_height<=r->original_height &&
        size==sizeof(*r)+sizeof(lv_image_header_t)+(uint64_t)r->width*r->height*2;
}
esp_err_t feishu_image_read_record(const char *name,int64_t id,uint64_t size,feishu_image_record_t *record)
{
    if(!record)return ESP_ERR_INVALID_ARG;
    size_t got=0;esp_err_t err=feishu_cache_blob_read(name,id,0,record,sizeof(*record),&got);
    if(err!=ESP_OK || got!=sizeof(*record) || !record_valid(record,size))return ESP_ERR_INVALID_RESPONSE;
    lv_image_header_t header;
    err=feishu_cache_blob_read(name,id,sizeof(*record),&header,sizeof(header),&got);
    if(err!=ESP_OK || got!=sizeof(header) || header.magic!=LV_IMAGE_HEADER_MAGIC || header.cf!=LV_COLOR_FORMAT_RGB565 ||
       header.w!=record->width || header.h!=record->height || header.stride!=record->width*2 || header.flags || header.reserved_2)
        return ESP_ERR_INVALID_RESPONSE;
    return ESP_OK;
}
static bool asset_hash(const char *message,const char *resource,char output[33])
{
    uint8_t digest[32];mbedtls_sha256_context hash;mbedtls_sha256_init(&hash);
    int rc=mbedtls_sha256_starts(&hash,0);
    if(!rc)rc=mbedtls_sha256_update(&hash,(const uint8_t *)message,strlen(message)+1);
    if(!rc)rc=mbedtls_sha256_update(&hash,(const uint8_t *)resource,strlen(resource)+1);
    if(!rc)rc=mbedtls_sha256_finish(&hash,digest);
    mbedtls_sha256_free(&hash);if(rc)return false;
    static const char hex[]="0123456789abcdef";
    for(unsigned i=0;i<16;++i) {output[i*2]=hex[digest[i]>>4];output[i*2+1]=hex[digest[i]&15];}
    output[32]=0;return true;
}
esp_err_t feishu_image_cached(const char *message,const char *resource,const feishu_image_view_t *view,feishu_prepared_image_t *image)
{
    if(!message || !*message || !resource || !*resource || !view || !image || view->zoom>16 ||
       view->center_x < -1 || view->center_y < -1 || view->center_x>8192 || view->center_y>8192)return ESP_ERR_INVALID_ARG;
    memset(image,0,sizeof(*image));char asset[33];if(!asset_hash(message,resource,asset))return ESP_FAIL;
    snprintf(image->name,sizeof(image->name),"image/%s/%u_%ld_%ld",asset,(unsigned)view->zoom,
        (long)(view->zoom ? view->center_x:-1),(long)(view->zoom ? view->center_y:-1));
    int64_t id=0;uint64_t size=0;esp_err_t err=feishu_cache_blob_find(image->name,&id,&size);
    if(err==ESP_OK)err=feishu_image_read_record(image->name,id,size,&image->info);
    if(err==ESP_OK)err=feishu_cache_blob_pin(image->name,id,&image->pin,&size);
    if(err==ESP_OK) {image->id=id;image->size=size;image->cached=true;}
    return err;
}
esp_err_t feishu_image_quick_view(const char *message,const char *resource,const feishu_image_view_t *view,
                                  feishu_prepared_image_t *image)
{
    if(!view || !image)return ESP_ERR_INVALID_ARG;
    const feishu_image_view_t overview={0,-1,-1};
    esp_err_t err=feishu_image_cached(message,resource,&overview,image);
    if(err!=ESP_OK)return err;
    feishu_image_info_t geometry;
    if(!feishu_jpeg_view_info(image->info.original_width,image->info.original_height,view,&geometry)) {
        feishu_cache_unpin(&image->pin);return ESP_ERR_INVALID_SIZE;
    }
    image->source_info=image->info;
    image->info=(feishu_image_record_t){FEISHU_IMAGE_RECORD_MAGIC,1,geometry.original_width,geometry.original_height,
        geometry.left,geometry.top,geometry.crop_width,geometry.crop_height,geometry.width,geometry.height,geometry.zoom,0};
    image->approximate=true;return ESP_OK;
}
esp_err_t feishu_image_prepare(const char *url,const char *token,const char *message,const char *resource,
                                const feishu_image_view_t *view,bool allow_network,
                                bool (*cancel)(void *),void *cancel_context,
                                feishu_prepared_image_t *image,int *http_status)
{
    if(!url || !token || !message || !resource || !view || !image || !http_status || view->zoom>16 ||
       view->center_x < -1 || view->center_y < -1 || view->center_x>8192 || view->center_y>8192)return ESP_ERR_INVALID_ARG;
    memset(image,0,sizeof(*image));*http_status=0;
    if(cancel && cancel(cancel_context))return FEISHU_HTTP_ERR_CANCELLED;
    esp_err_t cached=feishu_image_cached(message,resource,view,image);
    if(cached==ESP_OK)return ESP_OK;
    if(cached==ESP_ERR_INVALID_ARG || cached==ESP_ERR_NO_MEM)return cached;
    char asset[33];if(!asset_hash(message,resource,asset))return ESP_FAIL;
    image_context_t *c=calloc(1,sizeof(*c));if(!c)return ESP_ERR_NO_MEM;
    c->view=*view;c->cancel=cancel;c->cancel_context=cancel_context;c->deadline=esp_timer_get_time()+30000000;
    snprintf(c->source_name,sizeof(c->source_name),"image_src/%s",asset);
    esp_err_t err=ESP_ERR_NOT_FOUND;
    if(feishu_cache_blob_find(c->source_name,&c->source_id,&c->source_size)==ESP_OK && c->source_size<=ORIGINAL_CACHE_LIMIT &&
       feishu_cache_blob_pin(c->source_name,c->source_id,&c->source_pin,&c->source_size)==ESP_OK)err=decode(c);
    if(err!=ESP_OK && err!=FEISHU_HTTP_ERR_CANCELLED && allow_network && !c->timed_out) {
        streams_abort(c);c->source_id=0;c->source_offset=0;
        size_t length=0;
        err=!token[0] ? FEISHU_IMAGE_NEEDS_ACCESS:
            feishu_http_get_consume(url,token,FEISHU_JPEG_INPUT_LIMIT,consume_image,c,cancel_image,c,http_status,&length);
        if(c->timed_out)err=ESP_ERR_TIMEOUT;
    }
    if(err==ESP_OK) {
        err=feishu_cache_blob_commit(&c->preview.writer,image->name,c->capture_original ? &c->original.writer:NULL,
            c->capture_original ? c->source_name:NULL,&image->pin);
        if(err==ESP_OK) {image->id=image->pin;image->info=c->record;image->size=sizeof(c->record)+sizeof(lv_image_header_t)+(uint64_t)c->record.width*c->record.height*2;}
    }
    streams_abort(c);feishu_cache_trim();memset(c,0,sizeof(*c));free(c);return err;
}

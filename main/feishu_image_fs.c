#include "feishu_image_fs.h"
#include "feishu_cache.h"
#include "lvgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char name[96];
    int64_t id,pin;
    uint32_t scope,size,position;
    uint64_t cache_base;
    size_t cache_size;
    uint8_t cache[1024];
    bool approximate;
    feishu_preview_map_t view,source;
    lv_image_header_t header;
} file_t;
static lv_fs_drv_t driver;
static bool initialized;
static char selected_name[96];
static int64_t selected_id;
static uint64_t selected_size;
static bool selected_approximate;
static feishu_preview_map_t selected_view,selected_source;
static char selected_path[40];
static uint32_t selected_revision;
static bool ready(lv_fs_drv_t *drv) {(void)drv;return selected_id>0;}
static void *open_file(lv_fs_drv_t *drv,const char *path,lv_fs_mode_t mode)
{
    (void)drv;
    if(mode!=LV_FS_MODE_RD || !selected_id || strcmp(path,selected_path) || selected_size<=sizeof(feishu_image_record_t) ||
       selected_size>sizeof(feishu_image_record_t)+12+FEISHU_PREVIEW_WIDTH*FEISHU_PREVIEW_HEIGHT*2)return NULL;
    file_t *file=calloc(1,sizeof(*file));if(!file)return NULL;
    memcpy(file->name,selected_name,sizeof(file->name));file->id=selected_id;
    file->scope=feishu_cache_scope_version();uint64_t size=0;
    if(feishu_cache_blob_pin(file->name,file->id,&file->pin,&size)!=ESP_OK || size!=selected_size) {
        feishu_cache_unpin(&file->pin);free(file);return NULL;
    }
    file->size=(uint32_t)(size-sizeof(feishu_image_record_t));file->cache_base=UINT64_MAX;
    if(selected_approximate) {
        file->approximate=true;file->view=selected_view;file->source=selected_source;
        file->header=(lv_image_header_t){.magic=LV_IMAGE_HEADER_MAGIC,.cf=LV_COLOR_FORMAT_RGB565,
            .w=file->view.width,.h=file->view.height,.stride=file->view.width*2};
        file->size=sizeof(file->header)+file->view.width*file->view.height*2;
    }
    return file;
}
static lv_fs_res_t close_file(lv_fs_drv_t *drv,void *data)
{
    (void)drv;file_t *file=data;if(!file)return LV_FS_RES_INV_PARAM;
    feishu_cache_unpin(&file->pin);feishu_cache_trim();
    memset(file,0,sizeof(*file));free(file);return LV_FS_RES_OK;
}
static lv_fs_res_t read_file(lv_fs_drv_t *drv,void *data,void *buffer,uint32_t capacity,uint32_t *size)
{
    (void)drv;file_t *file=data;
    if(!file || !buffer || !size)return LV_FS_RES_INV_PARAM;
    *size=0;if(file->scope!=feishu_cache_scope_version())return LV_FS_RES_DENIED;
    if(capacity>file->size-file->position)capacity=file->size-file->position;
    if(file->approximate) {
        uint8_t *out=buffer;
        while(*size<capacity) {
            if(file->position<sizeof(file->header)) {
                size_t n=sizeof(file->header)-file->position;if(n>capacity-*size)n=capacity-*size;
                memcpy(out+*size,(uint8_t *)&file->header+file->position,n);
                file->position+=(uint32_t)n;*size+=(uint32_t)n;continue;
            }
            uint32_t byte=file->position-sizeof(file->header),pixel=byte/2,sx,sy;
            if(!feishu_jpeg_map_pixel(&file->view,&file->source,pixel%file->view.width,pixel/file->view.width,&sx,&sy))
                return LV_FS_RES_FS_ERR;
            uint64_t physical=sizeof(feishu_image_record_t)+sizeof(lv_image_header_t)+
                ((uint64_t)sy*file->source.width+sx)*2;
            uint64_t base=physical/sizeof(file->cache)*sizeof(file->cache);
            if(file->cache_base!=base) {
                file->cache_size=0;
                if(feishu_cache_blob_read(file->name,file->id,base,file->cache,sizeof(file->cache),&file->cache_size)!=ESP_OK)
                    return LV_FS_RES_FS_ERR;
                file->cache_base=base;
            }
            size_t skip=(size_t)(physical-base),phase=byte%2,n=2-phase;
            if(skip+2>file->cache_size)return LV_FS_RES_FS_ERR;
            if(n>capacity-*size)n=capacity-*size;
            memcpy(out+*size,file->cache+skip+phase,n);file->position+=(uint32_t)n;*size+=(uint32_t)n;
        }
        return LV_FS_RES_OK;
    }
    while(*size<capacity) {
        uint64_t physical=(uint64_t)file->position+sizeof(feishu_image_record_t);
        uint64_t base=physical/sizeof(file->cache)*sizeof(file->cache);
        if(file->cache_base!=base) {
            file->cache_size=0;
            if(feishu_cache_blob_read(file->name,file->id,base,file->cache,sizeof(file->cache),&file->cache_size)!=ESP_OK)
                return LV_FS_RES_FS_ERR;
            file->cache_base=base;
        }
        size_t skip=(size_t)(physical-base);
        if(file->cache_size<=skip)return LV_FS_RES_FS_ERR;
        size_t n=file->cache_size-skip;if(n>capacity-*size)n=capacity-*size;
        memcpy((uint8_t *)buffer+*size,file->cache+skip,n);file->position+=(uint32_t)n;*size+=(uint32_t)n;
    }
    return LV_FS_RES_OK;
}
static lv_fs_res_t seek_file(lv_fs_drv_t *drv,void *data,uint32_t position,lv_fs_whence_t whence)
{
    (void)drv;file_t *file=data;if(!file)return LV_FS_RES_INV_PARAM;
    int64_t next=whence==LV_FS_SEEK_SET ? position:whence==LV_FS_SEEK_CUR ?
        (int64_t)file->position+(int32_t)position:whence==LV_FS_SEEK_END ? (int64_t)file->size+(int32_t)position:-1;
    if(next<0 || next>file->size)return LV_FS_RES_INV_PARAM;
    file->position=(uint32_t)next;return LV_FS_RES_OK;
}
static lv_fs_res_t tell_file(lv_fs_drv_t *drv,void *data,uint32_t *position)
{
    (void)drv;if(!data || !position)return LV_FS_RES_INV_PARAM;
    *position=((file_t *)data)->position;return LV_FS_RES_OK;
}
esp_err_t feishu_image_fs_init(void)
{
    if(initialized)return ESP_OK;
    if(lv_fs_get_drv('F'))return ESP_ERR_INVALID_STATE;
    lv_fs_drv_init(&driver);driver.letter='F';driver.ready_cb=ready;driver.open_cb=open_file;
    driver.close_cb=close_file;driver.read_cb=read_file;driver.seek_cb=seek_file;driver.tell_cb=tell_file;
    driver.cache_size=0;lv_fs_drv_register(&driver);initialized=true;return ESP_OK;
}
esp_err_t feishu_image_fs_select(const feishu_prepared_image_t *image,char path[48])
{
    if(!image || !path || image->id<=0 || !memchr(image->name,0,sizeof(image->name)))return ESP_ERR_INVALID_ARG;
    esp_err_t err=feishu_image_fs_init();if(err!=ESP_OK)return err;
    if(image->approximate) {
        const feishu_image_record_t *v=&image->info,*s=&image->source_info;
        if(!v->width || !v->height || v->width>216 || v->height>226 || !s->width || !s->height ||
           s->width>216 || s->height>226 || !v->crop_width || !v->crop_height ||
           !s->crop_width || !s->crop_height || image->size!=sizeof(*s)+sizeof(lv_image_header_t)+(uint64_t)s->width*s->height*2)
            return ESP_ERR_INVALID_ARG;
        selected_view=(feishu_preview_map_t){v->width,v->height,v->left,v->top,v->crop_width,v->crop_height};
        selected_source=(feishu_preview_map_t){s->width,s->height,s->left,s->top,s->crop_width,s->crop_height};
        snprintf(selected_path,sizeof(selected_path),"%lld~%lu.bin",(long long)image->id,(unsigned long)++selected_revision);
    } else snprintf(selected_path,sizeof(selected_path),"%lld.bin",(long long)image->id);
    memcpy(selected_name,image->name,sizeof(selected_name));selected_id=image->id;selected_size=image->size;
    selected_approximate=image->approximate;
    snprintf(path,48,"F:%s",selected_path);return ESP_OK;
}

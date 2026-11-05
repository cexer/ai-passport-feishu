#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FEISHU_PREVIEW_WIDTH 216u
#define FEISHU_PREVIEW_HEIGHT 226u
#define FEISHU_JPEG_INPUT_LIMIT (8u*1024u*1024u)
#define FEISHU_JPEG_PIXEL_LIMIT (16u*1024u*1024u)
typedef struct {uint32_t zoom;int32_t center_x,center_y;} feishu_image_view_t;
typedef struct {
    uint32_t original_width,original_height,left,top,crop_width,crop_height,width,height,zoom;
    size_t workspace_peak;
} feishu_image_info_t;
enum {FEISHU_JPEG_OK,FEISHU_JPEG_INVALID,FEISHU_JPEG_UNSUPPORTED,FEISHU_JPEG_TOO_LARGE,
      FEISHU_JPEG_IO,FEISHU_JPEG_NO_MEM,FEISHU_JPEG_CANCELLED,FEISHU_JPEG_STORAGE};
typedef struct {
    /* Return bytes read, zero EOF, negative IO error; arbitrary short reads OK. */
    int (*read)(void *context,void *data,size_t capacity);
    bool (*begin)(void *context,const feishu_image_info_t *info);
    /* Sequential complete rows, at most 16 rows (6912 bytes) per callback. */
    bool (*write)(void *context,const void *data,size_t size);
    bool (*cancel)(void *context);
    void *context;
} feishu_jpeg_io_t;
/* Sequential RGB565 little-endian rows; never buffers the whole image. */
int feishu_jpeg_decode(const feishu_jpeg_io_t *io,const feishu_image_view_t *view,feishu_image_info_t *info);
uint32_t feishu_jpeg_max_zoom(uint32_t width,uint32_t height);
/* View geometry without decoding, allocating, or retaining a bitmap. */
bool feishu_jpeg_view_info(uint32_t width,uint32_t height,const feishu_image_view_t *view,
                            feishu_image_info_t *info);
typedef struct {uint32_t width,height,left,top,crop_width,crop_height;} feishu_preview_map_t;
/* Nearest-neighbor pixel mapping between two bounded views of one original. */
bool feishu_jpeg_map_pixel(const feishu_preview_map_t *view,const feishu_preview_map_t *source,
                            uint32_t x,uint32_t y,uint32_t *source_x,uint32_t *source_y);

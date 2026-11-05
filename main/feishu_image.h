#pragma once
#include "esp_err.h"
#include "feishu_jpeg.h"
#include <stdbool.h>
#include <stdint.h>

#define FEISHU_IMAGE_RECORD_MAGIC UINT32_C(0x314d4946)
#define FEISHU_IMAGE_NEEDS_ACCESS ((esp_err_t)0x7f05)
typedef struct {
    uint32_t magic,version,original_width,original_height,left,top,crop_width,crop_height,width,height,zoom,workspace_peak;
} feishu_image_record_t;
typedef struct {
    char name[96];
    int64_t id,pin;
    uint64_t size;
    feishu_image_record_t info;
    feishu_image_record_t source_info;
    bool cached,approximate;
} feishu_prepared_image_t;

esp_err_t feishu_image_read_record(const char *name,int64_t id,uint64_t size,feishu_image_record_t *info);
esp_err_t feishu_image_cached(const char *message,const char *resource,const feishu_image_view_t *view,feishu_prepared_image_t *image);
/* Derive a bounded low-resolution view from the already cached overview.
 * The virtual file maps its rows on demand; no decode or new bitmap is stored. */
esp_err_t feishu_image_quick_view(const char *message,const char *resource,const feishu_image_view_t *view,
                                  feishu_prepared_image_t *image);
esp_err_t feishu_image_prepare(const char *url,const char *token,const char *message,const char *resource,
                                const feishu_image_view_t *view,bool allow_network,
                                bool (*cancel)(void *),void *cancel_context,
                                feishu_prepared_image_t *image,int *http_status);

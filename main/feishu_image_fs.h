#pragma once
#include "feishu_image.h"
/* LVGL task only. Each opened decoder has an independent cache reader lease. */
esp_err_t feishu_image_fs_init(void);
esp_err_t feishu_image_fs_select(const feishu_prepared_image_t *image,char path[48]);

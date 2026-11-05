#pragma once
#include "feishu_data.h"
#include "esp_err.h"
typedef struct {uint32_t generation;uint64_t id_floor;bool held;} feishu_pool_info_t;
/* The cache serializes these platform operations. Backend-specific mounting,
 * physical capacity and recovery stay outside business cache operations. */
esp_err_t feishu_pool_mount(const char *directory);
void feishu_pool_unmount(void);
bool feishu_pool_room(size_t margin);
esp_err_t feishu_pool_open_data(feishu_data_t *store,const char *directory,
                                uint64_t *id_floor,bool broken,feishu_pool_info_t *info);
int feishu_pool_reclaim(feishu_data_t *store,int kind,size_t *bytes);

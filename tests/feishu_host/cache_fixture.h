#pragma once
/* Shared host port for the real cache adapter; no automatic formatting. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "sqlite3.h"
static char directory[128];
#define FEISHU_CACHE_DIRECTORY directory
#include "../../main/feishu_pool_sqlite.c"
#include "../../main/feishu_cache.c"
static unsigned lock_depth;
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void) {return (void *)1;}
int xSemaphoreTakeRecursive(SemaphoreHandle_t handle,unsigned timeout)
{ (void)timeout;assert(handle);++lock_depth;return 1; }
int xSemaphoreGiveRecursive(SemaphoreHandle_t handle) {assert(handle && lock_depth);--lock_depth;return 1;}
void vTaskDelay(unsigned ticks) {(void)ticks;}
esp_err_t esp_vfs_spiffs_register(const esp_vfs_spiffs_conf_t *cfg)
{ assert(!strcmp(cfg->base_path,directory) && !cfg->format_if_mount_failed);return ESP_OK; }
esp_err_t esp_vfs_spiffs_unregister(const char *label) {(void)label;return ESP_OK;}
esp_err_t esp_spiffs_format(const char *label) {(void)label;assert(0 && "must not format");return ESP_FAIL;}
esp_err_t esp_spiffs_info(const char *label,size_t *total,size_t *used)
{ (void)label;*total=1024*1024;*used=0;return ESP_OK; }
const esp_partition_t *esp_partition_find_first(int type,int subtype,const char *label)
{ (void)type;(void)subtype;(void)label;return NULL; }
esp_err_t esp_partition_read(const esp_partition_t *p,size_t o,void *d,size_t s)
{ (void)p;(void)o;(void)d;(void)s;return ESP_FAIL; }

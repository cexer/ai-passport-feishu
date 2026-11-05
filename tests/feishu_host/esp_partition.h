#pragma once
#include "esp_err.h"
#include <stddef.h>
#define ESP_PARTITION_TYPE_DATA 1
#define ESP_PARTITION_SUBTYPE_DATA_SPIFFS 0x82
typedef struct {size_t size;} esp_partition_t;
const esp_partition_t *esp_partition_find_first(int type,int subtype,const char *label);
esp_err_t esp_partition_read(const esp_partition_t *part,size_t offset,void *data,size_t size);

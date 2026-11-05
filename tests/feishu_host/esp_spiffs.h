#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
typedef struct {const char *base_path,*partition_label;size_t max_files;bool format_if_mount_failed;} esp_vfs_spiffs_conf_t;
esp_err_t esp_vfs_spiffs_register(const esp_vfs_spiffs_conf_t *config);
esp_err_t esp_vfs_spiffs_unregister(const char *label);
esp_err_t esp_spiffs_format(const char *label);
esp_err_t esp_spiffs_info(const char *label,size_t *total,size_t *used);

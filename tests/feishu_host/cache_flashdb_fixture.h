#pragma once
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "feishu_fdb_host.h"
static char directory[128]="raw-test";
#define FEISHU_CACHE_DIRECTORY directory
#include "../../main/feishu_pool_flashdb.c"
#include "../../main/feishu_cache.c"
static unsigned lock_depth;
static const feishu_fdb_io_t *host_io;
esp_err_t feishu_flash_port_open(feishu_fdb_io_t *io)
{if(!host_io)host_io=feishu_fdb_host_reset();*io=*host_io;return ESP_OK;}
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void) {return (void *)1;}
int xSemaphoreTakeRecursive(SemaphoreHandle_t handle,unsigned timeout)
{(void)timeout;assert(handle);++lock_depth;return 1;}
int xSemaphoreGiveRecursive(SemaphoreHandle_t handle)
{assert(handle && lock_depth);--lock_depth;return 1;}
void vTaskDelay(unsigned ticks) {(void)ticks;}

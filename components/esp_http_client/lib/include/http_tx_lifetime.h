/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
static inline bool esp_http_tx_acquire(char **data,size_t capacity,void *(*allocate)(size_t))
{
    if(*data)return true;
    if(!capacity)return false;
    *data=allocate(capacity);return *data!=NULL;
}
static inline void esp_http_tx_release(char **data,size_t capacity,void (*release)(void *))
{
    if(!*data)return;
    volatile unsigned char *p=(volatile unsigned char *)*data;
    for(size_t i=0;i<capacity;++i)p[i]=0;
    release(*data);*data=NULL;
}
static inline bool esp_http_tx_finish(bool enabled,bool get,size_t pending,size_t body,
                                      char **data,size_t capacity,void (*release)(void *))
{
    if(!enabled || !get || pending || body)return false;
    esp_http_tx_release(data,capacity,release);return true;
}

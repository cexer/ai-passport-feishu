#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* One atomic recovery record owns both the active index and saved networks.
 * Driver configs are opaque; their first 32 bytes are the ESP STA SSID. */
#define FEISHU_WIFI_SLOTS 10u
#define FEISHU_WIFI_BOOK_MAX 2048u
typedef struct { uint32_t magic; uint16_t version,size,count,active; uint32_t reserved; } feishu_wifi_book_header_t;
static inline bool feishu_wifi_book_valid(const void *data,size_t length,size_t size)
{
    if(!data || length<sizeof(feishu_wifi_book_header_t))return false;
    const feishu_wifi_book_header_t *h=data;
    return h->magic==0x57494632u && h->version==1 && h->size==size &&
        h->count<=FEISHU_WIFI_SLOTS && (h->count ? h->active<h->count:h->active==0) &&
        size>=32 && size<=(FEISHU_WIFI_BOOK_MAX-sizeof(*h))/FEISHU_WIFI_SLOTS &&
        length==sizeof(*h)+h->count*size;
}
static inline void *feishu_wifi_book_entry(void *data,size_t index)
{feishu_wifi_book_header_t *h=data;return (uint8_t *)data+sizeof(*h)+index*h->size;}
static inline bool feishu_wifi_book_import(void *data,size_t *length,size_t size)
{
    if(feishu_wifi_book_valid(data,*length,size))return true;
    if(*length!=size || size<32 || size>(FEISHU_WIFI_BOOK_MAX-sizeof(feishu_wifi_book_header_t))/FEISHU_WIFI_SLOTS)return false;
    memmove((uint8_t *)data+sizeof(feishu_wifi_book_header_t),data,size);
    feishu_wifi_book_header_t h={.magic=0x57494632u,.version=1,.size=(uint16_t)size,.count=1};
    memcpy(data,&h,sizeof(h));*length=sizeof(h)+size;return true;
}
static inline bool feishu_wifi_book_upsert(void *data,size_t *length,const void *config,size_t size)
{
    if(!config || size<32 || size>(FEISHU_WIFI_BOOK_MAX-sizeof(feishu_wifi_book_header_t))/FEISHU_WIFI_SLOTS)return false;
    if(!*length) {
        feishu_wifi_book_header_t h={.magic=0x57494632u,.version=1,.size=(uint16_t)size};memcpy(data,&h,sizeof(h));
    } else if(!feishu_wifi_book_import(data,length,size))return false;
    feishu_wifi_book_header_t *h=data;size_t i=0;
    while(i<h->count && strncmp(feishu_wifi_book_entry(data,i),config,32))++i;
    if(i==h->count) {if(h->count==FEISHU_WIFI_SLOTS)return false;++h->count;}
    memcpy(feishu_wifi_book_entry(data,i),config,size);h->active=(uint16_t)i;
    *length=sizeof(*h)+h->count*size;return true;
}
static inline bool feishu_wifi_book_remove(void *data,size_t *length,size_t index,size_t size)
{
    if(!feishu_wifi_book_import(data,length,size))return false;
    feishu_wifi_book_header_t *h=data;if(index>=h->count)return false;
    memmove(feishu_wifi_book_entry(data,index),feishu_wifi_book_entry(data,index+1),(h->count-index-1)*size);
    --h->count;
    if(!h->count || h->active==index)h->active=0;
    else if(h->active>index)--h->active;
    *length=sizeof(*h)+h->count*size;
    memset((uint8_t *)data+*length,0,size);return true;
}

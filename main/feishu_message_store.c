#include "feishu_message_store.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static bool begin(void *context,size_t slot)
{
    feishu_message_store_t *store=context;
    if(slot>=FEISHU_MAX_MESSAGES || store->scope!=feishu_cache_scope_version())return false;
    feishu_cache_text_abort(&store->bodies[slot]);
    bool okay=feishu_cache_text_begin(&store->bodies[slot])==ESP_OK;
    vTaskDelay(1);return okay;
}
static bool write_body(void *context,size_t slot,const uint8_t *data,size_t size)
{
    feishu_message_store_t *store=context;
    if(slot>=FEISHU_MAX_MESSAGES || store->scope!=feishu_cache_scope_version())return false;
    bool okay=feishu_cache_text_append(&store->bodies[slot],data,size)==ESP_OK;
    vTaskDelay(1);return okay;
}
static void reset(void *context,size_t slot)
{
    feishu_message_store_t *store=context;
    if(slot<FEISHU_MAX_MESSAGES)feishu_cache_text_abort(&store->bodies[slot]);
}
static void abort_slot(void *context,size_t slot)
{
    feishu_message_store_t *store=context;
    if(slot>=FEISHU_MAX_MESSAGES)return;
    feishu_cache_text_abort(&store->bodies[slot]);feishu_cache_text_abort(&store->maps[slot]);
    vTaskDelay(1);
}
static bool mention(void *context,size_t slot,const feishu_stream_mention_t *entry)
{
    feishu_message_store_t *store=context;char key[32];
    if(slot>=FEISHU_MAX_MESSAGES || !entry)return false;
    int size=snprintf(key,sizeof(key),"%s%s",entry->key[0]=='@' ? "":"@",entry->key);
    if(size<2 || (size_t)size>=sizeof(key))return false;
    bool okay=feishu_cache_text_add_mention(&store->maps[slot],store->scope,key,entry->name)==ESP_OK;
    vTaskDelay(1);return okay;
}
static bool prepare_body(void *context,size_t slot,bool valid,uint64_t *length,bool *mapped,
                            char *preview,size_t preview_capacity,void *workspace,size_t workspace_size)
{
    feishu_message_store_t *store=context;
    if(slot>=FEISHU_MAX_MESSAGES)return false;
    bool okay=feishu_cache_text_prepare(&store->bodies[slot],&store->maps[slot],
        store->scope,valid,preview,preview_capacity,workspace,workspace_size,length,mapped,
        store->cancelled,store->cancel_context)==ESP_OK;
    vTaskDelay(1);return okay;
}
static bool commit(void *context,size_t slot,const char *message,int64_t *id,uint64_t *length)
{
    feishu_message_store_t *store=context;if(slot>=FEISHU_MAX_MESSAGES)return false;
    if(store->cancelled && store->cancelled(store->cancel_context))return false;
    if(!store->maps[slot].id)*length=store->bodies[slot].raw;
    bool okay=feishu_cache_text_publish(&store->bodies[slot],&store->maps[slot],store->scope,*length,message,id)==ESP_OK;
    vTaskDelay(1);return okay;
}
void feishu_message_store_init(feishu_message_store_t *store,bool (*cancelled)(void *),void *context)
{
    memset(store,0,sizeof(*store));store->scope=feishu_cache_scope_version();
    store->cancelled=cancelled;store->cancel_context=context;
}
const feishu_stream_storage_t *feishu_message_store_operations(void)
{
    static const feishu_stream_storage_t operations={.begin=begin,.write=write_body,.reset=reset,
        .mention=mention,.prepare=prepare_body,.commit=commit,.abort=abort_slot};
    return &operations;
}

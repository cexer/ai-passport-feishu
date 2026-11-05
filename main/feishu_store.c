#include "feishu_store.h"
#include "feishu_store_legacy.h"
#include "feishu_root.h"
#include "feishu_cache.h"
#include "esp_random.h"
#include "nvs.h"
#include <stdlib.h>
#include <string.h>

enum { ROOT_MIGRATION,ROOT_CREDENTIALS,ROOT_ACCESS,ROOT_OWNER,ROOT_SEEN,ROOT_WIFI_SETUP,ROOT_WIFI,ROOT_AUTH_REQUEST,ROOT_VOICE };
typedef struct {
    uint32_t version,reserved;
    uint64_t epoch;
    char app_id[FEISHU_APP_ID_MAX],app_secret[FEISHU_APP_SECRET_MAX];
} credentials_header_t;
_Static_assert(sizeof(credentials_header_t)==208,"credential recovery format");
#ifndef FEISHU_STORE_DIRECTORY
#define FEISHU_STORE_DIRECTORY "/feishu"
#endif
static const feishu_root_t root={.directory=FEISHU_STORE_DIRECTORY};
static bool migrated;

static esp_err_t translate(int rc)
{
    switch(rc) {
    case FEISHU_ROOT_OK:return ESP_OK;
    case FEISHU_ROOT_MISSING:return ESP_ERR_NVS_NOT_FOUND;
    case FEISHU_ROOT_FULL:return FEISHU_STORE_ERR_FULL;
    case FEISHU_ROOT_CORRUPT:return FEISHU_STORE_ERR_CORRUPT;
    case FEISHU_ROOT_INVALID:return ESP_ERR_INVALID_SIZE;
    default:return ESP_FAIL;
    }
}
static esp_err_t read_part(unsigned key,size_t offset,void *data,size_t size,size_t *total)
{ return translate(feishu_root_read(&root,key,offset,data,size,total)); }
static esp_err_t save_parts(unsigned key,const feishu_root_part_t *parts,size_t count)
{
    for(unsigned tries=0;tries<1024;++tries) {
        int rc=feishu_root_write(&root,key,parts,count);
        if(rc!=FEISHU_ROOT_FULL)return translate(rc);
        if(feishu_cache_release_space()!=ESP_OK)return FEISHU_STORE_ERR_FULL;
    }
    return FEISHU_STORE_ERR_FULL;
}
static esp_err_t save_value(unsigned key,const void *data,size_t size)
{ feishu_root_part_t part={data,size};return save_parts(key,&part,1); }
static uint64_t fresh_epoch(void)
{ uint64_t value;esp_fill_random(&value,sizeof(value));return value ? value:1; }
static esp_err_t load_header(credentials_header_t *h,size_t *total)
{
    esp_err_t err=read_part(ROOT_CREDENTIALS,0,h,sizeof(*h),total);
    if(err!=ESP_OK)return err;
    if(h->version!=1 || h->reserved || !h->epoch || *total<=sizeof(*h) ||
       *total>sizeof(*h)+FEISHU_TOKEN_MAX || !memchr(h->app_id,0,sizeof(h->app_id)) ||
       !memchr(h->app_secret,0,sizeof(h->app_secret)))return FEISHU_STORE_ERR_CORRUPT;
    return ESP_OK;
}
static esp_err_t save_binding(const credentials_header_t *h,const char *refresh)
{
    feishu_root_part_t parts[]={{h,sizeof(*h)},{refresh,strlen(refresh)+1}};
    return save_parts(ROOT_CREDENTIALS,parts,2);
}
static esp_err_t save_scoped(unsigned key,uint64_t epoch,const char *value)
{
    feishu_root_part_t parts[]={{&epoch,sizeof(epoch)},{value,strlen(value)+1}};
    return save_parts(key,parts,2);
}
static bool absent(esp_err_t err)
{ return err==ESP_ERR_NVS_NOT_FOUND || err==ESP_ERR_NVS_PART_NOT_FOUND || err==ESP_ERR_INVALID_VERSION; }

/* Commit and read back named imports before removing legacy application keys.
 * The marker prevents stale account tokens from being imported after unbinding. */
static esp_err_t migrate_locked(void)
{
    if(migrated)return ESP_OK;
    uint32_t marker=0;size_t total=0;
    esp_err_t err=read_part(ROOT_MIGRATION,0,&marker,sizeof(marker),&total);
    if(err==ESP_OK) {
        if(total!=sizeof(marker) || marker!=1)return FEISHU_STORE_ERR_CORRUPT;
        migrated=true;return ESP_OK;
    }
    if(!absent(err))return err;
#ifdef FEISHU_STORE_FRESH_POOL
    /* A raw-pool reset starts a new installation. Never resurrect legacy NVS
     * credentials after the user explicitly clears the application pool. */
    credentials_header_t fresh={0};
    err=load_header(&fresh,&total);
    if(absent(err)) {
        fresh.version=1;fresh.epoch=fresh_epoch();err=save_binding(&fresh,"");
    }
    if(err!=ESP_OK)return err;
    err=read_part(ROOT_WIFI_SETUP,0,NULL,0,&total);
    if(absent(err)){uint8_t setup=1;err=save_value(ROOT_WIFI_SETUP,&setup,1);}
    if(err!=ESP_OK)return err;
    marker=1;err=save_value(ROOT_MIGRATION,&marker,sizeof(marker));
    if(err==ESP_OK){migrated=true;(void)feishu_legacy_cleanup();}
    return err;
#endif
    credentials_header_t h={0};
    err=load_header(&h,&total);
    if(absent(err)) {
        feishu_credentials_t *old=calloc(1,sizeof(*old));if(!old)return ESP_ERR_NO_MEM;
        err=feishu_legacy_load_app_credentials(old);
        if(err==ESP_OK) {
            esp_err_t next=feishu_legacy_load_credentials(old);
            if(next!=ESP_OK && !absent(next))err=next;
        } else if(absent(err)) {memset(old,0,sizeof(*old));err=ESP_OK;}
        if(err==ESP_OK) {
            h.version=1;h.epoch=fresh_epoch();
            memcpy(h.app_id,old->app_id,sizeof(h.app_id));memcpy(h.app_secret,old->app_secret,sizeof(h.app_secret));
            err=save_binding(&h,old->refresh_token);
        }
        memset(old,0,sizeof(*old));free(old);
    }
    if(err!=ESP_OK)return err;
    err=read_part(ROOT_ACCESS,0,NULL,0,&total);
    if(absent(err)) {
        char *access=calloc(1,FEISHU_TOKEN_MAX);if(!access)return ESP_ERR_NO_MEM;
        esp_err_t imported=h.app_id[0] ? feishu_legacy_load_access_token(access,FEISHU_TOKEN_MAX):ESP_ERR_NVS_NOT_FOUND;
        if(imported!=ESP_OK)access[0]=0;
        err=save_scoped(ROOT_ACCESS,h.epoch,access);memset(access,0,FEISHU_TOKEN_MAX);free(access);
    }
    if(err!=ESP_OK)return err;
    err=read_part(ROOT_OWNER,0,NULL,0,&total);
    if(absent(err)) {
        char owner[FEISHU_CHAT_ID_MAX]={0};
        if(h.app_id[0] && feishu_legacy_load_cache_owner(owner,sizeof(owner))!=ESP_OK)owner[0]=0;
        err=save_scoped(ROOT_OWNER,h.epoch,owner);
    }
    if(err!=ESP_OK)return err;
    err=read_part(ROOT_SEEN,0,NULL,0,&total);
    if(absent(err)) {
        feishu_seen_t seen[FEISHU_MAX_CHATS]={0};size_t count=0;
        if(feishu_legacy_load_seen(seen,FEISHU_MAX_CHATS,&count)!=ESP_OK)count=0;
        err=save_value(ROOT_SEEN,seen,count*sizeof(*seen));
    }
    if(err!=ESP_OK)return err;
    err=read_part(ROOT_WIFI_SETUP,0,NULL,0,&total);
    if(absent(err)) {uint8_t value=feishu_legacy_wifi_setup_required();err=save_value(ROOT_WIFI_SETUP,&value,1);}
    if(err!=ESP_OK)return err;
    marker=1;err=save_value(ROOT_MIGRATION,&marker,sizeof(marker));
    if(err==ESP_OK) {migrated=true;(void)feishu_legacy_cleanup();}
    return err;
}
static esp_err_t begin(void)
{
    esp_err_t err=feishu_cache_pool_lock();if(err!=ESP_OK)return err;
    err=migrate_locked();if(err!=ESP_OK)feishu_cache_pool_unlock();return err;
}
static esp_err_t end(esp_err_t err) {feishu_cache_pool_unlock();return err;}

/* Caller holds the pool lock throughout the complete credentials snapshot. */
static esp_err_t load_credentials(feishu_credentials_t *c,bool with_refresh)
{
    if(!c)return ESP_ERR_INVALID_ARG;
    memset(c,0,sizeof(*c));esp_err_t err=begin();if(err!=ESP_OK)return err;
    credentials_header_t h;size_t total;err=load_header(&h,&total);
    if(err==ESP_OK && (!h.app_id[0] || strncmp(h.app_id,"cli_",4) || !h.app_secret[0]))err=ESP_ERR_NVS_NOT_FOUND;
    if(err==ESP_OK) {
        memcpy(c->app_id,h.app_id,sizeof(h.app_id));memcpy(c->app_secret,h.app_secret,sizeof(h.app_secret));
        if(with_refresh) {
            size_t size=total-sizeof(h);err=read_part(ROOT_CREDENTIALS,sizeof(h),c->refresh_token,size,&total);
            if(err==ESP_OK && (!c->refresh_token[0] || memchr(c->refresh_token,0,size)!=c->refresh_token+size-1))err=ESP_ERR_NVS_NOT_FOUND;
        }
    }
    memset(&h,0,sizeof(h));return end(err);
}
esp_err_t feishu_store_load_app_credentials(feishu_credentials_t *c) {return load_credentials(c,false);}
esp_err_t feishu_store_load_credentials(feishu_credentials_t *c) {return load_credentials(c,true);}
static bool valid_app(const feishu_credentials_t *c)
{
    return c && memchr(c->app_id,0,sizeof(c->app_id)) && !strncmp(c->app_id,"cli_",4) &&
        memchr(c->app_secret,0,sizeof(c->app_secret)) && c->app_secret[0];
}
static esp_err_t replace_binding(const feishu_credentials_t *c,bool with_refresh)
{
    if(!valid_app(c) || (with_refresh && (!memchr(c->refresh_token,0,sizeof(c->refresh_token)) || !c->refresh_token[0])))return ESP_ERR_INVALID_ARG;
    esp_err_t err=begin();if(err!=ESP_OK)return err;
    credentials_header_t h={.version=1,.epoch=fresh_epoch()};
    memcpy(h.app_id,c->app_id,sizeof(h.app_id));memcpy(h.app_secret,c->app_secret,sizeof(h.app_secret));
    err=save_binding(&h,with_refresh ? c->refresh_token:"");memset(&h,0,sizeof(h));return end(err);
}
esp_err_t feishu_store_save_app_credentials(const feishu_credentials_t *c) {return replace_binding(c,false);}
esp_err_t feishu_store_save_credentials(const feishu_credentials_t *c) {return replace_binding(c,true);}
esp_err_t feishu_store_save_refresh_token(const char *token)
{
    if(!token || !*token || strlen(token)>=FEISHU_TOKEN_MAX)return ESP_ERR_INVALID_ARG;
    esp_err_t err=begin();if(err!=ESP_OK)return err;
    credentials_header_t h;size_t total;err=load_header(&h,&total);
    if(err==ESP_OK && !h.app_id[0])err=ESP_ERR_INVALID_STATE;
    if(err==ESP_OK)err=save_binding(&h,token);
    memset(&h,0,sizeof(h));return end(err);
}
static esp_err_t scoped_read(unsigned key,char *value,size_t capacity)
{
    if(!value || !capacity)return ESP_ERR_INVALID_ARG;
    value[0]=0;esp_err_t err=begin();if(err!=ESP_OK)return err;
    credentials_header_t h;size_t total=0;uint64_t epoch=0;err=load_header(&h,&total);
    if(err==ESP_OK)err=read_part(key,0,&epoch,sizeof(epoch),&total);
    if(err==ESP_OK && (epoch!=h.epoch || total<=sizeof(epoch) || total-sizeof(epoch)>capacity))err=ESP_ERR_NVS_NOT_FOUND;
    if(err==ESP_OK) {
        size_t size=total-sizeof(epoch);err=read_part(key,sizeof(epoch),value,size,&total);
        if(err==ESP_OK && (!value[0] || memchr(value,0,size)!=value+size-1))err=ESP_ERR_NVS_NOT_FOUND;
    }
    if(err!=ESP_OK)value[0]=0;
    memset(&h,0,sizeof(h));return end(err);
}
static esp_err_t scoped_save(unsigned key,const char *value,size_t limit)
{
    if(!value || strlen(value)>=limit)return ESP_ERR_INVALID_ARG;
    esp_err_t err=begin();if(err!=ESP_OK)return err;
    credentials_header_t h;size_t total;err=load_header(&h,&total);
    if(err==ESP_OK)err=save_scoped(key,h.epoch,value);
    memset(&h,0,sizeof(h));return end(err);
}
esp_err_t feishu_store_load_access_token(char *t,size_t n) {return scoped_read(ROOT_ACCESS,t,n);}
esp_err_t feishu_store_save_access_token(const char *t) {return scoped_save(ROOT_ACCESS,t,FEISHU_TOKEN_MAX);}
esp_err_t feishu_store_clear_access_token(void) {return scoped_save(ROOT_ACCESS,"",FEISHU_TOKEN_MAX);}
static bool valid_voice_cookie(const char *cookie)
{
    if(!cookie || strncmp(cookie,"session=",8) || !cookie[8] || strlen(cookie)>=FEISHU_VOICE_COOKIE_MAX)return false;
    for(const unsigned char *p=(const unsigned char *)cookie;*p;++p)if(*p<32 || *p>126)return false;
    return true;
}
esp_err_t feishu_store_save_voice_cookie(const char *cookie)
{ return valid_voice_cookie(cookie) ? scoped_save(ROOT_VOICE,cookie,FEISHU_VOICE_COOKIE_MAX):ESP_ERR_INVALID_ARG; }
esp_err_t feishu_store_load_voice_cookie(char *cookie,size_t capacity)
{
    esp_err_t err=scoped_read(ROOT_VOICE,cookie,capacity);
    if(err==ESP_OK && !valid_voice_cookie(cookie)){cookie[0]=0;err=FEISHU_STORE_ERR_CORRUPT;}
    return err;
}
esp_err_t feishu_store_load_cache_owner(char *o,size_t n) {return scoped_read(ROOT_OWNER,o,n);}
esp_err_t feishu_store_save_cache_owner(const char *o) {return scoped_save(ROOT_OWNER,o,FEISHU_CHAT_ID_MAX);}
esp_err_t feishu_store_clear_user_tokens(void)
{
    esp_err_t err=begin();if(err!=ESP_OK)return err;
    credentials_header_t h;size_t total;err=load_header(&h,&total);
    if(err==ESP_OK) {h.epoch=fresh_epoch();err=save_binding(&h,"");}
    memset(&h,0,sizeof(h));return end(err);
}
esp_err_t feishu_store_clear_credentials(void)
{
    esp_err_t err=begin();if(err!=ESP_OK)return err;
    credentials_header_t h={.version=1,.epoch=fresh_epoch()};return end(save_binding(&h,""));
}
bool feishu_store_wifi_setup_required(void)
{
    esp_err_t err=begin();if(err!=ESP_OK)return false;
    uint8_t value=0;size_t total=0;err=read_part(ROOT_WIFI_SETUP,0,&value,1,&total);
    end(err);return err==ESP_OK && total==1 && value!=0;
}
esp_err_t feishu_store_require_wifi_setup(bool required)
{
    esp_err_t err=begin();if(err!=ESP_OK)return err;
    uint8_t value=required ? 1:0;return end(save_value(ROOT_WIFI_SETUP,&value,1));
}
esp_err_t feishu_store_load_seen(feishu_seen_t *seen,size_t capacity,size_t *count)
{
    if(!seen || !capacity || !count || capacity>FEISHU_MAX_CHATS)return ESP_ERR_INVALID_ARG;
    *count=0;esp_err_t err=begin();if(err!=ESP_OK)return err;
    size_t total=0;err=read_part(ROOT_SEEN,0,NULL,0,&total);
    if(err==ESP_OK && (total%sizeof(*seen) || total/sizeof(*seen)>capacity))err=ESP_ERR_INVALID_SIZE;
    if(err==ESP_OK) {err=read_part(ROOT_SEEN,0,seen,total,&total);if(err==ESP_OK)*count=total/sizeof(*seen);}
    return end(err);
}
esp_err_t feishu_store_mark_seen(const char *chat_id,uint64_t time_ms)
{
    if(!chat_id || !*chat_id || strlen(chat_id)>=FEISHU_CHAT_ID_MAX)return ESP_ERR_INVALID_ARG;
    esp_err_t err=begin();if(err!=ESP_OK)return err;
    feishu_seen_t seen[FEISHU_MAX_CHATS]={0};size_t count=0;
    err=feishu_store_load_seen(seen,FEISHU_MAX_CHATS,&count);
    if(err==ESP_OK) {
        size_t index=0;while(index<count && strcmp(seen[index].chat_id,chat_id))++index;
        if(index==count) {
            if(count<FEISHU_MAX_CHATS)++count;
            else {index=0;for(size_t i=1;i<count;++i)if(seen[i].last_seen_time_ms<seen[index].last_seen_time_ms)index=i;}
            feishu_utf8_copy(seen[index].chat_id,sizeof(seen[index].chat_id),chat_id);
        }
        if(time_ms>seen[index].last_seen_time_ms)seen[index].last_seen_time_ms=time_ms;
        err=save_value(ROOT_SEEN,seen,count*sizeof(*seen));
    }
    return end(err);
}
esp_err_t feishu_store_load_wifi(void *config,size_t size)
{
    if(!config || !size || size>2048)return ESP_ERR_INVALID_ARG;
    memset(config,0,size);esp_err_t err=begin();if(err!=ESP_OK)return err;
    size_t total;err=read_part(ROOT_WIFI,0,config,size,&total);
    if(err==ESP_OK && total!=size)err=ESP_ERR_INVALID_VERSION;
    if(err!=ESP_OK)memset(config,0,size);
    return end(err);
}
esp_err_t feishu_store_save_wifi(const void *config,size_t size)
{
    if(!config || !size || size>2048)return ESP_ERR_INVALID_ARG;
    esp_err_t err=begin();if(err!=ESP_OK)return err;
    return end(save_value(ROOT_WIFI,config,size));
}
esp_err_t feishu_store_get_auth_request(unsigned *request)
{
    if(!request)return ESP_ERR_INVALID_ARG;
    *request=FEISHU_AUTH_NONE;esp_err_t err=begin();if(err!=ESP_OK)return err;
    uint8_t value=0;size_t total=0;err=read_part(ROOT_AUTH_REQUEST,0,&value,1,&total);
    if(err==ESP_ERR_NVS_NOT_FOUND)err=ESP_OK;
    else if(err==ESP_OK && (total!=1 || value>FEISHU_AUTH_REPAIR_APP))err=FEISHU_STORE_ERR_CORRUPT;
    if(err==ESP_OK)*request=value;
    return end(err);
}
esp_err_t feishu_store_request_auth(unsigned request)
{
    if(request>FEISHU_AUTH_REPAIR_APP)return ESP_ERR_INVALID_ARG;
    esp_err_t err=begin();if(err!=ESP_OK)return err;
    uint8_t value=(uint8_t)request;return end(save_value(ROOT_AUTH_REQUEST,&value,1));
}


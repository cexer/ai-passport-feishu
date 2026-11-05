#include "feishu_store_legacy.h"

#include "cJSON.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <stdlib.h>
#include <string.h>

esp_err_t feishu_legacy_cleanup(void)
{
    const char *keys[]={"app_id","app_secret","refresh","owner_v","seen","cache_owner","wifi_setup"};
    nvs_handle_t h;
    esp_err_t err=nvs_open("feishu",NVS_READWRITE,&h);
    if(err==ESP_OK) {
        for(size_t i=0;i<sizeof(keys)/sizeof(keys[0]);++i) {
            esp_err_t next=nvs_erase_key(h,keys[i]);
            if(next!=ESP_OK && next!=ESP_ERR_NVS_NOT_FOUND) {err=next;break;}
        }
        if(err==ESP_OK)err=nvs_commit(h);
        nvs_close(h);
    } else if(err==ESP_ERR_NVS_NOT_FOUND)err=ESP_OK;
    if(err!=ESP_OK)return err;
    esp_err_t token_err=nvs_flash_init_partition("feishu_cache");
    if(token_err==ESP_OK) {
        token_err=nvs_open_from_partition("feishu_cache","tokens",NVS_READWRITE,&h);
        if(token_err==ESP_OK) {
            token_err=nvs_erase_key(h,"access");
            if(token_err==ESP_ERR_NVS_NOT_FOUND)token_err=ESP_OK;
            if(token_err==ESP_OK)token_err=nvs_commit(h);
            nvs_close(h);
        } else if(token_err==ESP_ERR_NVS_NOT_FOUND)token_err=ESP_OK;
    }
    /* The legacy access cache is optional; never erase its whole partition. */
    return token_err;
}

#define FEISHU_NVS_NAMESPACE "feishu"
#define FEISHU_NVS_APP_ID "app_id"
#define FEISHU_NVS_APP_SECRET "app_secret"
#define FEISHU_NVS_REFRESH "refresh"
#define FEISHU_NVS_OWNER_VERSION "owner_v"
#define FEISHU_OWNER_VERSION 1
#define FEISHU_NVS_SEEN "seen"
#define FEISHU_NVS_CACHE_OWNER "cache_owner"
#define FEISHU_CACHE_PARTITION "feishu_cache"
#define FEISHU_CACHE_NAMESPACE "tokens"
#define FEISHU_CACHE_ACCESS "access"



static esp_err_t cache_prepare(void)
{
    return nvs_flash_init_partition(FEISHU_CACHE_PARTITION);
}


bool feishu_legacy_wifi_setup_required(void)
{
    nvs_handle_t h;
    uint8_t value=0;
    if(nvs_open(FEISHU_NVS_NAMESPACE,NVS_READONLY,&h)!=ESP_OK) return false;
    nvs_get_u8(h,"wifi_setup",&value);
    nvs_close(h);
    return value!=0;
}

esp_err_t feishu_legacy_load_cache_owner(char *owner,size_t capacity)
{
    if(!owner || !capacity) return ESP_ERR_INVALID_ARG;
    owner[0]='\0';
    nvs_handle_t handle;
    esp_err_t err=nvs_open(FEISHU_NVS_NAMESPACE,NVS_READONLY,&handle);
    if(err!=ESP_OK) return err;
    size_t size=capacity;
    err=nvs_get_str(handle,FEISHU_NVS_CACHE_OWNER,owner,&size);
    nvs_close(handle);
    return err;
}

static esp_err_t require_owner_provisioning(nvs_handle_t handle)
{
    uint8_t version = 0;
    esp_err_t err = nvs_get_u8(handle, FEISHU_NVS_OWNER_VERSION, &version);

    if (err != ESP_OK) return err;
    return version == FEISHU_OWNER_VERSION ? ESP_OK : ESP_ERR_INVALID_VERSION;
}

static esp_err_t load_string(nvs_handle_t handle, const char *key,
                             char *value, size_t capacity)
{
    size_t required = capacity;
    esp_err_t err = nvs_get_str(handle, key, value, &required);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        value[0] = '\0';
        return err;
    }
    if (err == ESP_ERR_NVS_INVALID_LENGTH) return ESP_ERR_INVALID_SIZE;
    return err;
}

static esp_err_t load_token_blob(nvs_handle_t handle, char *value,
                                 size_t capacity)
{
    size_t required = capacity;
    esp_err_t err = nvs_get_blob(handle, FEISHU_NVS_REFRESH, value, &required);

    if (err == ESP_ERR_NVS_TYPE_MISMATCH) {
        // Backward compatibility for short tokens stored by older firmware.
        return load_string(handle, FEISHU_NVS_REFRESH, value, capacity);
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) value[0] = '\0';
    if (err == ESP_OK && (required == 0 || required > capacity ||
                          value[required - 1] != '\0')) {
        value[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }
    return err;
}

esp_err_t feishu_legacy_load_credentials(feishu_credentials_t *credentials)
{
    nvs_handle_t handle;
    esp_err_t err;

    if (credentials == NULL) return ESP_ERR_INVALID_ARG;
    memset(credentials, 0, sizeof(*credentials));
    err = nvs_open(FEISHU_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    // Credentials from firmware predating owner provisioning have no marker.
    // Refuse them so an OTA upgrade cannot silently retain a developer's app.
    err = require_owner_provisioning(handle);
    if (err == ESP_OK) {
        err = load_string(handle, FEISHU_NVS_APP_ID, credentials->app_id,
                          sizeof(credentials->app_id));
    }
    if (err == ESP_OK) {
        err = load_string(handle, FEISHU_NVS_APP_SECRET,
                          credentials->app_secret,
                          sizeof(credentials->app_secret));
    }
    if (err == ESP_OK) {
        err = load_token_blob(handle, credentials->refresh_token,
                              sizeof(credentials->refresh_token));
    }
    nvs_close(handle);
    return err;
}

esp_err_t feishu_legacy_load_app_credentials(feishu_credentials_t *credentials)
{
    nvs_handle_t handle;
    esp_err_t err;

    if (credentials == NULL) return ESP_ERR_INVALID_ARG;
    memset(credentials, 0, sizeof(*credentials));
    err = nvs_open(FEISHU_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    err = require_owner_provisioning(handle);
    if (err == ESP_OK) {
        err = load_string(handle, FEISHU_NVS_APP_ID, credentials->app_id,
                          sizeof(credentials->app_id));
    }
    if (err == ESP_OK) {
        err = load_string(handle, FEISHU_NVS_APP_SECRET,
                          credentials->app_secret,
                          sizeof(credentials->app_secret));
    }
    nvs_close(handle);
    if (err != ESP_OK || strncmp(credentials->app_id, "cli_", 4) != 0 ||
        credentials->app_secret[0] == '\0') {
        memset(credentials, 0, sizeof(*credentials));
        return err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err;
    }
    return ESP_OK;
}

esp_err_t feishu_legacy_load_access_token(char *access_token, size_t capacity)
{
    nvs_handle_t handle;
    size_t required = capacity;
    esp_err_t err;

    if (access_token == NULL || capacity < 2) return ESP_ERR_INVALID_ARG;
    access_token[0] = '\0';
    err = cache_prepare();
    if (err != ESP_OK) return err;
    err = nvs_open_from_partition(FEISHU_CACHE_PARTITION,
                                  FEISHU_CACHE_NAMESPACE, NVS_READONLY,
                                  &handle);
    if (err != ESP_OK) return err;
    err = nvs_get_blob(handle, FEISHU_CACHE_ACCESS, access_token, &required);
    nvs_close(handle);
    if (err == ESP_OK && (required == 0 || required > capacity ||
                          access_token[required - 1] != '\0')) {
        access_token[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }
    return err;
}

esp_err_t feishu_legacy_load_seen(feishu_seen_t *seen, size_t capacity,
                                 size_t *count)
{
    nvs_handle_t handle;
    size_t bytes;
    esp_err_t err;

    if (seen == NULL || count == NULL || capacity == 0) return ESP_ERR_INVALID_ARG;
    *count = 0;
    bytes = capacity * sizeof(*seen);
    err = nvs_open(FEISHU_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    err = nvs_get_blob(handle, FEISHU_NVS_SEEN, seen, &bytes);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK || bytes % sizeof(*seen) != 0) {
        return err == ESP_OK ? ESP_ERR_INVALID_SIZE : err;
    }
    *count = bytes / sizeof(*seen);
    return ESP_OK;
}

#pragma once

#include "esp_err.h"
#include "feishu_model.h"

#include <stddef.h>

#define FEISHU_APP_ID_MAX 64
#define FEISHU_APP_SECRET_MAX 128
// Feishu user tokens are large opaque values. Real lark-cli credentials are
// currently about 7 KiB, so the former 2 KiB buffer was not sufficient.
#define FEISHU_TOKEN_MAX 8192
#define FEISHU_STORE_ERR_FULL ((esp_err_t)0x7f10)
#define FEISHU_STORE_ERR_CORRUPT ((esp_err_t)0x7f11)
#define FEISHU_VOICE_COOKIE_MAX 4097
/* Separate web session, scoped to the current app binding; never replaces OAuth. */
esp_err_t feishu_store_load_voice_cookie(char *cookie,size_t capacity);
esp_err_t feishu_store_save_voice_cookie(const char *cookie);

typedef struct {
    char app_id[FEISHU_APP_ID_MAX];
    char app_secret[FEISHU_APP_SECRET_MAX];
    char refresh_token[FEISHU_TOKEN_MAX];
} feishu_credentials_t;

esp_err_t feishu_store_load_credentials(feishu_credentials_t *credentials);
// App credentials are provisioned by the device owner. The refresh token may
// be empty until that app completes device authorization on this device.
esp_err_t feishu_store_load_app_credentials(feishu_credentials_t *credentials);
esp_err_t feishu_store_save_app_credentials(
    const feishu_credentials_t *credentials);
esp_err_t feishu_store_save_app_credentials_json(const uint8_t *json,
                                                 size_t length);
esp_err_t feishu_store_parse_app_credentials_json(const uint8_t *json,size_t length,
                                                   feishu_credentials_t *credentials);
esp_err_t feishu_store_save_credentials(const feishu_credentials_t *credentials);
esp_err_t feishu_store_save_credentials_json(const uint8_t *json, size_t length);
esp_err_t feishu_store_save_refresh_token(const char *refresh_token);
esp_err_t feishu_store_load_access_token(char *access_token, size_t capacity);
esp_err_t feishu_store_save_access_token(const char *access_token);
esp_err_t feishu_store_clear_access_token(void);
// Reauthorize the same owner app without asking for its secret again.
esp_err_t feishu_store_clear_user_tokens(void);
esp_err_t feishu_store_clear_credentials(void);

esp_err_t feishu_store_load_seen(feishu_seen_t *seen, size_t capacity,
                                 size_t *count);
esp_err_t feishu_store_mark_seen(const char *chat_id, uint64_t time_ms);
esp_err_t feishu_store_load_cache_owner(char *owner, size_t capacity);
esp_err_t feishu_store_save_cache_owner(const char *owner);
bool feishu_store_wifi_setup_required(void);
esp_err_t feishu_store_require_wifi_setup(bool required);
esp_err_t feishu_store_load_wifi(void *config,size_t size);
esp_err_t feishu_store_save_wifi(const void *config,size_t size);
enum { FEISHU_AUTH_NONE,FEISHU_AUTH_REAUTHORIZE,FEISHU_AUTH_CHANGE_APP,FEISHU_AUTH_REPAIR_APP };
esp_err_t feishu_store_get_auth_request(unsigned *request);
esp_err_t feishu_store_request_auth(unsigned request);

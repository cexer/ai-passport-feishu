#pragma once
#include "feishu_store.h"
/* Read-only import, followed by deletion of named application keys only. */
esp_err_t feishu_legacy_load_credentials(feishu_credentials_t *credentials);
esp_err_t feishu_legacy_load_app_credentials(feishu_credentials_t *credentials);
esp_err_t feishu_legacy_load_access_token(char *token,size_t capacity);
esp_err_t feishu_legacy_load_cache_owner(char *owner,size_t capacity);
esp_err_t feishu_legacy_load_seen(feishu_seen_t *seen,size_t capacity,size_t *count);
bool feishu_legacy_wifi_setup_required(void);
esp_err_t feishu_legacy_cleanup(void);

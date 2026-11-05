#include "feishu_store.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>

static bool json_string(cJSON *root, const char *name, char *output,
                        size_t output_size)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);

    if (!cJSON_IsString(item) || item->valuestring == NULL ||
        item->valuestring[0] == '\0' || strlen(item->valuestring) >= output_size) {
        return false;
    }
    memcpy(output, item->valuestring, strlen(item->valuestring) + 1);
    return true;
}

esp_err_t feishu_store_save_app_credentials_json(const uint8_t *json,
                                                 size_t length)
{
    feishu_credentials_t *credentials;
    cJSON *root;
    bool valid;

    if (json == NULL || length == 0 || length > 512) {
        return ESP_ERR_INVALID_ARG;
    }
    credentials = calloc(1, sizeof(*credentials));
    if (credentials == NULL) return ESP_ERR_NO_MEM;
    root = cJSON_ParseWithLength((const char *)json, length);
    if (root == NULL) {
        free(credentials);
        return ESP_ERR_INVALID_ARG;
    }
    valid = json_string(root, "app_id", credentials->app_id,
                        sizeof(credentials->app_id)) &&
            json_string(root, "app_secret", credentials->app_secret,
                        sizeof(credentials->app_secret));
    cJSON_Delete(root);
    esp_err_t err = valid ? feishu_store_save_app_credentials(credentials) :
                            ESP_ERR_INVALID_ARG;
    memset(credentials, 0, sizeof(*credentials));
    free(credentials);
    return err;
}

esp_err_t feishu_store_parse_app_credentials_json(const uint8_t *json,size_t length,
                                                   feishu_credentials_t *credentials)
{
    if(!json || !length || length>512 || !credentials)return ESP_ERR_INVALID_ARG;
    cJSON *root=cJSON_ParseWithLength((const char *)json,length);
    if(!root)return ESP_ERR_INVALID_ARG;
    memset(credentials,0,sizeof(*credentials));
    bool valid=json_string(root,"app_id",credentials->app_id,sizeof(credentials->app_id)) &&
        json_string(root,"app_secret",credentials->app_secret,sizeof(credentials->app_secret)) &&
        !strncmp(credentials->app_id,"cli_",4) && credentials->app_id[4];
    cJSON_Delete(root);
    if(!valid)memset(credentials,0,sizeof(*credentials));
    return valid ? ESP_OK:ESP_ERR_INVALID_ARG;
}

esp_err_t feishu_store_save_credentials_json(const uint8_t *json, size_t length)
{
    feishu_credentials_t *credentials;
    cJSON *root;
    bool valid;

    if (json == NULL || length == 0 || length > FEISHU_TOKEN_MAX + 1024) {
        return ESP_ERR_INVALID_ARG;
    }
    credentials = calloc(1, sizeof(*credentials));
    if (credentials == NULL) return ESP_ERR_NO_MEM;
    root = cJSON_ParseWithLength((const char *)json, length);
    if (root == NULL) {
        free(credentials);
        return ESP_ERR_INVALID_ARG;
    }
    valid = json_string(root, "app_id", credentials->app_id,
                        sizeof(credentials->app_id)) &&
            json_string(root, "app_secret", credentials->app_secret,
                        sizeof(credentials->app_secret)) &&
            json_string(root, "refresh_token", credentials->refresh_token,
                        sizeof(credentials->refresh_token));
    cJSON_Delete(root);
    if (!valid || strncmp(credentials->app_id, "cli_", 4) != 0) {
        memset(credentials, 0, sizeof(*credentials));
        free(credentials);
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = feishu_store_save_credentials(credentials);
    memset(credentials, 0, sizeof(*credentials));
    free(credentials);
    return err;
}


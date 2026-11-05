#include "feishu_provision.h"

#include "feishu_store.h"
#include "cJSON.h"
#include "esp_wifi.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>

#define PROVISION_PREFIX "FAP-FEISHU/1 "
#define PROVISION_READY "FAP-FEISHU/1 READY"
#define PROVISION_OK "FAP-FEISHU/1 OK"
#define PROVISION_ERROR "FAP-FEISHU/1 ERROR"
#define PROVISION_LINE_MAX 768
#define PROVISION_JSON_MAX 512

static const char *TAG = "feishu_provision";
static TaskHandle_t s_task;
static volatile bool s_received;
static volatile bool s_stop;
static bool s_wifi;
static feishu_credentials_t *s_candidate;
static atomic_bool s_pending;
static uint8_t *s_pending_json;
static size_t s_pending_length;
static esp_err_t s_commit_result;

static esp_err_t save_wifi(const uint8_t *json, size_t length)
{
    cJSON *root = cJSON_ParseWithLength((const char *)json, length);
    cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    cJSON *password = cJSON_GetObjectItemCaseSensitive(root, "password");
    wifi_config_t config = {0};
    esp_err_t result = ESP_ERR_INVALID_ARG;
    if (cJSON_IsString(ssid) && ssid->valuestring &&
        strlen(ssid->valuestring) > 0 && strlen(ssid->valuestring) <= 32 &&
        cJSON_IsString(password) && password->valuestring &&
        strlen(password->valuestring) <= 64) {
        memcpy(config.sta.ssid, ssid->valuestring, strlen(ssid->valuestring));
        memcpy(config.sta.password, password->valuestring, strlen(password->valuestring));
        config.sta.threshold.authmode = WIFI_AUTH_OPEN;
        result = feishu_store_save_wifi(&config, sizeof(config));
    }
    memset(&config, 0, sizeof(config));
    cJSON_Delete(root);
    return result;
}

void feishu_provision_commit_pending(void)
{
    if (!atomic_load_explicit(&s_pending, memory_order_acquire)) return;
    s_commit_result = s_wifi ? save_wifi(s_pending_json, s_pending_length) : s_candidate ?
        feishu_store_parse_app_credentials_json(s_pending_json,s_pending_length,s_candidate) :
        feishu_store_save_app_credentials_json(s_pending_json, s_pending_length);
    atomic_store_explicit(&s_pending, false, memory_order_release);
}

static bool process_line(char *line, uint8_t *json)
{
    const char *prefix = s_wifi ? "FAP-WIFI/1 " : PROVISION_PREFIX;
    if (strcmp(line, "FAP-SETUP/1 PING") == 0) {
        puts(s_wifi ? "FAP-WIFI/1 READY" : PROVISION_READY);
        return false;
    }
    if (strncmp(line, prefix, strlen(prefix)) != 0) {
        puts(s_wifi ? "FAP-WIFI/1 ERROR" : PROVISION_ERROR);
        return false;
    }
    const unsigned char *encoded =
        (const unsigned char *)(line + strlen(prefix));
    size_t encoded_length = strlen((const char *)encoded);
    size_t json_length = 0;
    esp_err_t result = ESP_ERR_INVALID_ARG;
    if (encoded_length > 0 &&
        mbedtls_base64_decode(json, PROVISION_JSON_MAX, &json_length,
                              encoded, encoded_length) == 0 &&
        json_length > 0 && json_length <= PROVISION_JSON_MAX) {
        json[json_length] = '\0';
        // Configuration writes need the existing onboarding worker's stack.
        // Keep ownership of this bounded buffer until that worker completes.
        s_pending_json = json;
        s_pending_length = json_length;
        s_commit_result = ESP_ERR_INVALID_STATE;
        atomic_store_explicit(&s_pending, true, memory_order_release);
        while (atomic_load_explicit(&s_pending, memory_order_acquire) && !s_stop) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (!s_stop) result = s_commit_result;
        atomic_store_explicit(&s_pending, false, memory_order_release);
        s_pending_json = NULL;
    }
    memset(json, 0, PROVISION_JSON_MAX + 1);
    memset(line, 0, PROVISION_LINE_MAX);
    puts(s_wifi ? (result == ESP_OK ? "FAP-WIFI/1 OK" : "FAP-WIFI/1 ERROR") :
                  (result == ESP_OK ? PROVISION_OK : PROVISION_ERROR));
    return result == ESP_OK;
}

static void provision_task(void *argument)
{
    (void)argument;
    bool accepted = false;
    int flags = -1;
    char *line = calloc(1, PROVISION_LINE_MAX);
    uint8_t *json = calloc(1, PROVISION_JSON_MAX + 1);
    if (line == NULL || json == NULL) {
        ESP_LOGE(TAG, "could not allocate USB provisioning buffers");
        goto finished;
    }

    setvbuf(stdout, NULL, _IONBF, 0);
    flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    // IDF 5.5.3's bare USB/JTAG VFS has no RX ring-buffer count. Setting
    // O_NONBLOCK skips its one-byte FIFO prefetch and consequently reads zero
    // bytes forever. Its default (no-driver) FIFO read already returns
    // immediately, so retain that bounded path without adding a USB driver.
    if (flags >= 0) fcntl(STDIN_FILENO, F_SETFL, flags & ~O_NONBLOCK);
    puts(s_wifi ? "FAP-WIFI/1 READY" : PROVISION_READY);
    size_t used = 0;
    bool overflow = false;
    while (!s_stop && !accepted) {
        char value;
        ssize_t count = read(STDIN_FILENO, &value, 1);
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (count <= 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (value == '\n' || value == '\r') {
            if (used > 0 && !overflow) {
                line[used] = '\0';
                accepted = process_line(line, json);
                used = 0;
            }
            used = 0;
            overflow = false;
        } else if (overflow) {
            continue;
        } else if (used + 1 < PROVISION_LINE_MAX) {
            line[used++] = value;
        } else {
            memset(line, 0, PROVISION_LINE_MAX);
            used = 0;
            overflow = true;
            puts(s_wifi ? "FAP-WIFI/1 ERROR" : PROVISION_ERROR);
        }
    }

finished:
    if (flags >= 0) fcntl(STDIN_FILENO, F_SETFL, flags);
    if (line != NULL) {
        memset(line, 0, PROVISION_LINE_MAX);
        free(line);
    }
    if (json != NULL) {
        memset(json, 0, PROVISION_JSON_MAX + 1);
        free(json);
    }
    s_received = accepted;
    s_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t start(bool wifi,feishu_credentials_t *candidate)
{
    if (s_task != NULL) return ESP_ERR_INVALID_STATE;
    s_received = false;
    s_stop = false;
    s_wifi = wifi;
    s_candidate=candidate;
    atomic_store(&s_pending, false);
    return xTaskCreate(provision_task, "feishu_provision", 4096, NULL, 4,
                       &s_task) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t feishu_provision_start(void) { return start(false,NULL); }
esp_err_t feishu_provision_start_wifi(void) { return start(true,NULL); }
esp_err_t feishu_provision_start_candidate(feishu_credentials_t *candidate)
{ return candidate ? start(false,candidate):ESP_ERR_INVALID_ARG; }

void feishu_provision_stop(void)
{
    if (s_task != NULL) {
        s_stop = true;
        for (unsigned attempt = 0; attempt < 20 && s_task != NULL; ++attempt) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (s_task != NULL) {
            ESP_LOGW(TAG, "USB provisioning task did not stop in time");
        }
    }
}

bool feishu_provision_credentials_received(void)
{
    return s_received;
}

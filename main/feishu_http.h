#pragma once

#include "esp_err.h"
#include "esp_http_client.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    char *body;
    size_t length;
    int status_code;
} feishu_http_response_t;

typedef esp_err_t (*feishu_http_sink_t)(void *context,const uint8_t *data,size_t size);
typedef bool (*feishu_http_cancel_t)(void *context);
#define FEISHU_HTTP_ERR_CANCELLED ((esp_err_t)0x7f04)
typedef bool (*feishu_http_write_t)(void *context,const void *data,size_t size);
typedef bool (*feishu_http_upload_t)(void *context,feishu_http_write_t write,void *write_context);
typedef struct {const char *name,*value;} feishu_http_header_t;
/* Worker-owned connection. Custom headers replace bearer/JSON defaults. */
esp_err_t feishu_http_post_headers_stream(const char *url,const feishu_http_header_t *headers,size_t header_count,
    size_t body_size,feishu_http_upload_t upload,void *upload_context,bool keepalive,
    size_t response_limit,feishu_http_sink_t sink,void *sink_context,
    feishu_http_cancel_t cancel,void *cancel_context,int *status_code,size_t *response_size);
typedef int (*feishu_http_read_t)(void *context,void *data,size_t capacity);
typedef esp_err_t (*feishu_http_consumer_t)(void *context,feishu_http_read_t read,void *read_context,int64_t content_length);
esp_err_t feishu_http_get_consume(const char *url,const char *bearer_token,size_t limit,
                                   feishu_http_consumer_t consume,void *context,
                                   feishu_http_cancel_t cancel,void *cancel_context,
                                   int *status_code,size_t *length);
/* Manual bounded upload/read; response callbacks run after TLS read returns.
 * keepalive is reserved for the single ASR worker-owned connection. */
esp_err_t feishu_http_post_stream(const char *url,const char *bearer_token,size_t body_size,
                                   feishu_http_upload_t upload,void *upload_context,bool keepalive,
                                   size_t response_limit,feishu_http_sink_t sink,void *sink_context,
                                   feishu_http_cancel_t cancel,void *cancel_context,
                                   int *status_code,size_t *response_size);
/* Consumer runs after socket/TLS read returns, keeping its stack separate from
 * storage/codec work. No response aggregation or allocation by content length. */
esp_err_t feishu_http_get_stream(const char *url,const char *bearer_token,size_t limit,
                                  feishu_http_sink_t sink,void *context,
                                  feishu_http_cancel_t cancel,void *cancel_context,
                                  int *status_code,size_t *length);

/* Opt-in API error extraction: parse at most 4 KiB of non-2xx JSON only
 * for its root numeric code. Never forwards error content to the success sink. */
esp_err_t feishu_http_get_stream_code(const char *url,const char *token,size_t limit,
    feishu_http_sink_t sink,void *context,feishu_http_cancel_t cancel,void *cancel_context,
    int *status_code,size_t *length,int *api_code);

esp_err_t feishu_http_request(esp_http_client_method_t method,
                              const char *url, const char *bearer_token,
                              const char *json_body, size_t response_limit,
                              feishu_http_response_t *response);
// Bounded registration form; redirects disabled so polling codes stay on-host.
esp_err_t feishu_http_form_request(const char *url,const char *body,size_t limit,
                                   feishu_http_response_t *response);
esp_err_t feishu_http_form_request_cancelable(const char *url,const char *body,size_t limit,
    feishu_http_response_t *response,feishu_http_cancel_t cancel,void *context);
// Receive only actual bytes in bounded chunks, then join after TLS is freed.
esp_err_t feishu_http_get_response(const char *url,const char *token,size_t limit,
    feishu_http_cancel_t cancel,void *context,feishu_http_response_t *response);
// Memory-bounded OAuth response reader. It extracts only the large token
// strings and the small error string while HTTPS data is streaming.
esp_err_t feishu_http_oauth_request(const char *url, const char *json_body,
                                    char *access_token, size_t access_capacity,
                                    char *refresh_token, size_t refresh_capacity,
                                    char *oauth_error, size_t error_capacity,
                                    int *status_code, size_t *response_length);
esp_err_t feishu_http_stream_request(const char *url, const char *bearer_token,
                                     const char *json_body,
                                     size_t response_limit,
                                     feishu_http_response_t *response);
esp_err_t feishu_http_download_file(const char *url, const char *bearer_token,
                                    const char *path, size_t limit,
                                    int *status_code, size_t *length);
void feishu_http_stream_close(void);
void feishu_http_response_free(feishu_http_response_t *response);

/* Optional cooperative heartbeat, scoped to the current network worker. */
typedef bool (*feishu_http_progress_t)(void *context);
void feishu_http_set_progress(feishu_http_progress_t progress,void *context);
/* Opt in only while a live microphone queue must preempt a read. Reset when
 * the owner installs/clears its progress hook. Never enables POST replay. */
void feishu_http_set_read_preemptible(bool enabled);
bool feishu_http_progress(void);

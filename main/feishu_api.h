#pragma once

#include "esp_err.h"
#include "feishu_model.h"
#include "feishu_page.h"
#include "feishu_store.h"
#include "feishu_outbox.h"
#include "feishu_data.h"
#include "feishu_image.h"
#include "feishu_outgoing_mentions.h"

#include <stddef.h>
#include <stdint.h>

#define FEISHU_TENANT_TOKEN_MAX 2048


// Distinct from malformed JSON, missing permissions and transport failures.
#define FEISHU_ERR_AUTH_REVOKED ((esp_err_t)0x7f01)
#define FEISHU_ERR_ASR_LIMITED ((esp_err_t)0x7f14)
#define FEISHU_ERR_VOICE_LOGIN ((esp_err_t)0x7f15)
#define FEISHU_ERR_ASR_TOO_LONG ((esp_err_t)0x7f16)
#define FEISHU_ERR_APP_REJECTED ((esp_err_t)0x7f13)
#define FEISHU_ERR_ACCESS_REJECTED ((esp_err_t)0x7f02)
#define FEISHU_ERR_CREDENTIAL_SAVE ((esp_err_t)0x7f03)

typedef struct {
    char user_access_token[FEISHU_TOKEN_MAX];
    char tenant_access_token[FEISHU_TENANT_TOKEN_MAX];
    char app_id[FEISHU_APP_ID_MAX];
    char app_secret[FEISHU_APP_SECRET_MAX];
    char self_open_id[FEISHU_CHAT_ID_MAX];
    char last_sent_id[FEISHU_MESSAGE_ID_MAX]; /* Only a server-acknowledged ID. */
    char last_sent_preview[FEISHU_TEXT_MAX];
    uint64_t last_sent_length;
    bool defer_list_bodies; /* Active assistant: cache list previews, fetch body on open. */
} feishu_api_session_t;

esp_err_t feishu_api_authenticate(feishu_api_session_t *session,
                                  feishu_credentials_t *credentials);
esp_err_t feishu_api_list_chats(feishu_api_session_t *session,
                                feishu_chat_t *chats, size_t capacity,
                                size_t *count);
esp_err_t feishu_api_chat_page(feishu_api_session_t *session,const char *token,
                                feishu_chat_t *chats,size_t capacity,feishu_page_cursor_t *page);
esp_err_t feishu_api_list_messages(feishu_api_session_t *session,
                                   const char *chat_id,
                                   feishu_message_t *messages,
                                   size_t capacity, size_t *count);
esp_err_t feishu_api_message_page(feishu_api_session_t *session,const char *chat_id,const char *token,
                                   feishu_message_t *messages,size_t capacity,feishu_page_cursor_t *page);
/* Paged UI previews: no synchronous body compression/Flash commits. */
esp_err_t feishu_api_message_preview_page(feishu_api_session_t *session,const char *chat_id,const char *token,
                                   feishu_message_t *messages,size_t capacity,feishu_page_cursor_t *page);
/* Ephemeral latest-message preview for an assistant turn. No text cache writes,
 * compression, or pagination workspace; raw mention keys may remain. */
esp_err_t feishu_api_message_preview(feishu_api_session_t *session,const char *chat_id,
                                   feishu_message_t *message,bool *present);
esp_err_t feishu_api_get_message(feishu_api_session_t *session,const char *message_id,
                                  feishu_message_t *message);
/* UI-safe cooperative priority change; never closes a worker-owned socket. */
void feishu_api_cancel_prefetch(void);
esp_err_t feishu_api_reply_text(feishu_api_session_t *session,
                                const char *message_id, const char *text,
                                const char *idempotency_key);
esp_err_t feishu_api_send_text(feishu_api_session_t *session,
                               const char *chat_id, const char *text,
                               const char *idempotency_key);
esp_err_t feishu_api_send_pending_part(feishu_api_session_t *session,const feishu_outbox_t *pending,
                                        uint64_t *sent_bytes,bool *definitely_rejected);
esp_err_t feishu_api_download_image(feishu_api_session_t *session,
                                    const feishu_message_t *message,
                                    const char *path, size_t limit);
esp_err_t feishu_api_prepare_image(feishu_api_session_t *session,const feishu_message_t *message,
                                     const feishu_image_view_t *view,bool allow_network,feishu_prepared_image_t *image);
esp_err_t feishu_api_asr_stream_packet(feishu_api_session_t *session,
                                       const char stream_id[17],
                                       uint32_t sequence_id, int action,
                                       const int16_t *pcm, size_t samples,
                                       char *recognition, size_t recognition_size,
                                       feishu_data_writer_t *final_draft,const volatile bool *cancel_requested);
void feishu_api_asr_stream_close(void);
typedef bool (*feishu_asr_write_t)(void *,const void *,size_t);
typedef bool (*feishu_asr_upload_t)(void *,feishu_asr_write_t,void *);
/* Stream several capture blocks into one request, returning buffers as soon as
 * socket writes consume them; no whole-request PCM allocation. */
esp_err_t feishu_api_internal_asr_upload(const char *cookie,const char *source,const char *context_id,
    uint32_t sequence,unsigned action,size_t pcm_bytes,feishu_asr_upload_t upload_pcm,void *upload_context,
    char *recognition,size_t recognition_size,uint64_t *last_length,
    feishu_data_writer_t *final_draft,const volatile bool *cancel_requested,const volatile esp_err_t *capture_error);
esp_err_t feishu_api_internal_asr_packet(const char *cookie,const char *source,const char *context_id,
    uint32_t sequence,unsigned action,const int16_t *pcm,size_t samples,
    char *recognition,size_t recognition_size,uint64_t *last_length,
    feishu_data_writer_t *final_draft,const volatile bool *cancel_requested,const volatile esp_err_t *capture_error);

/* Resolve through this owner's app-scoped OAuth identity; never copy open IDs
 * from another bot/CLI application. Fails closed on incomplete/ambiguous lists. */
esp_err_t feishu_api_resolve_mention(feishu_api_session_t *session,const char *chat,
    const char *name,feishu_mention_t *result,const volatile bool *cancel);

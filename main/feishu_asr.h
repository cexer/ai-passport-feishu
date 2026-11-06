#pragma once

#include "esp_err.h"
#include "feishu_api.h"

#include <stdbool.h>
/* NULL draft selects a RAM-only result with recognition_size as a strict full
 * text limit. Existing persistent drafts remain available via a writer. */

esp_err_t feishu_asr_record(feishu_api_session_t *session,
                            volatile bool *stop_requested,
                            volatile bool *cancel_requested,
                            volatile unsigned *elapsed_ms,
                            char *recognition, size_t recognition_size,
                            feishu_data_writer_t *draft,volatile bool *preparing);
#ifdef FEISHU_VOICE_AUTOTEST
/* Diagnostic-only local provisioning, without rerunning the separate suite. */
#ifdef FEISHU_VOICE_AUTOTEST_PROVISION
bool feishu_asr_autotest_provision(void);
#endif
void feishu_asr_autotest(feishu_api_session_t *session);
void feishu_asr_autotest_prepare(volatile bool *stop,volatile bool *cancel);
void feishu_asr_autotest_finish(void);
#endif

/* Worker-context resource handoff. Pause optional background work before
 * allocating the audio codec; resume only after all audio/network owners join. */
void feishu_asr_set_resource_hooks(esp_err_t (*acquire)(void),void (*release)(void));

/* Worker-owned optional microphone warm-up. Input is discarded, never stored
 * or uploaded. cancel is non-blocking for UI focus changes; prepare(false)
 * joins the owner. A record call takes ownership without reopening the codec. */
esp_err_t feishu_asr_prepare(bool enable);
void feishu_asr_prepare_cancel(void);
bool feishu_asr_prepare_ready(void);

#define FEISHU_ERR_XIAO_PAIRING ((esp_err_t)0x7f17)
typedef struct {
    bool supported,paired;
    char code[17];
    char device_id[18];
    uint32_t expires_at_ms;
} feishu_asr_pairing_t;
/* Worker-only. Returns no login token; cancel is non-blocking from the UI. */
esp_err_t feishu_asr_pairing_check(feishu_asr_pairing_t *info,const volatile bool *cancel);
void feishu_asr_pairing_snapshot(feishu_asr_pairing_t *info);
#ifdef FEISHU_MCP_DIAGNOSTIC
#include "feishu_mcp.h"
esp_err_t feishu_asr_mcp_probe(feishu_api_session_t *session,const char *chat,
    const feishu_mcp_status_t *snapshot);
#endif

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

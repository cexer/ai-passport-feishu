#pragma once
#include "esp_err.h"
#include "feishu_mcp.h"
#include <stdbool.h>

typedef enum { FEISHU_ASSISTANT_HEARD,FEISHU_ASSISTANT_REPLY,FEISHU_ASSISTANT_TOOL,FEISHU_ASSISTANT_LISTENING,FEISHU_ASSISTANT_THINKING,FEISHU_ASSISTANT_PREPARING,FEISHU_ASSISTANT_RECONNECTING,FEISHU_ASSISTANT_GREETED,FEISHU_ASSISTANT_PAUSED } feishu_assistant_event_t;
typedef struct {
    feishu_mcp_status_t status;
    feishu_mcp_execute_t execute;
    void (*event)(void *context,feishu_assistant_event_t event,const char *text);
    void *context;
    unsigned (*get_volume)(void *context);
    unsigned volume; /* 0 for silent diagnostics, 1..100 for user sessions. */
    bool continuous;
    bool activation_chime;
    bool skip_greeting; /* Same activation resumed after releasing the voice worker. */
    volatile bool *speech_detected; /* Capture-owned flag: OK submits speech before confirming a send. */
    unsigned silence_seconds; /* 0 = OK, otherwise 2..5. */
    bool (*continue_session)(void *context);
    bool (*work_pending)(void *context);
    bool (*reply_work_pending)(void *context,bool playing);
    bool (*can_listen)(void *context);
    bool (*waiting_confirmation)(void *context);
    bool (*diagnostic_ready)(void *context); /* NULL in ordinary firmware. */
    unsigned (*get_silence)(void *context);
    void (*feedback)(void *context,bool capture);
} feishu_assistant_hooks_t;
/* Continuous visits or explicit single-turn diagnostics. OK submits capture; long OK cancels.
 * Unlike ordinary ASR, keeps WSS open for genuine cloud MCP and reply audio.
 * All hooks run on the calling worker. Never call from a UI callback. */
esp_err_t feishu_assistant_run(volatile bool *stop,volatile bool *cancel,
    volatile unsigned *elapsed,volatile bool *preparing,const feishu_assistant_hooks_t *hooks);
#ifdef FEISHU_ASSISTANT_AUTOTEST
esp_err_t feishu_assistant_autotest_run(volatile bool *stop,volatile bool *cancel,
    volatile unsigned *elapsed,volatile bool *preparing,const feishu_assistant_hooks_t *hooks,unsigned test_case);
#endif

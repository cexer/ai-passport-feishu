#pragma once
#include <stdbool.h>
#include <stddef.h>

#define FEISHU_MCP_INPUT_MAX 4096u
#define FEISHU_MCP_OUTPUT_MAX 2048u
typedef struct {
    const char *page;
    unsigned conversations,messages;
    int battery_percent;
    bool wifi_connected;
} feishu_mcp_status_t;
typedef enum {
    FEISHU_MCP_IGNORE,FEISHU_MCP_INITIALIZED,FEISHU_MCP_LISTED,
    FEISHU_MCP_STATUS,FEISHU_MCP_ERROR,FEISHU_MCP_TOOL
} feishu_mcp_result_t;

struct cJSON;
/* Worker-only tools. Sending requires a current revision and explicit later-turn
 * user confirmation (or physical OK). No credential/reset tool is exposed. */
typedef bool (*feishu_mcp_execute_t)(void *context,const char *name,
    const struct cJSON *arguments,char *text,size_t capacity);
feishu_mcp_result_t feishu_mcp_dispatch(const char *request,size_t length,
    const char *session,const feishu_mcp_status_t *status,char *output,size_t capacity,
    feishu_mcp_execute_t execute,void *context);

/* Bounded, read-only protocol prototype. The caller supplies a locked snapshot;
 * there are no UI, network, storage or message-send side effects here. */
feishu_mcp_result_t feishu_mcp_reply(const char *request,size_t length,
    const char *session,const feishu_mcp_status_t *status,char *output,size_t capacity);

#pragma once
#include <stdbool.h>
#include <stddef.h>

#define FEISHU_MCP_INPUT_MAX 4096u
#define FEISHU_MCP_OUTPUT_MAX 1024u
typedef struct {
    const char *page;
    unsigned conversations,messages;
    int battery_percent;
    bool wifi_connected;
} feishu_mcp_status_t;
typedef enum {
    FEISHU_MCP_IGNORE,FEISHU_MCP_INITIALIZED,FEISHU_MCP_LISTED,
    FEISHU_MCP_STATUS,FEISHU_MCP_ERROR
} feishu_mcp_result_t;

/* Bounded, read-only protocol prototype. The caller supplies a locked snapshot;
 * there are no UI, network, storage or message-send side effects here. */
feishu_mcp_result_t feishu_mcp_reply(const char *request,size_t length,
    const char *session,const feishu_mcp_status_t *status,char *output,size_t capacity);

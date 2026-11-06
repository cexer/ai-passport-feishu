#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef enum { FEISHU_MCP_WIRE_ERROR=-1,FEISHU_MCP_WIRE_NONE,
    FEISHU_MCP_WIRE_TEXT,FEISHU_MCP_WIRE_PING,FEISHU_MCP_WIRE_PONG,FEISHU_MCP_WIRE_CLOSE } feishu_mcp_wire_event_t;
typedef struct {
    size_t used,remaining,payload,control_used;
    unsigned opcode;
    bool frame,assembling,fin;
    uint8_t control[125];
} feishu_mcp_wire_t;
/* Feed unmasked SDK payload chunks, including propagated control frames.
 * Text fragments survive interleaved control frames. No allocation or I/O. */
feishu_mcp_wire_event_t feishu_mcp_wire_feed(feishu_mcp_wire_t *state,unsigned opcode,
    size_t payload,bool fin,const void *bytes,size_t length,char *text,size_t capacity);

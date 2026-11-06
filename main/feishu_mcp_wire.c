#include "feishu_mcp_wire.h"
#include <string.h>
feishu_mcp_wire_event_t feishu_mcp_wire_feed(feishu_mcp_wire_t *s,unsigned opcode,
    size_t payload,bool fin,const void *bytes,size_t n,char *text,size_t capacity)
{
    if(!s || (!bytes&&n) || !text || !capacity)return FEISHU_MCP_WIRE_ERROR;
    bool control=opcode==8 || opcode==9 || opcode==10;
    if(!s->frame) {
        if(control) {
            if(!fin || payload>sizeof(s->control))return FEISHU_MCP_WIRE_ERROR;
            s->control_used=0;
        } else if(opcode==1) {
            if(s->assembling)return FEISHU_MCP_WIRE_ERROR;
            s->used=0;s->assembling=true;
        } else if(opcode!=0 || !s->assembling)return FEISHU_MCP_WIRE_ERROR;
        if(!control && (s->used>=capacity || payload>=capacity-s->used))return FEISHU_MCP_WIRE_ERROR;
        s->frame=true;s->remaining=s->payload=payload;s->opcode=opcode;s->fin=fin;
    }
    if(opcode!=s->opcode || payload!=s->payload || fin!=s->fin || n>s->remaining)return FEISHU_MCP_WIRE_ERROR;
    if(control) {
        if(n>sizeof(s->control)-s->control_used)return FEISHU_MCP_WIRE_ERROR;
        if(n)memcpy(s->control+s->control_used,bytes,n);
        s->control_used+=n;
    } else {
        if(s->used>=capacity || n>=capacity-s->used)return FEISHU_MCP_WIRE_ERROR;
        if(n)memcpy(text+s->used,bytes,n);
        s->used+=n;text[s->used]=0;
    }
    s->remaining-=n;
    if(s->remaining)return FEISHU_MCP_WIRE_NONE;
    s->frame=false;
    if(opcode==8)return FEISHU_MCP_WIRE_CLOSE;
    if(opcode==9)return FEISHU_MCP_WIRE_PING;
    if(opcode==10)return FEISHU_MCP_WIRE_PONG;
    if(fin){s->assembling=false;return FEISHU_MCP_WIRE_TEXT;}
    return FEISHU_MCP_WIRE_NONE;
}

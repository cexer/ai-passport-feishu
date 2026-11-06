#include "feishu_mcp_wire.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    feishu_mcp_wire_t s={0};char text[4097]={0};
    assert(feishu_mcp_wire_feed(&s,1,3,false,"a",1,text,sizeof(text))==FEISHU_MCP_WIRE_NONE);
    assert(feishu_mcp_wire_feed(&s,1,3,false,"bc",2,text,sizeof(text))==FEISHU_MCP_WIRE_NONE);
    assert(feishu_mcp_wire_feed(&s,9,2,true,"p",1,text,sizeof(text))==FEISHU_MCP_WIRE_NONE);
    assert(feishu_mcp_wire_feed(&s,9,2,true,"i",1,text,sizeof(text))==FEISHU_MCP_WIRE_PING);
    assert(s.control_used==2 && !memcmp(s.control,"pi",2) && !strcmp(text,"abc"));
    assert(feishu_mcp_wire_feed(&s,10,0,true,NULL,0,text,sizeof(text))==FEISHU_MCP_WIRE_PONG);
    assert(feishu_mcp_wire_feed(&s,0,3,true,"def",3,text,sizeof(text))==FEISHU_MCP_WIRE_TEXT);
    assert(!strcmp(text,"abcdef") && !s.assembling);
    memset(&s,0,sizeof(s));
    const char ping[]="mcp-probe";
    for(unsigned i=0;i<9;++i) {
        feishu_mcp_wire_event_t e=feishu_mcp_wire_feed(&s,10,9,true,&ping[i],1,text,sizeof(text));
        assert(e==(i==8 ? FEISHU_MCP_WIRE_PONG:FEISHU_MCP_WIRE_NONE));
    }
    assert(s.control_used==9 && !memcmp(s.control,"mcp-probe",9));
    memset(&s,0,sizeof(s));
    assert(feishu_mcp_wire_feed(&s,9,126,true,NULL,0,text,sizeof(text))==FEISHU_MCP_WIRE_ERROR);
    memset(&s,0,sizeof(s));assert(feishu_mcp_wire_feed(&s,9,0,false,NULL,0,text,sizeof(text))==FEISHU_MCP_WIRE_ERROR);
    memset(&s,0,sizeof(s));assert(feishu_mcp_wire_feed(&s,0,1,true,"a",1,text,sizeof(text))==FEISHU_MCP_WIRE_ERROR);
    memset(&s,0,sizeof(s));assert(feishu_mcp_wire_feed(&s,2,1,true,"a",1,text,sizeof(text))==FEISHU_MCP_WIRE_ERROR);
    memset(&s,0,sizeof(s));assert(feishu_mcp_wire_feed(&s,1,4097,true,NULL,0,text,sizeof(text))==FEISHU_MCP_WIRE_ERROR);
    memset(&s,0,sizeof(s));assert(feishu_mcp_wire_feed(&s,1,1,true,"ab",2,text,sizeof(text))==FEISHU_MCP_WIRE_ERROR);
    memset(&s,0,sizeof(s));assert(feishu_mcp_wire_feed(&s,1,1,false,"a",1,text,sizeof(text))==FEISHU_MCP_WIRE_NONE);
    assert(feishu_mcp_wire_feed(&s,1,1,true,"b",1,text,sizeof(text))==FEISHU_MCP_WIRE_ERROR);
    memset(&s,0,sizeof(s));assert(feishu_mcp_wire_feed(&s,8,2,true,"\x03\xe8",2,text,sizeof(text))==FEISHU_MCP_WIRE_CLOSE);
    memset(&s,0,sizeof(s));assert(feishu_mcp_wire_feed(&s,1,2,true,"ok",2,text,sizeof(text))==FEISHU_MCP_WIRE_TEXT);
    puts("MCP WS: split control payloads, fragmented text, interleaved ping/pong, bounds, close and reset recovery PASS");
}

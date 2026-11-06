#include "feishu_mcp.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static feishu_mcp_status_t snapshot={"messages",7,8,96,true};
static char output[FEISHU_MCP_OUTPUT_MAX];
static const char *status_request="{\"type\":\"mcp\",\"session_id\":\"test\",\"payload\":{\"jsonrpc\":\"2.0\",\"id\":\"a\\\"b\",\"method\":\"tools/call\",\"params\":{\"name\":\"self.feishu.get_status\",\"arguments\":{}}}}";
static feishu_mcp_result_t call(const char *request)
{return feishu_mcp_reply(request,strlen(request),"test",&snapshot,output,sizeof(output));}
static void *fail_alloc(size_t ignored){(void)ignored;return NULL;}
int main(void)
{
    assert(call(status_request)==FEISHU_MCP_STATUS);
    cJSON *root=cJSON_Parse(output);assert(root);
    cJSON *payload=cJSON_GetObjectItemCaseSensitive(root,"payload");
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(payload,"id")->valuestring,"a\"b"));
    cJSON *result=cJSON_GetObjectItemCaseSensitive(payload,"result");
    cJSON *entry=cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(result,"content"),0);
    cJSON *value=cJSON_Parse(cJSON_GetObjectItemCaseSensitive(entry,"text")->valuestring);assert(value);
    assert(cJSON_GetObjectItemCaseSensitive(value,"conversations")->valueint==7);
    assert(!cJSON_GetObjectItemCaseSensitive(value,"token"));cJSON_Delete(value);cJSON_Delete(root);
    assert(call("{\"type\":\"mcp\",\"payload\":{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}}")==FEISHU_MCP_INITIALIZED);
    assert(call("{\"type\":\"mcp\",\"payload\":{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}}")==FEISHU_MCP_LISTED);
    assert(strstr(output,"self.feishu.get_status") && !strstr(output,"send_message"));
    assert(call("{\"type\":\"mcp\",\"payload\":{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}}")==FEISHU_MCP_IGNORE && !output[0]);
    assert(call("{\"type\":\"hello\"}")==FEISHU_MCP_IGNORE);
    const char *bad[]={
        "{\"type\":\"mcp\",\"session_id\":\"wrong\",\"payload\":{}}",
        "{\"type\":\"mcp\",\"type\":\"mcp\"}",
        "{\"type\":\"mcp\",\"payload\":{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"self.feishu.send_message\"}}}",
        "{\"type\":\"mcp\",\"payload\":{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"self.feishu.get_status\",\"arguments\":{\"recipient\":\"x\"}}}}",
        "{\"type\":\"mcp\",\"payload\":{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\",\"params\":{\"cursor\":\"next\"}}}",
        "{\"type\":\"mcp\",\"payload\":{\"jsonrpc\":\"2.0\",\"id\":1.5,\"method\":\"tools/list\"}}",
        "{\"type\":\"mcp\",\"payload\":{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\",\"method\":\"tools/call\"}}",
        "{\"type\":\"mcp\",\"unknown\":\"\\u0000\"}","{}{}","{","[[]]"
    };
    for(size_t i=0;i<sizeof(bad)/sizeof(bad[0]);++i)assert(call(bad[i])==FEISHU_MCP_ERROR);
    char deep[256];size_t n=0;for(unsigned i=0;i<32;++i)deep[n++]='[';for(unsigned i=0;i<32;++i)deep[n++]=']';deep[n]=0;
    assert(call(deep)==FEISHU_MCP_ERROR);
    char many[700];size_t used=(size_t)snprintf(many,sizeof(many),"{\"type\":\"mcp\",\"junk\":[");
    for(unsigned i=0;i<256;++i)used+=(size_t)snprintf(many+used,sizeof(many)-used,"0,");
    snprintf(many+used,sizeof(many)-used,"0]}");
    assert(call(many)==FEISHU_MCP_ERROR && !output[0]);
    assert(call("{\"type\":\"mcp\",\"junk\":\"\xff\"}")==FEISHU_MCP_ERROR);
    char large[FEISHU_MCP_INPUT_MAX+1];memset(large,' ',sizeof(large));
    assert(feishu_mcp_reply(large,sizeof(large),"test",&snapshot,output,sizeof(output))==FEISHU_MCP_ERROR);
    char tiny[8]="old";
    assert(feishu_mcp_reply(status_request,strlen(status_request),"test",&snapshot,tiny,sizeof(tiny))==FEISHU_MCP_ERROR && !tiny[0]);
    cJSON_Hooks hooks={fail_alloc,free};cJSON_InitHooks(&hooks);
    assert(call(status_request)==FEISHU_MCP_ERROR);cJSON_InitHooks(NULL);
    for(unsigned i=0;i<1000;++i)assert(call(status_request)==FEISHU_MCP_STATUS);
    puts("MCP read-only: negotiation, typed status, escaping, duplicate/session/size/depth rejection, OOM and recovery PASS");
}

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
static unsigned executed;
static bool execution_ok=true;
static char *owned_request;
static bool execute(void *context,const char *name,const cJSON *args,char *text,size_t capacity)
{
    assert(context==&executed && name && cJSON_IsObject(args));++executed;
    if(owned_request){free(owned_request);owned_request=NULL;assert(cJSON_IsObject(args));}
    snprintf(text,capacity,"{\"result\":\"draft prepared, NOT sent\"}");return execution_ok;
}
static feishu_mcp_result_t dispatch(const char *name,const char *args,size_t capacity)
{
    char request[2048];snprintf(request,sizeof(request),"{\"type\":\"mcp\",\"session_id\":\"test\",\"payload\":{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\",\"params\":{\"name\":\"%s\",\"arguments\":%s}}}",name,args);
    return feishu_mcp_dispatch(request,strlen(request),"test",&snapshot,output,capacity,execute,&executed);
}
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
    root=cJSON_Parse(output);assert(root);
    payload=cJSON_GetObjectItemCaseSensitive(root,"payload");
    result=cJSON_GetObjectItemCaseSensitive(payload,"result");
    const cJSON *instructions=cJSON_GetObjectItemCaseSensitive(result,"instructions");
    assert(cJSON_IsString(instructions) && strstr(instructions->valuestring,"wait for its result"));
    assert(strstr(instructions->valuestring,"Do not greet again"));cJSON_Delete(root);
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
    const char *names[]={"list_chats","get_messages","open_chat","navigate","prepare_message","get_context","read_draft","edit_draft","undo_draft","open_composer","request_send","settings","find_member"};
    for(unsigned i=0;i<sizeof(names)/sizeof(*names);++i) {
        char request[256];snprintf(request,sizeof(request),"{\"type\":\"mcp\",\"payload\":{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\",\"params\":{\"cursor\":\"%u\"}}}",i);
        assert(feishu_mcp_dispatch(request,strlen(request),"test",&snapshot,output,sizeof(output),execute,&executed)==FEISHU_MCP_LISTED);
        assert(strstr(output,names[i]));root=cJSON_Parse(output);assert(root);cJSON_Delete(root);
        assert((strstr(output,"nextCursor")!=NULL)==(i+1<sizeof(names)/sizeof(*names)));
    }
    assert(!executed);
    assert(dispatch("self.feishu.find_member","{\"name\":\"Neo\"}",sizeof(output))==FEISHU_MCP_TOOL);
    assert(dispatch("self.feishu.find_member","{\"name\":\"Neo\",\"chat_index\":0}",sizeof(output))==FEISHU_MCP_TOOL);
    assert(dispatch("self.feishu.find_member","{\"name\":\"Neo\",\"chat_id\":\"oc_123\"}",sizeof(output))==FEISHU_MCP_TOOL);
    assert(dispatch("self.feishu.find_member","{}",sizeof(output))==FEISHU_MCP_ERROR);
    assert(dispatch("self.feishu.find_member","{\"name\":\"\"}",sizeof(output))==FEISHU_MCP_ERROR);
    assert(dispatch("self.feishu.find_member","{\"name\":\"Neo\",\"chat_index\":0,\"chat_id\":\"oc_123\"}",sizeof(output))==FEISHU_MCP_ERROR);
    assert(dispatch("self.feishu.find_member","{\"name\":\"Neo\",\"member_id\":\"ou_invented\"}",sizeof(output))==FEISHU_MCP_ERROR);

    assert(dispatch("self.feishu.list_chats","{}",sizeof(output))==FEISHU_MCP_TOOL);
    assert(dispatch("self.feishu.get_messages","{\"chat_id\":\"oc_123\"}",sizeof(output))==FEISHU_MCP_TOOL);
    assert(dispatch("self.feishu.open_chat","{\"chat_id\":\"oc_123\"}",sizeof(output))==FEISHU_MCP_TOOL);
    assert(dispatch("self.feishu.navigate","{\"page\":\"drafts\"}",sizeof(output))==FEISHU_MCP_TOOL);
    assert(dispatch("self.feishu.prepare_message","{\"chat_id\":\"oc_123\",\"text\":\"hello\"}",sizeof(output))==FEISHU_MCP_TOOL);

    assert(dispatch("self.feishu.get_context","{}",sizeof(output))==FEISHU_MCP_TOOL);
    assert(dispatch("self.feishu.read_draft","{\"offset\":128}",sizeof(output))==FEISHU_MCP_TOOL);
    assert(dispatch("self.feishu.edit_draft","{\"revision\":\"d1234567890abcdef\",\"text\":\"Revised text\"}",sizeof(output))==FEISHU_MCP_TOOL);
    assert(dispatch("self.feishu.undo_draft","{\"revision\":\"d1234567890abcdef\"}",sizeof(output))==FEISHU_MCP_TOOL);
    assert(dispatch("self.feishu.open_composer","{\"chat_index\":0}",sizeof(output))==FEISHU_MCP_TOOL);
    assert(dispatch("self.feishu.open_composer","{}",sizeof(output))==FEISHU_MCP_TOOL);
    assert(dispatch("self.feishu.open_composer","{\"chat_index\":0,\"chat_id\":\"oc_123\"}",sizeof(output))==FEISHU_MCP_ERROR);
    assert(dispatch("self.feishu.open_chat","{}",sizeof(output))==FEISHU_MCP_ERROR);
    assert(dispatch("self.feishu.request_send","{\"revision\":\"d1234567890abcdef\"}",sizeof(output))==FEISHU_MCP_TOOL);
    assert(dispatch("self.feishu.request_send","{}",sizeof(output))==FEISHU_MCP_ERROR);
    assert(dispatch("self.feishu.request_send","{\"revision\":\"d1234567890abcdef\",\"text\":\"other\"}",sizeof(output))==FEISHU_MCP_ERROR);
    assert(dispatch("self.feishu.prepare_message","{\"chat_index\":0,\"text\":\"hello\",\"mentions\":[\"Neo\"]}",sizeof(output))==FEISHU_MCP_TOOL);
    assert(dispatch("self.feishu.edit_draft","{\"revision\":\"d1234567890abcdef\",\"text\":\"hello\",\"mentions\":[]}",sizeof(output))==FEISHU_MCP_TOOL);
    unsigned before=executed;assert(before==17);
    assert(dispatch("self.feishu.prepare_message","{\"chat_index\":0,\"text\":\"hello\",\"mentions\":[1]}",sizeof(output))==FEISHU_MCP_ERROR);
    assert(dispatch("self.feishu.prepare_message","{\"chat_index\":0,\"text\":\"hello\",\"mentions\":[\"a\",\"b\",\"c\"]}",sizeof(output))==FEISHU_MCP_ERROR);
    assert(dispatch("self.feishu.edit_draft","{\"text\":\"missing revision\"}",sizeof(output))==FEISHU_MCP_ERROR);
    assert(dispatch("self.feishu.edit_draft","{\"revision\":\"d1234567890abcdef\",\"text\":\"ok\",\"chat_id\":\"oc_wrong\"}",sizeof(output))==FEISHU_MCP_ERROR);
    assert(dispatch("self.feishu.undo_draft","{\"revision\":\"d1234567890abcdeg\"}",sizeof(output))==FEISHU_MCP_ERROR);
    assert(dispatch("self.feishu.get_context","{\"token\":true}",sizeof(output))==FEISHU_MCP_ERROR);
    assert(dispatch("self.feishu.read_draft","{\"offset\":512}",sizeof(output))==FEISHU_MCP_ERROR);
    const char *invalid[]={"{}","{\"chat_id\":1}","{\"chat_id\":\"\"}","{\"chat_id\":\"oc_123\",\"text\":\"\"}",
        "{\"chat_id\":\"oc_123\",\"text\":\"ok\",\"send\":true}","{\"chat_id\":\"oc_123\",\"chat_id\":\"other\",\"text\":\"ok\"}"};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i)assert(dispatch("self.feishu.prepare_message",invalid[i],sizeof(output))==FEISHU_MCP_ERROR);
    assert(dispatch("self.feishu.navigate","{\"page\":\"erase\"}",sizeof(output))==FEISHU_MCP_ERROR);
    assert(dispatch("self.feishu.list_chats","{\"offset\":1.5}",sizeof(output))==FEISHU_MCP_ERROR);
    assert(dispatch("self.feishu.list_chats","{\"offset\":-1}",sizeof(output))==FEISHU_MCP_ERROR);
    assert(dispatch("self.feishu.send_message","{\"chat_id\":\"oc_123\",\"text\":\"ok\"}",sizeof(output))==FEISHU_MCP_ERROR);
    assert(dispatch("self.feishu.prepare_message","{\"chat_id\":\"oc_123\",\"text\":\"hello\"}",64)==FEISHU_MCP_ERROR);
    assert(executed==before);
    assert(dispatch("self.feishu.get_messages","{\"chat_index\":0}",sizeof(output))==FEISHU_MCP_TOOL);
    assert(dispatch("self.feishu.prepare_message","{\"chat_index\":0,\"text\":\"saved draft\"}",sizeof(output))==FEISHU_MCP_TOOL);
    before=executed;
    assert(dispatch("self.feishu.get_messages","{\"chat_index\":10}",sizeof(output))==FEISHU_MCP_ERROR);
    assert(dispatch("self.feishu.get_messages","{\"chat_index\":-1}",sizeof(output))==FEISHU_MCP_ERROR);
    assert(dispatch("self.feishu.get_messages","{\"chat_index\":0.5}",sizeof(output))==FEISHU_MCP_ERROR);
    assert(dispatch("self.feishu.get_messages","{\"chat_index\":0,\"chat_id\":\"oc_123\"}",sizeof(output))==FEISHU_MCP_ERROR);
    assert(dispatch("self.feishu.get_messages","{}",sizeof(output))==FEISHU_MCP_ERROR);
    assert(executed==before);
    char long_text[210],long_args[300];memset(long_text,'x',200);long_text[200]=0;
    snprintf(long_args,sizeof(long_args),"{\"chat_index\":0,\"text\":\"%s\"}",long_text);
    assert(dispatch("self.feishu.prepare_message",long_args,sizeof(output))==FEISHU_MCP_ERROR && executed==before);
    execution_ok=false;assert(dispatch("self.feishu.list_chats","{}",sizeof(output))==FEISHU_MCP_TOOL && strstr(output,"\"isError\":true"));
    execution_ok=true;
    for(unsigned i=0;i<1000;++i)assert(dispatch("self.feishu.list_chats","{}",sizeof(output))==FEISHU_MCP_TOOL);
    const char *lifetime="{\"type\":\"mcp\",\"payload\":{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\",\"params\":{\"name\":\"self.feishu.list_chats\",\"arguments\":{}}}}";
    owned_request=malloc(strlen(lifetime)+1);assert(owned_request);memcpy(owned_request,lifetime,strlen(lifetime)+1);
    assert(feishu_mcp_dispatch(owned_request,strlen(owned_request),"test",&snapshot,output,sizeof(output),execute,&executed)==FEISHU_MCP_TOOL && !owned_request);
    root=cJSON_Parse(output);assert(root);cJSON_Delete(root);
    assert(dispatch("self.feishu.settings","{\"action\":\"get\"}",sizeof(output))==FEISHU_MCP_TOOL);
    assert(dispatch("self.feishu.settings","{\"action\":\"set\",\"key\":\"volume\",\"delta\":10}",sizeof(output))==FEISHU_MCP_TOOL);
    assert(dispatch("self.feishu.settings","{\"action\":\"set\",\"key\":\"boot\",\"value\":true}",sizeof(output))==FEISHU_MCP_TOOL);
    assert(dispatch("self.feishu.settings","{\"action\":\"set\",\"key\":\"silence_seconds\",\"value\":0}",sizeof(output))==FEISHU_MCP_TOOL);
    assert(dispatch("self.feishu.settings","{\"action\":\"set\",\"key\":\"timeout_minutes\",\"value\":60}",sizeof(output))==FEISHU_MCP_TOOL);
    const char *bad_settings[]={"{}","{\"action\":\"get\",\"key\":\"volume\"}",
        "{\"action\":\"set\",\"key\":\"volume\",\"value\":101}","{\"action\":\"set\",\"key\":\"volume\",\"value\":1.5}",
        "{\"action\":\"set\",\"key\":\"volume\",\"delta\":10,\"value\":20}","{\"action\":\"set\",\"key\":\"boot\",\"value\":1}",
        "{\"action\":\"set\",\"key\":\"silence_seconds\",\"value\":1}","{\"action\":\"set\",\"key\":\"timeout_minutes\",\"value\":61}",
        "{\"action\":\"set\",\"key\":\"token\",\"value\":true}"};
    before=executed;for(unsigned i=0;i<sizeof(bad_settings)/sizeof(*bad_settings);++i)
        assert(dispatch("self.feishu.settings",bad_settings[i],sizeof(output))==FEISHU_MCP_ERROR);
    assert(executed==before);
    assert(dispatch("self.feishu.request_send","{\"revision\":\"d1234567890abcdef\",\"confirmed\":true}",sizeof(output))==FEISHU_MCP_TOOL);
    assert(dispatch("self.feishu.request_send","{\"revision\":\"d1234567890abcdef\",\"confirmed\":1}",sizeof(output))==FEISHU_MCP_ERROR);
    assert(dispatch("self.feishu.open_composer","{\"reply_to_message_id\":\"om_test\"}",sizeof(output))==FEISHU_MCP_TOOL);
    assert(dispatch("self.feishu.open_composer","{\"reply_to_message_id\":\"\"}",sizeof(output))==FEISHU_MCP_ERROR);
    assert(dispatch("self.feishu.navigate","{\"page\":\"xiaozhi_settings\"}",sizeof(output))==FEISHU_MCP_TOOL);
    assert(dispatch("self.feishu.navigate","{\"page\":\"network\"}",sizeof(output))==FEISHU_MCP_TOOL);
    puts("MCP read-only: negotiation, typed status, escaping, duplicate/session/size/depth rejection, OOM and recovery PASS");
    puts("MCP assistant: paginated tool discovery, schema validation before execution, no direct send, failure and recovery PASS");
}

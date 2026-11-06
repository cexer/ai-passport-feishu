#include "feishu_mcp.h"
#include "feishu_json.h"
#include "cJSON.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

static bool bounded(void *context,feishu_json_event_t event,unsigned depth,const uint8_t *bytes,size_t size)
{
    unsigned *nodes=context;
    if(depth>10)return false;
    if(event==FEISHU_JSON_OBJECT_BEGIN || event==FEISHU_JSON_ARRAY_BEGIN || event==FEISHU_JSON_KEY ||
       event==FEISHU_JSON_STRING_BEGIN || event==FEISHU_JSON_ATOM)
        if(++*nodes>128)return false;
    if(event==FEISHU_JSON_STRING_PART)for(size_t i=0;i<size;++i)if(!bytes[i])return false;
    return true;
}
static bool unique(const cJSON *node)
{
    if(cJSON_IsObject(node))for(const cJSON *a=node->child;a;a=a->next)
        for(const cJSON *b=a->next;b;b=b->next)if(!strcmp(a->string,b->string))return false;
    for(const cJSON *a=node->child;a;a=a->next)if(!unique(a))return false;
    return true;
}
static bool ascii(const char *text,size_t maximum)
{
    if(!text || strlen(text)>maximum)return false;
    for(;*text;++text)if((unsigned char)*text<32 || (unsigned char)*text>126)return false;
    return true;
}
static bool quote(char *out,size_t capacity,const char *text)
{
    size_t used=0;if(capacity<3)return false;out[used++]='"';
    for(;*text;++text) {
        if(*text=='"' || *text=='\\'){if(used+2>=capacity)return false;out[used++]='\\';}
        else if((unsigned char)*text<32)return false;
        if(used+1>=capacity)return false;
        out[used++]=*text;
    }
    if(used+2>capacity)return false;
    out[used++]='"';out[used]=0;return true;
}
feishu_mcp_result_t feishu_mcp_reply(const char *request,size_t length,
    const char *session,const feishu_mcp_status_t *status,char *output,size_t capacity)
{
    if(!output || !capacity)return FEISHU_MCP_ERROR;
    output[0]=0;
    if(!request || !length || length>FEISHU_MCP_INPUT_MAX || !ascii(session,95))return FEISHU_MCP_ERROR;
    unsigned nodes=0;feishu_json_t validator;feishu_json_init(&validator,bounded,&nodes);
    if(!feishu_json_feed(&validator,request,length) || !feishu_json_finish(&validator))return FEISHU_MCP_ERROR;
    cJSON *root=cJSON_ParseWithLengthOpts(request,length,NULL,false);
    if(!root)return FEISHU_MCP_ERROR;
    feishu_mcp_result_t kind=FEISHU_MCP_ERROR;int error=-32600;
    char id[128]="null",body[700]={0},session_json[196];
    if(!unique(root) || !cJSON_IsObject(root))goto finished;
    const cJSON *type=cJSON_GetObjectItemCaseSensitive(root,"type");
    if(!cJSON_IsString(type) || strcmp(type->valuestring,"mcp")){kind=FEISHU_MCP_IGNORE;goto finished;}
    const cJSON *sid=cJSON_GetObjectItemCaseSensitive(root,"session_id");
    if(sid && (!cJSON_IsString(sid) || strcmp(sid->valuestring,session)))goto finished;
    const cJSON *payload=cJSON_GetObjectItemCaseSensitive(root,"payload");
    if(!cJSON_IsObject(payload))goto finished;
    const cJSON *rpc=cJSON_GetObjectItemCaseSensitive(payload,"jsonrpc"),
        *method=cJSON_GetObjectItemCaseSensitive(payload,"method"),
        *request_id=cJSON_GetObjectItemCaseSensitive(payload,"id"),
        *params=cJSON_GetObjectItemCaseSensitive(payload,"params");
    if(!cJSON_IsString(rpc) || strcmp(rpc->valuestring,"2.0") || !cJSON_IsString(method) ||
       !ascii(method->valuestring,64) || (params && !cJSON_IsObject(params)))goto finished;
    if(!request_id){kind=FEISHU_MCP_IGNORE;goto finished;}
    if(cJSON_IsNumber(request_id) && request_id->valuedouble>=INT_MIN && request_id->valuedouble<=INT_MAX &&
       request_id->valuedouble==(double)request_id->valueint)snprintf(id,sizeof(id),"%d",request_id->valueint);
    else if(cJSON_IsString(request_id) && ascii(request_id->valuestring,48)) {
        if(!quote(id,sizeof(id),request_id->valuestring))goto finished;
    } else goto finished;
    if(!strcmp(method->valuestring,"initialize")) {
        snprintf(body,sizeof(body),"\"result\":{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"ai-passport-feishu\",\"version\":\"1\"}}");
        kind=FEISHU_MCP_INITIALIZED;
    } else if(!strcmp(method->valuestring,"tools/list")) {
        const cJSON *cursor=cJSON_GetObjectItemCaseSensitive(params,"cursor");
        if(cursor && (!cJSON_IsString(cursor) || cursor->valuestring[0])){error=-32602;goto finished;}
        snprintf(body,sizeof(body),"\"result\":{\"tools\":[{\"name\":\"self.feishu.get_status\",\"description\":\"Read device page, cached counts, battery and Wi-Fi status; no personal content or changes\",\"inputSchema\":{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}}]}");
        kind=FEISHU_MCP_LISTED;
    } else if(!strcmp(method->valuestring,"tools/call")) {
        error=-32602;
        const cJSON *name=cJSON_GetObjectItemCaseSensitive(params,"name"),
            *arguments=cJSON_GetObjectItemCaseSensitive(params,"arguments");
        if(!cJSON_IsString(name) || strcmp(name->valuestring,"self.feishu.get_status") ||
           (arguments && (!cJSON_IsObject(arguments) || arguments->child)))goto finished;
        if(!status || !ascii(status->page,31) || status->battery_percent < -1 || status->battery_percent>100)goto finished;
        char page[66],text[256],quoted[530];
        if(!quote(page,sizeof(page),status->page))goto finished;
        int n=snprintf(text,sizeof(text),"{\"page\":%s,\"conversations\":%u,\"cached_messages\":%u,\"battery_percent\":%d,\"wifi_connected\":%s}",
            page,status->conversations,status->messages,status->battery_percent,status->wifi_connected?"true":"false");
        if(n<0 || (size_t)n>=sizeof(text) || !quote(quoted,sizeof(quoted),text))goto finished;
        snprintf(body,sizeof(body),"\"result\":{\"content\":[{\"type\":\"text\",\"text\":%s}],\"isError\":false}",quoted);
        kind=FEISHU_MCP_STATUS;
    } else error=-32601;
finished:
    cJSON_Delete(root);
    if(kind==FEISHU_MCP_IGNORE)return kind;
    if(kind==FEISHU_MCP_ERROR)snprintf(body,sizeof(body),"\"error\":{\"code\":%d,\"message\":\"Unsupported or invalid read-only request\"}",error);
    if(!quote(session_json,sizeof(session_json),session))return FEISHU_MCP_ERROR;
    int n=snprintf(output,capacity,"{\"session_id\":%s,\"type\":\"mcp\",\"payload\":{\"jsonrpc\":\"2.0\",\"id\":%s,%s}}",session_json,id,body);
    if(n<0 || (size_t)n>=capacity){output[0]=0;return FEISHU_MCP_ERROR;}
    return kind;
}

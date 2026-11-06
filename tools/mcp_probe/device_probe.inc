/* Same configured device WSS, ordinary heap config. No audio, sends or UI writes
 * are exposed as tools. Only aggregate state is returned by the one tool. */
#include "feishu_mcp_wire.h"
typedef struct {feishu_mcp_wire_t frame;bool pong;} mcp_wire_t;
static int mcp_wire_read(esp_transport_handle_t ws,mcp_wire_t *r,char *buffer,size_t capacity)
{
    if(!r->frame.frame) {
        int ready=esp_transport_poll_read(ws,100);if(ready<=0)return ready;
    }
    char bytes[256];int n=esp_transport_read(ws,bytes,sizeof(bytes),1000);
    int opcode=esp_transport_ws_get_read_opcode(ws);
    if(n<0)return -1;
    if(!n && opcode==WS_TRANSPORT_OPCODES_NONE)return 0;
    feishu_mcp_wire_event_t event=feishu_mcp_wire_feed(&r->frame,(unsigned)opcode,
        (size_t)esp_transport_ws_get_read_payload_len(ws),esp_transport_ws_get_fin_flag(ws),
        bytes,(size_t)n,buffer,capacity);
    if(event==FEISHU_MCP_WIRE_PING) {
        int length=(int)r->frame.control_used;
        return esp_transport_ws_send_raw(ws,WS_TRANSPORT_OPCODES_PONG|WS_TRANSPORT_OPCODES_FIN,
            (char *)r->frame.control,length,1000)==length ? 0:-1;
    }
    if(event==FEISHU_MCP_WIRE_PONG) {
        ESP_LOGI(TAG,"MCP_CONTROL event=pong bytes=%u",(unsigned)r->frame.control_used);
        r->pong=r->frame.control_used==9 && !memcmp(r->frame.control,"mcp-probe",9);return 0;
    }
    if(event==FEISHU_MCP_WIRE_CLOSE) {
        unsigned code=r->frame.control_used>=2 ? ((unsigned)r->frame.control[0]<<8)|r->frame.control[1]:0;
        ESP_LOGI(TAG,"MCP_WIRE_CLOSE code=%u",code);return -1;
    }
    if(event==FEISHU_MCP_WIRE_ERROR)return -1;
    return event==FEISHU_MCP_WIRE_TEXT ? (int)r->frame.used:0;
}
esp_err_t feishu_asr_mcp_probe(feishu_api_session_t *api,const char *chat,const feishu_mcp_status_t *snapshot)
{
    xiao_config_t *cfg=calloc(1,sizeof(*cfg));
    char *request=calloc(1,FEISHU_MCP_INPUT_MAX+1),*reply=calloc(1,FEISHU_MCP_OUTPUT_MAX);
    feishu_xiao_message_t *envelope=calloc(1,sizeof(*envelope));
    esp_transport_handle_t ssl=NULL,ws=NULL;char session[96]={0};mcp_wire_t wire={0};
    esp_err_t err=ESP_ERR_NO_MEM;bool initialized=false,listed=false,local=false,dual=false;
    unsigned remote_calls=0;const char *stage="allocate";
    if(!cfg || !request || !reply || !envelope)goto done;
    ESP_LOGI(TAG,"MCP_DEVICE_BEGIN heap=%u audio=0 sends=0 tools=1",(unsigned)esp_get_free_heap_size());
    stage="config";err=xiao_config(cfg,NULL,false);if(err!=ESP_OK)goto done;
    stage="connect";err=xiao_ws_open(cfg,&ssl,&ws);if(err!=ESP_OK)goto done;
    /* Otherwise IDF consumes PONG internally and resets its visible opcode. */
    esp_transport_ws_config_t controls={.propagate_control_frames=true};
    err=esp_transport_ws_set_config(ws,&controls);if(err!=ESP_OK)goto done;
    snprintf(reply,FEISHU_MCP_OUTPUT_MAX,"{\"type\":\"hello\",\"version\":%d,\"transport\":\"websocket\",\"features\":{\"mcp\":true},\"audio_params\":{\"format\":\"pcm\",\"sample_rate\":8000,\"channels\":1,\"frame_duration\":20}}",cfg->version);
    if(esp_transport_ws_send_raw(ws,WS_TRANSPORT_OPCODES_TEXT|WS_TRANSPORT_OPCODES_FIN,reply,(int)strlen(reply),1000)!=(int)strlen(reply)){err=ESP_FAIL;goto done;}
    int64_t deadline=esp_timer_get_time()+20000000;
    while(esp_timer_get_time()<deadline && !(initialized&&listed)) {
        stage="negotiate";int n=mcp_wire_read(ws,&wire,request,FEISHU_MCP_INPUT_MAX+1);
        if(n<0){err=ESP_ERR_INVALID_RESPONSE;goto done;}if(!n)continue;
        feishu_xiao_message_init(envelope);
        if(!feishu_xiao_message_feed(envelope,request,(size_t)n)){err=ESP_ERR_INVALID_RESPONSE;goto done;}
        int kind=feishu_xiao_message_finish(envelope);
        if(kind==XIAO_MESSAGE_HELLO) {memcpy(session,envelope->session,sizeof(session));continue;}
        if(!session[0]){err=ESP_ERR_INVALID_RESPONSE;goto done;}
        feishu_mcp_result_t result=feishu_mcp_reply(request,(size_t)n,session,snapshot,reply,FEISHU_MCP_OUTPUT_MAX);
        if(result==FEISHU_MCP_IGNORE)continue;
        if(!reply[0]){err=ESP_ERR_INVALID_RESPONSE;goto done;}
        if(esp_transport_ws_send_raw(ws,WS_TRANSPORT_OPCODES_TEXT|WS_TRANSPORT_OPCODES_FIN,reply,(int)strlen(reply),1000)!=(int)strlen(reply)){err=ESP_FAIL;goto done;}
        initialized|=result==FEISHU_MCP_INITIALIZED;listed|=result==FEISHU_MCP_LISTED;
        remote_calls+=result==FEISHU_MCP_STATUS;
        ESP_LOGI(TAG,"MCP_DEVICE_RPC kind=%u initialized=%u listed=%u heap=%u stack=%u",(unsigned)result,(unsigned)initialized,(unsigned)listed,
            (unsigned)esp_get_free_heap_size(),(unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
    if(!initialized || !listed){err=ESP_ERR_TIMEOUT;goto done;}
    stage="local_status";
    snprintf(request,FEISHU_MCP_INPUT_MAX+1,"{\"type\":\"mcp\",\"session_id\":\"%s\",\"payload\":{\"jsonrpc\":\"2.0\",\"id\":99,\"method\":\"tools/call\",\"params\":{\"name\":\"self.feishu.get_status\",\"arguments\":{}}}}",session);
    local=feishu_mcp_reply(request,strlen(request),session,snapshot,reply,FEISHU_MCP_OUTPUT_MAX)==FEISHU_MCP_STATUS;
    if(!local){err=ESP_ERR_INVALID_RESPONSE;goto done;}
    /* Drop protocol parser/buffers before the second TLS connection. */
    free(envelope);envelope=NULL;
    memset(request,0,FEISHU_MCP_INPUT_MAX+1);free(request);request=NULL;
    memset(reply,0,FEISHU_MCP_OUTPUT_MAX);free(reply);reply=NULL;
    memset(cfg,0,sizeof(*cfg));free(cfg);cfg=NULL;
    stage="dual_tls";
    /* One-message pages bound the dual-TLS workspace; this is not evidence
     * that the ordinary eight-message page fits alongside a persistent WSS. */
    feishu_message_t *items=calloc(1,sizeof(*items));
    if(!items){err=ESP_ERR_NO_MEM;goto done;}
    feishu_page_cursor_t page={0};unsigned before=esp_get_free_heap_size();
    err=feishu_api_message_page(api,chat,"",items,1,&page);
    free(items);
    ESP_LOGI(TAG,"MCP_DUAL_TLS result=%d rows=%u before=%u after=%u minimum=%u stack=%u",err,(unsigned)page.count,before,
        (unsigned)esp_get_free_heap_size(),(unsigned)esp_get_minimum_free_heap_size(),(unsigned)uxTaskGetStackHighWaterMark(NULL));
    if(err!=ESP_OK)goto done;
    char ping[]="mcp-probe";wire.pong=false;
    if(esp_transport_ws_send_raw(ws,WS_TRANSPORT_OPCODES_PING|WS_TRANSPORT_OPCODES_FIN,ping,sizeof(ping)-1,1000)!=(int)sizeof(ping)-1){err=ESP_FAIL;goto done;}
    request=calloc(1,FEISHU_MCP_INPUT_MAX+1);if(!request){err=ESP_ERR_NO_MEM;goto done;}
    deadline=esp_timer_get_time()+3000000;
    while(!wire.pong && esp_timer_get_time()<deadline)if(mcp_wire_read(ws,&wire,request,FEISHU_MCP_INPUT_MAX+1)<0)break;
    dual=wire.pong;err=dual ? ESP_OK:ESP_ERR_TIMEOUT;
done:
    xiao_ws_close(&ws,&ssl);
    if(cfg){memset(cfg,0,sizeof(*cfg));free(cfg);}
    if(request){memset(request,0,FEISHU_MCP_INPUT_MAX+1);free(request);}
    if(reply){memset(reply,0,FEISHU_MCP_OUTPUT_MAX);free(reply);}
    free(envelope);
    ESP_LOGI(TAG,"MCP_DEVICE_RESULT result=%d stage=%s initialized=%u listed=%u local_status=%u dual_tls_alive=%u remote_calls=%u heap=%u minimum=%u",err,stage,
        (unsigned)initialized,(unsigned)listed,(unsigned)local,(unsigned)dual,remote_calls,(unsigned)esp_get_free_heap_size(),(unsigned)esp_get_minimum_free_heap_size());
    return err;
}

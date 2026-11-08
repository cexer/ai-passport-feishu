typedef struct {
    feishu_member_search_t search;
    const volatile bool *cancel;
    unsigned epoch,scope;
    char base[256];
} member_lookup_t;
static uint64_t member_cursor_hash(const char *token)
{
    uint64_t hash=UINT64_C(14695981039346656037);
    for(;*token;++token)hash=(hash^(unsigned char)*token)*UINT64_C(1099511628211);
    return hash;
}
static bool member_lookup_cancelled(void *context)
{
    member_lookup_t *w=context;
    return (w->cancel && *w->cancel) || w->epoch!=atomic_load(&s_read_epoch) ||
        w->scope!=feishu_cache_scope_version();
}
static esp_err_t member_lookup_bytes(void *context,const uint8_t *data,size_t length)
{
    member_lookup_t *w=context;
    if(member_lookup_cancelled(w))return FEISHU_HTTP_ERR_CANCELLED;
    return feishu_member_search_feed(&w->search,data,length) ? ESP_OK:ESP_ERR_INVALID_RESPONSE;
}
esp_err_t feishu_api_resolve_mention(feishu_api_session_t *session,const char *chat,
    const char *name,feishu_mention_t *result,const volatile bool *cancel)
{
    if(!session || !chat || strncmp(chat,"oc_",3) || strlen(chat)>63 || !name || !*name ||
       strlen(name)>=64 || !result)return ESP_ERR_INVALID_ARG;
    for(const char *p=chat;*p;++p)if(!((*p>='a'&&*p<='z') || (*p>='A'&&*p<='Z') ||
        (*p>='0'&&*p<='9') || *p=='_' || *p=='-'))return ESP_ERR_INVALID_ARG;
    if(!strcmp(name,"所有人") || !strcmp(name,"all")){
        memset(result,0,sizeof(*result));strcpy(result->id,"all");strcpy(result->name,"所有人");return ESP_OK;
    }
    member_lookup_t *w=calloc(1,sizeof(*w));if(!w)return ESP_ERR_NO_MEM;
    w->cancel=cancel;w->epoch=atomic_load(&s_read_epoch);w->scope=feishu_cache_scope_version();
    esp_err_t err=ESP_FAIL;
    if(!session->user_access_token[0]){err=refresh_session_token(session);if(err!=ESP_OK)goto done;}
    for(unsigned attempt=0;attempt<2;++attempt){
        bool rejected=false,complete=true;
        if(!feishu_member_search_init(&w->search,name)){err=ESP_ERR_INVALID_ARG;break;}
        for(unsigned bucket=0;bucket<2 && complete && !rejected;++bucket){
            snprintf(w->base,sizeof(w->base),"https://open.feishu.cn/open-apis/im/v1/chats/%s/members%s",chat,
                bucket ? "/bots":"?member_id_type=open_id&page_size=100");
            w->search.token[0]=0;
            for(unsigned page=0;;++page){
                if(member_lookup_cancelled(w)){err=FEISHU_HTTP_ERR_CANCELLED;complete=false;break;}
                if(page>=20){err=ESP_ERR_INVALID_SIZE;complete=false;break;}
                uint64_t previous=member_cursor_hash(w->search.token);
                char *encoded=NULL;const char *url=w->base;
                if(w->search.token[0]){
                    encoded=malloc(1900);if(!encoded){err=ESP_ERR_NO_MEM;complete=false;break;}
                    if(!feishu_page_url(w->base,w->search.token,encoded,1900)){free(encoded);err=ESP_ERR_INVALID_SIZE;complete=false;break;}
                    url=encoded;
                }
                feishu_member_search_page(&w->search);int status=0,api_code=-1;size_t length=0;
                err=feishu_http_get_stream_code(url,session->user_access_token,512u*1024u,
                    member_lookup_bytes,w,member_lookup_cancelled,w,&status,&length,&api_code);
                if(status<200 || status>=300)w->search.code=api_code;
                free(encoded);
                ESP_LOGI(TAG,"MEMBER_PAGE bucket=%u page=%u http=%d code=%d result=%d bytes=%u",bucket,page,status,w->search.code,err,(unsigned)length);
                if(feishu_auth_access_rejected(status,w->search.code)){
                    rejected=true;err=FEISHU_ERR_ACCESS_REJECTED;break;
                }
                if(err!=ESP_OK || status!=200 || !feishu_member_search_finish(&w->search)){
                    if(err==ESP_OK)err=ESP_ERR_INVALID_RESPONSE;
                    complete=false;break;
                }
                if(!w->search.more)break;
                if(bucket || previous==member_cursor_hash(w->search.token)){err=ESP_ERR_INVALID_RESPONSE;complete=false;break;}
            }
        }
        if(rejected && !attempt){err=refresh_session_token(session);if(err==ESP_OK)continue;}
        else if(complete && !rejected)err=feishu_member_search_result(&w->search,result) ? ESP_OK:ESP_ERR_NOT_FOUND;
        break;
    }
done:
    memset(w,0,sizeof(*w));free(w);return err;
}

#include "feishu_registration.h"
#include "feishu_http.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"

#define REGISTER_ENDPOINT "https://accounts.feishu.cn/oauth/v1/app/registration"
static esp_err_t exchange(const char *body,bool beginning,feishu_register_result_t *result,
    feishu_registration_cancel_t cancel,void *context)
{
    feishu_http_response_t response={0};
    esp_err_t err=feishu_http_form_request_cancelable(REGISTER_ENDPOINT,body,4096,&response,cancel,context);
    if(err==ESP_OK) {
        if(response.status_code==429 || response.status_code>=500)err=ESP_FAIL;
        else if(response.status_code!=200 && response.status_code!=400)err=ESP_ERR_INVALID_RESPONSE;
        else if(!feishu_register_decode(response.body,response.length,beginning,result))err=ESP_ERR_INVALID_RESPONSE;
        else if(result->status==FEISHU_REGISTER_COMPLETE && response.status_code!=200)err=ESP_ERR_INVALID_RESPONSE;
    }
    if(beginning || err!=ESP_OK)ESP_LOGI("feishu_registration","begin=%u HTTP=%d bytes=%u result=%s code_bytes=%u",
        (unsigned)beginning,response.status_code,(unsigned)response.length,esp_err_to_name(err),
        err==ESP_OK ? (unsigned)strlen(result->device_code):0u);
    if(response.body)memset(response.body,0,response.length);
    feishu_http_response_free(&response);
    if(err!=ESP_OK) {memset(result,0,sizeof(*result));result->status=FEISHU_REGISTER_FAILED;}
    return err;
}
esp_err_t feishu_registration_begin_cancelable(feishu_register_result_t *request,const char *app,
    feishu_registration_cancel_t cancel,void *context)
{
    if(!request)return ESP_ERR_INVALID_ARG;
    esp_err_t err=exchange("action=begin&archetype=PersonalAgent&auth_method=client_secret&request_user_info=open_id",true,request,cancel,context);
    if(err==ESP_OK && request->status==FEISHU_REGISTER_PENDING && request->device_code[0]) {
        char url[FEISHU_REGISTER_URL_MAX];
        if(!feishu_register_url(url,sizeof(url),request->verification_url,app))err=ESP_ERR_INVALID_RESPONSE;
        else memcpy(request->verification_url,url,strlen(url)+1);
    } else if(err==ESP_OK)err=ESP_ERR_INVALID_RESPONSE;
    if(err!=ESP_OK)memset(request,0,sizeof(*request));
    return err;
}
esp_err_t feishu_registration_begin(feishu_register_result_t *request,const char *app)
{return feishu_registration_begin_cancelable(request,app,NULL,NULL);}
esp_err_t feishu_registration_poll_cancelable(const feishu_register_result_t *request,feishu_register_result_t *result,
    feishu_registration_cancel_t cancel,void *context)
{
    if(!request || !result)return ESP_ERR_INVALID_ARG;
    size_t size=feishu_register_poll_form_size(request->device_code);
    if(!size)return ESP_ERR_INVALID_ARG;
    char *body=malloc(size);if(!body)return ESP_ERR_NO_MEM;
    if(!feishu_register_poll_form(body,size,request->device_code)){free(body);return ESP_ERR_INVALID_ARG;}
    esp_err_t err=exchange(body,false,result,cancel,context);memset(body,0,size);free(body);return err;
}
esp_err_t feishu_registration_poll(const feishu_register_result_t *request,feishu_register_result_t *result)
{return feishu_registration_poll_cancelable(request,result,NULL,NULL);}

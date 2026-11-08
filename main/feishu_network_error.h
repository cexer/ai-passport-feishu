#pragma once
#include <stdbool.h>
#include <stddef.h>
/* ESP-IDF Wi-Fi disconnect reason numbers. No credentials or driver buffers. */
static inline const char *feishu_network_error_hint(bool connected,bool user_offline,unsigned reason)
{
    if(connected)return NULL;
    if(user_offline)return "网络已主动断开，请到网络连接设置中连接";
    switch(reason){
    case 201:return "找不到已保存的Wi-Fi，请检查热点或设置网络";
    case 202:return "Wi-Fi验证失败，请检查网络密码";
    case 15:case 204:return "Wi-Fi握手超时，请检查网络或密码";
    case 210:case 211:return "Wi-Fi安全设置不兼容，请更换网络";
    case 212:return "Wi-Fi信号未达到连接要求，请检查网络";
    default:return "Wi-Fi未连接，正在尝试重连";
    }
}

/* Driver shutdown reports a local disconnect; it must not erase the cause. */
static inline unsigned feishu_network_record_reason(unsigned previous,unsigned reason,bool stopping)
{return stopping ? previous:reason;}

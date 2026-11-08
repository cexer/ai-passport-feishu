#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* The exit latch remains set while the audio/network owner tears down.
 * A repeated shortcut must not launch a second owner during that interval. */
typedef enum {
    FEISHU_ASSISTANT_ACTIVATE,
    FEISHU_ASSISTANT_DEACTIVATE,
    FEISHU_ASSISTANT_WAIT_FOR_STOP
} feishu_assistant_toggle_t;
static inline feishu_assistant_toggle_t feishu_assistant_toggle_action(
    bool active,bool requested,bool exit_requested)
{
    if(exit_requested && active)return FEISHU_ASSISTANT_WAIT_FOR_STOP;
    return active || requested ? FEISHU_ASSISTANT_DEACTIVATE:FEISHU_ASSISTANT_ACTIVATE;
}

typedef struct { unsigned speech_ms,silence_ms; bool heard; } feishu_assistant_vad_t;
/* 20ms PCM frames. A short transient cannot start a command. Only silence
 * AFTER speech submits; idle listening has no artificial 30-second command. */
static inline bool feishu_assistant_vad_frame(feishu_assistant_vad_t *v,
    const int16_t *pcm,size_t count,unsigned seconds)
{
    uint64_t energy=0;
    for(size_t i=0;i<count;++i){int32_t x=pcm[i];energy+=(uint64_t)(x*x);}
    bool voiced=count && energy/count>400u*400u;
    if(voiced){if(v->speech_ms<1000)v->speech_ms+=20;v->silence_ms=0;if(v->speech_ms>=60)v->heard=true;}
    else {if(!v->heard)v->speech_ms=0;else if(v->silence_ms<6000)v->silence_ms+=20;}
    return seconds && v->heard && v->silence_ms>=seconds*1000;
}
static inline bool feishu_assistant_goodbye(const char *text)
{
    if(!text)return false;
    const char *phrases[]={"再见","小智再见","再见小智","拜拜","小智拜拜","退出助手","退出小智助手","结束对话","goodbye","bye"};
    char normalized[64];size_t used=0;
    while(*text){
        unsigned char c=(unsigned char)*text;
        if(c<128){++text;if(c==' ' || c=='\t' || c=='\n' || c=='.' || c==',' || c=='!')continue;
            if(used+1>=sizeof(normalized))return false;
            normalized[used++]=(char)(c>='A' && c<='Z' ? c+32:c);continue;}
        if(!strncmp(text,"。",3) || !strncmp(text,"，",3) || !strncmp(text,"！",3) || !strncmp(text,"、",3)){text+=3;continue;}
        if(used+1>=sizeof(normalized))return false;
        normalized[used++]=*text++;
    }
    normalized[used]=0;
    for(unsigned i=0;i<sizeof(phrases)/sizeof(*phrases);++i)
        if(!strcmp(phrases[i],normalized))return true;
    return false;
}
/* Exact rational 24k->16k conversion, one 20ms frame, no clock reopen.
 * Linear interpolation; adjacent source positions 0,1.5,3,4.5,... */
static inline size_t feishu_assistant_resample(const int16_t *in,size_t n,int16_t *out,size_t cap)
{
    if(!in || !out || n%3 || cap<n/3*2)return 0;
    for(size_t i=0,j=0;i<n;i+=3){out[j++]=in[i];out[j++]=(int16_t)(((int32_t)in[i+1]+in[i+2])/2);}
    return n/3*2;
}

/* Inactivity lifetime: callers renew only for real speech/reply/work, never pings. */
static inline bool feishu_assistant_expired(uint32_t now,uint32_t started,unsigned minutes,bool active)
{return active && minutes && (uint32_t)(now-started)>=minutes*60000u;}
static inline bool feishu_assistant_idle_expired(uint32_t now,uint32_t *activity,unsigned minutes,bool active,bool busy)
{
    if(active && busy)*activity=now;
    return !busy && feishu_assistant_expired(now,*activity,minutes,active);
}
static inline unsigned feishu_assistant_breath(uint32_t now)
{unsigned p=now%2000u;return p<1000u ? p:2000u-p;}
static inline unsigned feishu_assistant_breath_dots(uint32_t now)
{return feishu_assistant_breath(now)*4u/1001u;}
static inline unsigned feishu_assistant_breath_opacity(uint32_t now)
{return 48u+feishu_assistant_breath(now)*207u/1000u;}

/* Only a new user operation may interrupt audio that is actually playing.
 * A target selected by the current MCP reply is not another user command. */
static inline bool feishu_assistant_reply_interrupt(bool playing,bool user_work)
{return playing && user_work;}

/* Progress renews the response inactivity watchdog; a long spoken reply is
 * not itself a timeout. Control pings do not increment received application bytes. */
static inline bool feishu_assistant_reply_stalled(uint32_t now,uint32_t received,uint32_t *seen,uint32_t *progress)
{
    if(received!=*seen){*seen=received;*progress=now;}
    return (uint32_t)(now-*progress)>=90000u;
}


typedef enum {
    FEISHU_ASSISTANT_OFF, FEISHU_ASSISTANT_CONNECTING, FEISHU_ASSISTANT_PREPARING_MIC,
    FEISHU_ASSISTANT_READY, FEISHU_ASSISTANT_PROCESSING, FEISHU_ASSISTANT_SPEAKING,
    FEISHU_ASSISTANT_HANDOFF, FEISHU_ASSISTANT_PAUSED_WORK, FEISHU_ASSISTANT_STOPPING
} feishu_assistant_phase_t;
static inline const char *feishu_assistant_phase_notice(feishu_assistant_phase_t phase)
{
    switch(phase){
    case FEISHU_ASSISTANT_CONNECTING:return "正在连接小智";
    case FEISHU_ASSISTANT_PREPARING_MIC:return "麦克风准备中";
    case FEISHU_ASSISTANT_READY:return "小智聆听中";
    case FEISHU_ASSISTANT_PROCESSING:return "小智正在干活";
    case FEISHU_ASSISTANT_SPEAKING:return "小智正在回复";
    case FEISHU_ASSISTANT_PAUSED_WORK:return "小智暂停聆听";
    case FEISHU_ASSISTANT_HANDOFF:return "小智正在切换";
    case FEISHU_ASSISTANT_STOPPING:return "正在关闭小智";
    default:return "小智未激活";
    }
}
static inline bool feishu_assistant_memory_recovery(bool enabled,bool cancelled,bool closing,bool retried)
{return enabled && !cancelled && !closing && !retried;}

/* Two rapid transport retries; a genuinely stable listening/turn interval may
 * replenish the budget. Caller never retries user cancellation or memory OOM. */
static inline bool feishu_assistant_transport_retry(unsigned *attempts,bool stable)
{
    if(stable)*attempts=0;
    if(*attempts>=2)return false;
    ++*attempts;return true;
}

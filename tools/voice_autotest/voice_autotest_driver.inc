/* Included only by an explicitly enabled diagnostic build, on the normal
 * messenger worker after authentication/list refresh. Never calls send APIs. */
#ifdef FEISHU_VOICE_AUTOTEST_PROVISION
#include "feishu_store.h"
#include <unistd.h>
#include <fcntl.h>
/* Local USB handoff only: credentials never enter generated source or logs.
 * Store through the real mirrored-config writer; do not rebuild the pool. */
bool feishu_asr_autotest_provision(void)
{
    static const char prefix[]="FAP-VOICE/1 ";
    size_t capacity=FEISHU_VOICE_COOKIE_MAX+sizeof(prefix);
    char *line=calloc(1,capacity);
    if(!line)return false;
    int flags=fcntl(STDIN_FILENO,F_GETFL,0);
    if(flags>=0)fcntl(STDIN_FILENO,F_SETFL,flags & ~O_NONBLOCK);
    size_t used=0;bool complete=false,overflow=false;
    TickType_t start=xTaskGetTickCount();
    puts("VOICE_PROVISION_READY");
    while(xTaskGetTickCount()-start<pdMS_TO_TICKS(60000)) {
        char c;ssize_t n=read(STDIN_FILENO,&c,1);
        if(n<=0) {vTaskDelay(pdMS_TO_TICKS(10));continue;}
        if(c=='\r' || c=='\n') {if(used) {complete=true;break;}continue;}
        if(used+1<capacity)line[used++]=c;else overflow=true;
    }
    esp_err_t err=ESP_ERR_INVALID_ARG;
    if(complete && !overflow && used>sizeof(prefix)-1 &&
       !memcmp(line,prefix,sizeof(prefix)-1))
        err=feishu_store_save_voice_cookie(line+sizeof(prefix)-1);
    memset(line,0,capacity);free(line);
    if(flags>=0)fcntl(STDIN_FILENO,F_SETFL,flags);
    ESP_LOGI(TAG,"VOICE_PROVISION_RESULT err=%d elapsed_ms=%u",err,
        (unsigned)((xTaskGetTickCount()-start)*portTICK_PERIOD_MS));
    return err==ESP_OK;
}
#endif
static bool voice_probe_case(feishu_api_session_t *session,unsigned number,const uint8_t *pcm,size_t bytes,
    const char *expected,unsigned stop_ms,unsigned cancel_ms,unsigned fail_ms,unsigned delay_ms,
    bool repeat,esp_err_t expected_error)
{
    volatile bool stop=false,cancel=false;volatile unsigned elapsed=0;
    char recognition[FEISHU_REPLY_MAX]={0};
    memset(&voice_probe,0,sizeof(voice_probe));
    voice_probe.active=true;voice_probe.pcm=pcm;voice_probe.size=bytes;voice_probe.repeat=repeat;
    voice_probe.stop_ms=stop_ms;voice_probe.cancel_ms=cancel_ms;voice_probe.fail_ms=fail_ms;voice_probe.delay_ms=delay_ms;
    voice_probe.stop=&stop;voice_probe.cancel=&cancel;
    unsigned before=esp_get_free_heap_size();
    ESP_LOGI(TAG,"VOICE_PROBE_BEGIN case=%u heap=%u",number,before);
    esp_err_t err=feishu_asr_record(session,&stop,&cancel,&elapsed,recognition,sizeof(recognition),NULL,NULL);
    bool match=err==expected_error;
    bool ram_match=expected ? !strcmp(recognition,expected):true;
    if(expected && err==ESP_OK)match=match && ram_match;
    voice_probe.active=false;
    ESP_LOGI(TAG,"VOICE_MIC_STATS case=%u samples=%u zeros=%u clipped=%u mean=%ld mean_power=%llu",
        number,voice_probe.samples,voice_probe.zeros,voice_probe.clipped,
        (long)(voice_probe.samples ? voice_probe.sum/voice_probe.samples:0),
        (unsigned long long)(voice_probe.samples ? voice_probe.power/voice_probe.samples:0));
    ESP_LOGI(TAG,"VOICE_PROBE_RESULT case=%u pass=%u err=%d expected=%d elapsed=%u reads=%u mic_peak=%d ram_bytes=%u ram_match=%u heap_before=%u heap_after=%u largest=%u",
        number,(unsigned)match,err,expected_error,elapsed,voice_probe.reads,voice_probe.peak,
        (unsigned)strlen(recognition),(unsigned)ram_match,before,(unsigned)esp_get_free_heap_size(),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    return match;
}
void feishu_asr_autotest(feishu_api_session_t *session)
{
#ifdef FEISHU_ASR_XIAOZHI
    volatile bool pair_cancel=true;feishu_asr_pairing_t pairing;
    esp_err_t pair_cancelled=feishu_asr_pairing_check(&pairing,&pair_cancel);
    pair_cancel=false;
    esp_err_t pair_checked=feishu_asr_pairing_check(&pairing,&pair_cancel);
    bool pair_pass=pair_cancelled==FEISHU_HTTP_ERR_CANCELLED && pair_checked==ESP_OK && pairing.supported && pairing.paired;
    ESP_LOGI(TAG,"XIAO_PAIR_CHECK pass=%u cancel=%d result=%d paired=%u token_logged=0",(unsigned)pair_pass,pair_cancelled,pair_checked,(unsigned)pairing.paired);
    if(!pair_pass){ESP_LOGI(TAG,"VOICE_PROBE_SUITE passed=0 total=9 sends=0");return;}
#endif
#if defined(FEISHU_VOICE_AUTOTEST_PROVISION) && !defined(FEISHU_ASR_XIAOZHI)
    if(!feishu_asr_autotest_provision()) {ESP_LOGI(TAG,"VOICE_PROBE_SUITE passed=0 total=9 sends=0");return;}
#endif
    unsigned passed=0,total=0;
#define PROBE(pcm,expected,ms,cancel,fail,delay,repeat,error) \
    do {++total;if(voice_probe_case(session,total,pcm,sizeof(pcm),expected,ms,cancel,fail,delay,repeat,error))++passed;} while(0)
    PROBE(voice_fixture_one,voice_expected_one,4800,0,0,0,false,ESP_OK);
    PROBE(voice_fixture_two,voice_expected_two,5200,0,0,0,false,ESP_OK);
#ifdef FEISHU_ASR_XIAOZHI
    static const uint8_t silence[1920]={0};
    ++total;if(voice_probe_case(session,total,silence,sizeof(silence),NULL,2040,0,0,0,true,ESP_ERR_NOT_FOUND))++passed;
#else
    ++total;if(voice_probe_case(session,total,NULL,0,NULL,2000,0,0,0,false,ESP_OK))++passed;
#endif
    PROBE(voice_fixture_one,NULL,4800,600,0,0,false,FEISHU_HTTP_ERR_CANCELLED);
    PROBE(voice_fixture_one,NULL,4800,0,400,0,false,ESP_FAIL);
    PROBE(voice_fixture_one,voice_expected_one,4800,0,0,1500,false,ESP_OK);
    PROBE(voice_fixture_one,NULL,4800,0,0,5000,false,ESP_ERR_TIMEOUT);
    PROBE(voice_fixture_one,voice_expected_one,4800,0,0,0,false,ESP_OK);
    PROBE(voice_fixture_one,NULL,30000,0,0,0,true,ESP_OK);
#undef PROBE
#ifdef FEISHU_ASR_XIAOZHI
    /* Focus warm-up discards samples; claim skips settling without a prefix loss. */
    esp_err_t warm=feishu_asr_prepare(true);
    for(unsigned i=0;i<300 && !feishu_asr_prepare_ready() && warm==ESP_OK;++i)vTaskDelay(pdMS_TO_TICKS(10));
    bool ready=feishu_asr_prepare_ready();
    bool warm_pass=ready && voice_probe_case(session,10,voice_fixture_one,sizeof(voice_fixture_one),voice_expected_one,4800,0,0,0,false,ESP_OK);
    feishu_asr_prepare_cancel();(void)feishu_asr_prepare(false);
    warm=feishu_asr_prepare(true);vTaskDelay(pdMS_TO_TICKS(200));
    feishu_asr_prepare_cancel();(void)feishu_asr_prepare(false);
    bool cancelled=warm==ESP_OK && !feishu_asr_prepare_ready();
    bool recovered=voice_probe_case(session,11,voice_fixture_one,sizeof(voice_fixture_one),voice_expected_one,4800,0,0,0,false,ESP_OK);
    ESP_LOGI(TAG,"AUDIO_PREWARM_SUITE pass=%u ready=%u cancelled=%u recovered=%u",(unsigned)(warm_pass&&cancelled&&recovered),(unsigned)ready,(unsigned)cancelled,(unsigned)recovered);
#endif
    ESP_LOGI(TAG,"VOICE_PROBE_SUITE passed=%u total=%u sends=0",passed,total);
}
void feishu_asr_autotest_prepare(volatile bool *stop,volatile bool *cancel)
{
    memset(&voice_probe,0,sizeof(voice_probe));voice_probe.active=true;
    voice_probe.pcm=voice_fixture_one;voice_probe.size=sizeof(voice_fixture_one);
    voice_probe.stop_ms=4800;voice_probe.stop=stop;voice_probe.cancel=cancel;
}
void feishu_asr_autotest_finish(void) {voice_probe.active=false;}

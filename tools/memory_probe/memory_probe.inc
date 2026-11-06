/* Explicit diagnostic only. No audio upload, playback or message sends. */
#include "bsp_audio.h"
#include "esp_heap_trace.h"
#include "lwip/tcpip.h"
#include "lwip/priv/tcp_priv.h"

typedef struct {unsigned active,waiting,bound;size_t waiting_bytes;} memory_tcp_t;
static void memory_tcp_count(void *argument)
{
    memory_tcp_t *s=argument;
    for(struct tcp_pcb *p=tcp_active_pcbs;p;p=p->next)++s->active;
    for(struct tcp_pcb *p=tcp_tw_pcbs;p;p=p->next){++s->waiting;s->waiting_bytes+=heap_caps_get_allocated_size(p);}
    for(struct tcp_pcb *p=tcp_bound_pcbs;p;p=p->next)++s->bound;
}
static void memory_tcp_sample(unsigned phase,unsigned cycle)
{
    memory_tcp_t s={0};err_t result=tcpip_callback_wait(memory_tcp_count,&s);
    ESP_LOGI(TAG,"MEMORY_TCP phase=%u cycle=%u result=%d active=%u wait=%u bound=%u pcb_size=%u wait_allocated=%u free=%u",phase,cycle,result,
        s.active,s.waiting,s.bound,(unsigned)sizeof(struct tcp_pcb),(unsigned)s.waiting_bytes,(unsigned)esp_get_free_heap_size());
}

static void memory_trace_end(unsigned phase)
{
#if CONFIG_HEAP_TRACING_STANDALONE
    heap_trace_stop();heap_trace_summary_t summary={0};heap_trace_summary(&summary);
    ESP_LOGI(TAG,"MEMTRACE phase=%u count=%u capacity=%u high=%u overflow=%u allocs=%u frees=%u",phase,
        (unsigned)summary.count,(unsigned)summary.capacity,(unsigned)summary.high_water_mark,
        (unsigned)summary.has_overflowed,(unsigned)summary.total_allocations,(unsigned)summary.total_frees);
    for(size_t i=0;i<summary.count;++i) {
        heap_trace_record_t record={0};if(heap_trace_get(i,&record)!=ESP_OK)break;
        void *pc0=NULL,*pc1=NULL,*pc2=NULL,*pc3=NULL;
#if CONFIG_HEAP_TRACING_STACK_DEPTH >= 4
        pc0=record.alloced_by[0];pc1=record.alloced_by[1];pc2=record.alloced_by[2];pc3=record.alloced_by[3];
#endif
        ESP_LOGI(TAG,"MEMTRACE_RECORD phase=%u bytes=%u pc0=%p pc1=%p pc2=%p pc3=%p",phase,(unsigned)record.size,
            pc0,pc1,pc2,pc3);
    }
#else
    (void)phase;
#endif
}

static void memory_probe(void)
{
    enum { CYCLES=20 };
    size_t chat=0;unsigned passed=0,total=0;bool integrity=true;
    (void)feishu_asr_prepare(false);
    xSemaphoreTake(s_state_lock,portMAX_DELAY);
    bool available=s_state.nav.chat_count>0;
    s_state.nav.page=FEISHU_PAGE_STATUS;s_state.loading=false;
    feishu_utf8_copy(s_state.status,sizeof(s_state.status),"自动资源检查，保持静音");
    xSemaphoreGive(s_state_lock);
    if(!available || reserve_audio_resources()!=ESP_OK) {
        ESP_LOGI(TAG,"MEMORY_PROBE_DONE pass=0 unavailable=1");return;
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG,"MEMORY_PROBE_BEGIN phases=4 cycles=%u sends=0 playback=0 heap=%u",CYCLES,(unsigned)esp_get_free_heap_size());
    for(unsigned phase=0;phase<4;++phase) {
        heap_trace_record_t *trace=NULL;
#if CONFIG_HEAP_TRACING_STANDALONE
        if(phase<2) {
            trace=calloc(96,sizeof(*trace));
            if(trace && heap_trace_init_standalone(trace,96)==ESP_OK)heap_trace_start(HEAP_TRACE_LEAKS);
        }
#endif
        unsigned baseline=esp_get_free_heap_size();
        for(unsigned cycle=0;cycle<CYCLES;++cycle) {
            esp_err_t err=ESP_OK;bool ready=true;
            if(phase==0) {
                int16_t pcm[320];err=bsp_audio_init();
                if(err==ESP_OK){bsp_audio_set_volume(0);err=bsp_audio_set_format(16000,16,1);}
                if(err==ESP_OK)err=bsp_audio_read(pcm,sizeof(pcm));
                memset(pcm,0,sizeof(pcm));bsp_audio_deinit();
            } else {
                if(phase==1 || phase==3) {
                    err=feishu_asr_prepare(true);
                    if(phase==1)vTaskDelay(pdMS_TO_TICKS(300));
                    else {
                        for(unsigned i=0;i<300 && !feishu_asr_prepare_ready() && err==ESP_OK;++i)vTaskDelay(pdMS_TO_TICKS(10));
                        ready=feishu_asr_prepare_ready();
                    }
                }
                if(err==ESP_OK && phase>=2)err=load_messages_for_chat(chat,false,false);
                feishu_asr_prepare_cancel();(void)feishu_asr_prepare(false);
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            bool valid=heap_caps_check_integrity_all(false);
            integrity=integrity&&valid;
            bool pass=err==ESP_OK && ready && valid && !feishu_asr_prepare_ready();
            ++total;passed+=pass;
            ESP_LOGI(TAG,"MEMORY_PROBE phase=%u cycle=%u pass=%u err=%d ready=%u free=%u delta=%ld largest=%u tasks=%u minimum=%u",phase,cycle,
                (unsigned)pass,err,(unsigned)ready,(unsigned)esp_get_free_heap_size(),
                (long)esp_get_free_heap_size()-(long)baseline,
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
                (unsigned)uxTaskGetNumberOfTasks(),(unsigned)esp_get_minimum_free_heap_size());
            memory_tcp_sample(phase,cycle);
            if(!valid || esp_get_free_heap_size()<60000)break;
        }
        if(trace) {
            memory_trace_end(phase);
#if CONFIG_HEAP_TRACING_STANDALONE
            heap_trace_init_standalone(NULL,0);
#endif
            free(trace);
        }
        if(!integrity || esp_get_free_heap_size()<60000)break;
    }
    /* No background refresh or audio owner runs during this bounded quiet
     * window. Observe delayed TCP teardown without rebooting the device. */
    for(unsigned i=0;i<=26;++i) {
        memory_tcp_sample(4,i);
        if(i<26)vTaskDelay(pdMS_TO_TICKS(5000));
    }
    release_audio_resources();
    ESP_LOGI(TAG,"MEMORY_PROBE_DONE pass=%u passed=%u total=%u expected=%u integrity=%u heap=%u",(unsigned)(passed==4*CYCLES && integrity),passed,total,4*CYCLES,(unsigned)integrity,(unsigned)esp_get_free_heap_size());
}

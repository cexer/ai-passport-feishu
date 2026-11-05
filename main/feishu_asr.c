#include "feishu_asr.h"
#include "feishu_asr_batch.h"
#include "feishu_adpcm.h"
#include "feishu_http.h"

#include "bsp_audio.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "feishu_asr";

#define ASR_SAMPLE_RATE 16000
#define ASR_CHUNK_MS 100
#define ASR_CHUNK_SAMPLES (ASR_SAMPLE_RATE * ASR_CHUNK_MS / 1000)
#define ASR_BUFFER_COUNT 32
#define ASR_MIN_BUFFERS 8
#define ASR_PACKET_BLOCKS 6
#define ASR_MIN_MS 400
#define ASR_MAX_MS 30000
#define ASR_CAPTURE_STACK 4096
/* The real microphone lost the first phrase when used immediately after
 * codec open; a two-second pre-read restored it in the physical comparison.
 * Keep this bounded settling window in PREPARING, not in user recording time. */
#define ASR_PRIME_MS 2000
#define ASR_RUNTIME_RESERVE (32u*1024u)
#ifdef FEISHU_VOICE_AUTOTEST
#include "voice_autotest_config.h"
#endif

#define ASR_SENTINEL_DONE (-1)
#define ASR_SENTINEL_ERROR (-2)

typedef struct {
    uint8_t *encoded[ASR_BUFFER_COUNT];
    int16_t *capture_pcm,*upload_pcm;
    feishu_adpcm_t encoder;
    unsigned buffer_count;
    unsigned queue_high;
    QueueHandle_t free_queue;
    QueueHandle_t ready_queue;
    SemaphoreHandle_t capture_done;
    TaskHandle_t capture_task;
    volatile bool *stop_requested;
    volatile unsigned *elapsed_ms;
    volatile bool *preparing;
    volatile esp_err_t capture_error;
    int upload_first;
    bool stream_done;
} asr_context_t;

static int capture_next(void *argument,const void **data)
{
    asr_context_t *context=argument;
    int index=context->upload_first;context->upload_first=-1;
    if(index<0 && xQueueReceive(context->ready_queue,&index,pdMS_TO_TICKS(500))!=pdTRUE) {
        context->capture_error=ESP_ERR_TIMEOUT;return ASR_SENTINEL_ERROR;
    }
    if(index>=0 && index<(int)context->buffer_count) {
        if(!feishu_adpcm_decode(context->encoded[index],FEISHU_ADPCM_BYTES,ASR_CHUNK_SAMPLES,
            context->upload_pcm,ASR_CHUNK_SAMPLES)) {
            xQueueSend(context->free_queue,&index,0);context->capture_error=ESP_ERR_INVALID_RESPONSE;
            return ASR_SENTINEL_ERROR;
        }
        *data=context->upload_pcm;
    }
    else if(index>=0){context->capture_error=ESP_ERR_INVALID_RESPONSE;return ASR_SENTINEL_ERROR;}
    return index;
}
static void capture_release(void *argument,int index)
{asr_context_t *context=argument;xQueueSend(context->free_queue,&index,0);}
static bool upload_capture(void *argument,feishu_asr_write_t write,void *writer)
{
    asr_context_t *context=argument;
#ifdef FEISHU_VOICE_AUTOTEST
    voice_probe_upload_delay();
#endif
    return feishu_asr_batch(ASR_PACKET_BLOCKS,ASR_CHUNK_SAMPLES*sizeof(int16_t),context->upload_pcm,
        capture_next,capture_release,context,write,writer,&context->stream_done);
}

static void capture_task(void *argument)
{
    asr_context_t *context = argument;
    int index;

    context->capture_error = ESP_OK;
    for (;;) {
        bool minimum_met = *context->elapsed_ms >= ASR_MIN_MS;
        if ((*context->stop_requested && minimum_met) ||
            *context->elapsed_ms >= ASR_MAX_MS) {
            break;
        }
        /* Do not silently lose I2S samples when the network stops consuming.
         * The hardware DMA holds about 90 ms; fail within 50 ms of starvation. */
        if (xQueueReceive(context->free_queue, &index, pdMS_TO_TICKS(50)) != pdTRUE) {
            context->capture_error = ESP_ERR_TIMEOUT;
            break;
        }
        esp_err_t err = bsp_audio_read(context->capture_pcm,
                                       ASR_CHUNK_SAMPLES*sizeof(int16_t));
#ifdef FEISHU_VOICE_AUTOTEST
        if(err==ESP_OK)err=voice_probe_capture(context->capture_pcm,ASR_CHUNK_SAMPLES*sizeof(int16_t),*context->elapsed_ms);
#endif
        if(err==ESP_OK && feishu_adpcm_encode(&context->encoder,context->capture_pcm,ASR_CHUNK_SAMPLES,
            context->encoded[index],FEISHU_ADPCM_BYTES)!=FEISHU_ADPCM_BYTES)err=ESP_ERR_INVALID_RESPONSE;
        if (err != ESP_OK) {
            xQueueSend(context->free_queue, &index, portMAX_DELAY);
            context->capture_error = err;
            break;
        }
        /* The first retained frame is already captured before UI readiness. */
        if(context->preparing && *context->preparing) {
            ESP_LOGI(TAG,"ASR_READY prime_ms=%u retained_ms=%u",ASR_PRIME_MS,ASR_CHUNK_MS);
            *context->preparing=false;
        }
        *context->elapsed_ms += ASR_CHUNK_MS;
        if (xQueueSend(context->ready_queue, &index, portMAX_DELAY) != pdTRUE) {
            context->capture_error = ESP_FAIL;
            break;
        }
        unsigned queued=uxQueueMessagesWaiting(context->ready_queue);
        if(queued>context->queue_high)context->queue_high=queued;
    }
    index = context->capture_error == ESP_OK ? ASR_SENTINEL_DONE :
                                               ASR_SENTINEL_ERROR;
    xQueueSend(context->ready_queue, &index, portMAX_DELAY);
    context->capture_task = NULL;
    xSemaphoreGive(context->capture_done);
    vTaskDelete(NULL);
}

static void make_stream_id(char output[17])
{
    static const char alphabet[] = "0123456789abcdefghijklmnopqrstuvwxyz";

    for (size_t i = 0; i < 16; ++i) {
        output[i] = alphabet[esp_random() % (sizeof(alphabet) - 1)];
    }
    output[16] = '\0';
}

static void context_destroy(asr_context_t *context)
{
    if (context == NULL) return;
    if (context->capture_done != NULL) vSemaphoreDelete(context->capture_done);
    if (context->free_queue != NULL) vQueueDelete(context->free_queue);
    if (context->ready_queue != NULL) vQueueDelete(context->ready_queue);
    for(unsigned i=0;i<ASR_BUFFER_COUNT;++i) {
        if(context->encoded[i]){memset(context->encoded[i],0,FEISHU_ADPCM_BYTES);free(context->encoded[i]);}
    }
    if(context->capture_pcm){memset(context->capture_pcm,0,ASR_CHUNK_SAMPLES*sizeof(int16_t));free(context->capture_pcm);}
    if(context->upload_pcm){memset(context->upload_pcm,0,ASR_CHUNK_SAMPLES*sizeof(int16_t));free(context->upload_pcm);}
    memset(context, 0, sizeof(*context));
    free(context);
}

esp_err_t feishu_asr_record(feishu_api_session_t *session,
                            volatile bool *stop_requested,
                            volatile bool *cancel_requested,
                            volatile unsigned *elapsed_ms,
                            char *recognition, size_t recognition_size,
                            feishu_data_writer_t *draft,volatile bool *preparing)
{
    asr_context_t *context=NULL;
    char stream_id[17];
    char context_id[37];
    uint32_t sequence = 1;
    uint64_t last_length=0;
    char *cookie=NULL;
    size_t cookie_capacity=FEISHU_VOICE_COOKIE_MAX;
    int index;
    esp_err_t err = ESP_OK;
    bool recording_ended=false,audio_released=false;

    if (session == NULL || stop_requested == NULL || elapsed_ms == NULL ||
        recognition == NULL || recognition_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    recognition[0] = '\0';
    ESP_LOGI(TAG,"ASR_MEMORY stage=start heap=%u minimum=%u",
        (unsigned)esp_get_free_heap_size(),(unsigned)esp_get_minimum_free_heap_size());
    if(preparing)*preparing=true;
    *elapsed_ms = 0;
    cookie=calloc(1,FEISHU_VOICE_COOKIE_MAX);
    if(!cookie){err=ESP_ERR_NO_MEM;goto finished;}
    err=feishu_store_load_voice_cookie(cookie,FEISHU_VOICE_COOKIE_MAX);
    if(err!=ESP_OK){err=FEISHU_ERR_VOICE_LOGIN;goto finished;}
    size_t cookie_size=strlen(cookie)+1;
    char *compact=realloc(cookie,cookie_size);
    if(compact){cookie=compact;cookie_capacity=cookie_size;}
    make_stream_id(stream_id);
    snprintf(context_id,sizeof(context_id),"%08lx-%04lx-4%03lx-8%03lx-%08lx%04lx",
        (unsigned long)esp_random(),(unsigned long)(esp_random()&65535),
        (unsigned long)(esp_random()&4095),(unsigned long)(esp_random()&4095),
        (unsigned long)esp_random(),(unsigned long)(esp_random()&65535));
    feishu_api_asr_stream_close();
    /* Establish TLS and BEGIN before enabling the microphone. Empty BEGIN has
     * been verified against the live gateway with both cumulative and END data. */
    err=feishu_api_internal_asr_packet(cookie,stream_id,context_id,sequence++,1,NULL,0,
        recognition,recognition_size,&last_length,NULL,cancel_requested,NULL);
    if(err!=ESP_OK)goto finished;
    ESP_LOGI(TAG,"ASR_MEMORY stage=begin_done heap=%u minimum=%u",
        (unsigned)esp_get_free_heap_size(),(unsigned)esp_get_minimum_free_heap_size());
    err = bsp_audio_init();
    if (err != ESP_OK) goto finished;
    bsp_audio_set_volume(0);
    err = bsp_audio_set_format(ASR_SAMPLE_RATE, 16, 1);
    if (err != ESP_OK) goto finished;

    context = calloc(1, sizeof(*context));
    if (context == NULL) {err=ESP_ERR_NO_MEM;goto finished;}
    context->capture_pcm=calloc(ASR_CHUNK_SAMPLES,sizeof(int16_t));
    context->upload_pcm=calloc(ASR_CHUNK_SAMPLES,sizeof(int16_t));
    if(!context->capture_pcm || !context->upload_pcm){err=ESP_ERR_NO_MEM;goto finished;}
    for(unsigned i=0;i<ASR_BUFFER_COUNT;++i) {
        /* TLS receive buffers can appear after BEGIN. Budget the capture
         * stack and 32 KiB runtime headroom before spending spare RAM on
         * burst tolerance; an oversized queue must not starve networking. */
        if(i>=ASR_MIN_BUFFERS && esp_get_free_heap_size()<FEISHU_ADPCM_BYTES+ASR_CAPTURE_STACK+ASR_RUNTIME_RESERVE)break;
        context->encoded[i]=calloc(1,FEISHU_ADPCM_BYTES);
        if(!context->encoded[i]){if(i>=ASR_MIN_BUFFERS)break;err=ESP_ERR_NO_MEM;goto finished;}
        ++context->buffer_count;
    }
    /* Drain real codec/DMA samples while still preparing; reuse the existing
     * PCM buffer, without storing or uploading this settling interval. */
    for(unsigned primed=0;primed<ASR_PRIME_MS;primed+=ASR_CHUNK_MS) {
        if((cancel_requested && *cancel_requested) || *stop_requested) {
            err=FEISHU_HTTP_ERR_CANCELLED;goto finished;
        }
        err=bsp_audio_read(context->capture_pcm,ASR_CHUNK_SAMPLES*sizeof(int16_t));
        if(err!=ESP_OK)goto finished;
    }
    if((cancel_requested && *cancel_requested) || *stop_requested) {
        err=FEISHU_HTTP_ERR_CANCELLED;goto finished;
    }
    context->preparing=preparing;
    context->stop_requested = stop_requested;
    context->elapsed_ms = elapsed_ms;
    context->free_queue = xQueueCreate(ASR_BUFFER_COUNT, sizeof(int));
    context->ready_queue = xQueueCreate(ASR_BUFFER_COUNT + 1, sizeof(int));
    context->capture_done = xSemaphoreCreateBinary();
    if (context->free_queue == NULL || context->ready_queue == NULL ||
        context->capture_done == NULL) {
        err=ESP_ERR_NO_MEM;goto finished;
    }
    for (index = 0; index < (int)context->buffer_count; ++index) {
        xQueueSend(context->free_queue, &index, 0);
    }
    ESP_LOGI(TAG,"ASR_CAPTURE codec=ima4 frames=%u capacity_ms=%u audio_ram=%u heap=%u largest=%u",context->buffer_count,
        context->buffer_count*ASR_CHUNK_MS,
        (unsigned)(context->buffer_count*FEISHU_ADPCM_BYTES+2*ASR_CHUNK_SAMPLES*sizeof(int16_t)),(unsigned)esp_get_free_heap_size(),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    if (xTaskCreate(capture_task, "feishu_capture", ASR_CAPTURE_STACK,
                    context, 6, &context->capture_task) != pdPASS) {
        err=ESP_ERR_NO_MEM;goto finished;
    }

    for (;;) {
        if (xQueueReceive(context->ready_queue, &index, portMAX_DELAY) != pdTRUE) {
            err = ESP_FAIL;
            break;
        }
        if (index == ASR_SENTINEL_ERROR) {
            err = context->capture_error;
            break;
        }
        if (index == ASR_SENTINEL_DONE) {
            recording_ended=true;
            break;
        }
        context->upload_first=index;
        err=feishu_api_internal_asr_upload(cookie,stream_id,context_id,sequence++,0,
            ASR_PACKET_BLOCKS*ASR_CHUNK_SAMPLES*sizeof(int16_t),upload_capture,context,
            recognition,recognition_size,&last_length,NULL,cancel_requested,&context->capture_error);
        if(context->upload_first>=0) {
            xQueueSend(context->free_queue,&context->upload_first,0);context->upload_first=-1;
        }
        if(err!=ESP_OK){if(context->capture_error!=ESP_OK)err=context->capture_error;break;}
        if(context->stream_done) {
            recording_ended=true;
            break;
        }
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "stream failed at sequence %lu: %s",
                 (unsigned long)sequence, esp_err_to_name(err));
        *stop_requested = true;
    }
    while (context->capture_task != NULL) {
        if (xQueueReceive(context->ready_queue, &index,
                          pdMS_TO_TICKS(250)) == pdTRUE && index >= 0) {
            xQueueSend(context->free_queue, &index, portMAX_DELAY);
        }
    }
    xSemaphoreTake(context->capture_done, pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG,"ASR_QUEUE high_water=%u ms=%u",context->queue_high,context->queue_high*ASR_CHUNK_MS);
    /* The END response can allocate a persistent writer. Capture is already
     * joined, so release audio/codec/DMA resources before that storage peak.
     * Keep only the scalar error; no callback may reference the freed context. */
    esp_err_t capture_error=context->capture_error;
    unsigned before_release=esp_get_free_heap_size();
    context_destroy(context);context=NULL;
    bsp_audio_deinit();audio_released=true;
    ESP_LOGI(TAG,"ASR_FINALIZE before=%u after=%u minimum=%u",before_release,
        (unsigned)esp_get_free_heap_size(),(unsigned)esp_get_minimum_free_heap_size());
    if(err==ESP_OK && recording_ended)
        err=feishu_api_internal_asr_packet(cookie,stream_id,context_id,sequence++,2,NULL,0,
            recognition,recognition_size,&last_length,draft,cancel_requested,&capture_error);
finished:
    if(preparing)*preparing=false;
    context_destroy(context);
    if(!audio_released)bsp_audio_deinit();
    feishu_api_asr_stream_close();
    if(cookie){memset(cookie,0,cookie_capacity);free(cookie);}
    memset(stream_id, 0, sizeof(stream_id));
    ESP_LOGI(TAG,"ASR_MEMORY stage=finished result=%d heap=%u minimum=%u",err,
        (unsigned)esp_get_free_heap_size(),(unsigned)esp_get_minimum_free_heap_size());
    return err;
}
#ifdef FEISHU_VOICE_AUTOTEST
#include "voice_autotest_driver.inc"
#endif

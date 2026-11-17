/**
 * ---------------------------------------------------------------------------
 *
 * Project : TanRadio
 * Module  : radio
 * File    : radio.c
 *
 * Purpose :
 * TanRadio internet radio subsystem.
 *
 * ---------------------------------------------------------------------------
 */

/* Standard library */
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

/* ESP-IDF */
#include "esp_http_client.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_aac_dec.h"
#include "impl/esp_ts_dec.h"
#include "esp_log.h"
#include <stdlib.h>
#include "esp_heap_caps.h"
#include "bsp/device.h"
#include "network/network.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/queue.h"
#include <strings.h>
#include "freertos/task.h"

/* Local header */
#include "radio.h"
#include "audio/audio.h"
#include "audio/pcm.h"
#include "radio/stations.h"
#include "radio/browser.h"
#include "radio/hls_stream.h"
#include "freertos/semphr.h"
#include "storage/sdcard.h"

static const char *TAG = "tanradio.radio";

/* ---------------------------------------------------------------------------
 * Private State
 * ---------------------------------------------------------------------------
 */

static size_t radio_selected_station = 0;
static size_t radio_playing_station = 0;
static SemaphoreHandle_t radio_station_mutex;
/* Serializes the PCM consumer with format changes and stream resets. */
static SemaphoreHandle_t radio_output_mutex;
static bool radio_output_ready;
typedef enum {
    RADIO_ERROR_NONE,
    RADIO_ERROR_PCM,
    RADIO_ERROR_OUTPUT,
    RADIO_ERROR_FORMAT,
    RADIO_ERROR_UNAVAILABLE,
    RADIO_ERROR_TLS,
    RADIO_ERROR_HLS,
    RADIO_ERROR_DECODE,
} radio_error_t;

static atomic_int radio_audio_error;
static radio_station_t radio_requested_station;
static radio_station_t radio_current_station;

#define RADIO_STREAM_BUFFER_SIZE 16384
#define RADIO_COMPRESSED_BUFFER_SIZE (256 * 1024)
#define RADIO_MAX_DECODE_BYTES (256 * 1024)
#define RADIO_PROBE_MS 3000
#define RADIO_FIRST_PCM_LIMIT (512 * 1024)
#define RADIO_PCM_STREAM_BUFFER_SIZE (256 * 1024)
#define RADIO_PCM_LOW_WATERMARK (32 * 1024)
#define RADIO_PCM_HIGH_WATERMARK (160 * 1024)
#define RADIO_AUDIO_WRITE_CHUNK_SIZE 8192

static TaskHandle_t radio_task_handle = NULL;
static TaskHandle_t radio_audio_task_handle = NULL;
static StreamBufferHandle_t radio_pcm_stream = NULL;
static StaticStreamBuffer_t radio_pcm_stream_storage;
static uint8_t *radio_pcm_stream_memory = NULL;
static StreamBufferHandle_t radio_compressed_stream = NULL;
static StaticStreamBuffer_t radio_compressed_stream_storage;
static uint8_t *radio_compressed_stream_memory = NULL;

static atomic_bool radio_exit_requested = false;
static atomic_bool radio_switch_requested = false;
/* Playback is opt-in: wait for an explicit radio_play() request at startup. */
static atomic_bool radio_stop_requested = true;
static atomic_bool radio_resume_requested = false;
static atomic_bool radio_muted = false;
static atomic_bool radio_buffering = false;
static atomic_bool radio_input_ended = false;

typedef struct {
    uint64_t position;
    uint32_t connection;
    char title[RADIO_TITLE_SIZE];
} radio_title_marker_t;

typedef struct {
    esp_http_client_handle_t client;
    uint8_t *buffer;
    icy_filter_t *icy;
    uint32_t connection;
    char last_title[RADIO_TITLE_SIZE];
    atomic_bool stop_requested;
    atomic_bool finished;
    atomic_bool playing_hls;
    hls_stream_t *hls;
    hls_stream_result_t hls_result;
    stream_format_t mime;
    int result;
} radio_download_context_t;

typedef struct {
    radio_metadata_t metadata;
    uint32_t interval;
    stream_format_t mime;
    bool interval_seen, interval_invalid;
} radio_stream_headers_t;

/* The station mutex also protects the metadata snapshot and its connection. */
static radio_metadata_t radio_metadata;
static uint32_t radio_connection;
static uint32_t radio_metadata_revision;
static QueueHandle_t radio_wire_titles, radio_pcm_titles;
/* PCM output mutex protects the consumer position and output connection. */
static uint64_t radio_pcm_position;
static uint32_t radio_pcm_connection;

static void radio_reset_metadata_locked(void)
{
    ++radio_connection;
    ++radio_metadata_revision;
    radio_metadata = (radio_metadata_t){0};
}

static esp_err_t radio_stream_header(esp_http_client_event_t *event)
{
    radio_stream_headers_t *headers = event->user_data;
    /* Redirects supply a new set of headers. */
    if (event->event_id == HTTP_EVENT_HEADERS_SENT) *headers = (radio_stream_headers_t){0};
    if (event->event_id != HTTP_EVENT_ON_HEADER) return ESP_OK;
    const char *key = event->header_key, *value = event->header_value;
    if (!strcasecmp(key, "Content-Type")) {
        headers->mime = stream_format_from_mime(value);
    } else if (!strcasecmp(key, "icy-metaint")) {
        uint32_t interval;
        if (!icy_parse_uint(value, &interval) ||
            (headers->interval_seen && headers->interval != interval)) {
            headers->interval_invalid = true;
        } else {
            headers->interval = interval;
            headers->interval_seen = true;
        }
        return ESP_OK;
    }
    if (!strcasecmp(key, "icy-br")) {
        uint32_t rate;
        if (icy_parse_uint(value, &rate) && rate <= 10000)
            headers->metadata.bitrate_kbps = rate;
    } else {
        char *field = NULL;
        size_t capacity = 0;
        if (!strcasecmp(key, "icy-name")) {
            field = headers->metadata.name;
            capacity = sizeof(headers->metadata.name);
        } else if (!strcasecmp(key, "icy-genre")) {
            field = headers->metadata.genre;
            capacity = sizeof(headers->metadata.genre);
        } else if (!strcasecmp(key, "icy-description")) {
            field = headers->metadata.description;
            capacity = sizeof(headers->metadata.description);
        }
        if (field) icy_copy_text(field, capacity, (const uint8_t *)value, strlen(value));
    }
    return ESP_OK;
}

static void radio_set_buffering(bool buffering)
{
    if (radio_buffering == buffering) {
        return;
    }

    radio_buffering = buffering;
    if (buffering) {
        ESP_LOGI(TAG, "Playback buffering; outputting silence");
    } else {
        ESP_LOGI(TAG, "Playback buffers ready; resuming audio");
    }
}

static bool radio_stream_interrupted(void)
{
    return radio_exit_requested || radio_switch_requested ||
           radio_stop_requested || radio_resume_requested;
}

static bool radio_connection_active(void *unused)
{
    (void)unused;
    return !radio_stream_interrupted();
}

static bool radio_download_active(void *arg)
{
    const radio_download_context_t *context = arg;
    return !atomic_load(&context->stop_requested) && !radio_stream_interrupted();
}

static bool radio_download_audio(void *arg, const uint8_t *data, size_t size)
{
    radio_download_context_t *context = arg;
    size_t sent = 0;
    while (sent < size && radio_download_active(context)) {
        sent += xStreamBufferSend(radio_compressed_stream, data + sent,
                                  size - sent, pdMS_TO_TICKS(100));
    }
    return sent == size;
}

static void radio_download_title(void *arg, uint64_t position, const char *title)
{
    radio_download_context_t *context = arg;
    if (!radio_wire_titles || !strcmp(title, context->last_title)) return;
    radio_title_marker_t marker = {.position = position, .connection = context->connection};
    memcpy(marker.title, title, strlen(title) + 1);
    if (xQueueSend(radio_wire_titles, &marker, 0) == pdTRUE)
        memcpy(context->last_title, marker.title, sizeof(marker.title));
}

/** Remove ICY framing before the compressed buffer sees the stream. */
static void radio_download_task(void *arg)
{
    radio_download_context_t *context = arg;
    context->result = -1;
    size_t prefix = 0;
    bool first = true;
    while (radio_download_active(context)) {
        int count = esp_http_client_read(context->client, (char *)context->buffer + prefix,
                                         STREAM_HTTP_CHUNK - prefix);
        if (count <= 0) {
            context->result = count;
            break;
        }
        size_t received = prefix + (size_t)count;
        if (first && received < 8 && !esp_http_client_is_complete_data_received(context->client)) {
            prefix = received;
            continue;
        }
        if (first && !context->icy) {
            stream_format_t format = stream_format_probe(context->buffer, received);
            if (format == STREAM_PLAYLIST || (format == STREAM_UNKNOWN && context->mime == STREAM_PLAYLIST)) {
                context->hls = hls_stream_create();
                if (context->hls) {
                    atomic_store(&context->playing_hls, true);
                    context->hls_result = hls_stream_run(context->hls, context->client,
                        context->buffer, received, radio_download_active, radio_download_audio, context);
                }
                break;
            }
        }
        prefix = 0;
        first = false;
        if (!icy_filter_feed(context->icy, context->buffer, received,
                             radio_download_audio, radio_download_title, context)) break;
    }
    atomic_store(&context->finished, true);
    vTaskDelete(NULL);
}

static bool radio_take_title(QueueHandle_t queue, uint32_t connection,
                             uint64_t position, radio_title_marker_t *latest)
{
    if (!queue) return false;
    bool changed = false;
    radio_title_marker_t marker;
    while (xQueuePeek(queue, &marker, 0) == pdTRUE &&
           (marker.connection != connection || marker.position <= position)) {
        xQueueReceive(queue, &marker, 0);
        if (marker.connection != connection) continue;
        *latest = marker;
        changed = true;
    }
    return changed;
}

static void radio_publish_pcm_title(uint64_t last_position)
{
    radio_title_marker_t marker;
    if (!radio_take_title(radio_pcm_titles, radio_pcm_connection, last_position, &marker)) return;
    xSemaphoreTake(radio_station_mutex, portMAX_DELAY);
    if (marker.connection == radio_connection && !radio_stop_requested &&
        !radio_exit_requested && strcmp(radio_metadata.title, marker.title)) {
        memcpy(radio_metadata.title, marker.title, sizeof(marker.title));
        ++radio_metadata_revision;
    }
    xSemaphoreGive(radio_station_mutex);
}

/* ---------------------------------------------------------------------------
 * Private Functions
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Continuously write queued PCM to I2S.
 *
 * Keeping I2S writes out of the HTTP/decoder task lets the radio task absorb
 * network jitter and continue fetching compressed data while audio hardware
 * consumes the previous PCM frames.
 */
static void radio_audio_step(uint8_t *pcm_chunk)
{
    if (radio_stream_interrupted()) {
        vTaskDelay(pdMS_TO_TICKS(20));
        return;
    }

    if (!radio_output_ready) {
        memset(pcm_chunk, 0, RADIO_AUDIO_WRITE_CHUNK_SIZE);
        (void)audio_write_pcm(pcm_chunk, RADIO_AUDIO_WRITE_CHUNK_SIZE);
        return;
    }
    size_t available = xStreamBufferBytesAvailable(radio_pcm_stream);

    bool input_ended = radio_input_ended;
    if (input_ended && !available) {
        memset(pcm_chunk, 0, RADIO_AUDIO_WRITE_CHUNK_SIZE);
        audio_write_pcm(pcm_chunk, RADIO_AUDIO_WRITE_CHUNK_SIZE);
        return;
    }
    if (radio_buffering) {
        if (!input_ended && available < RADIO_PCM_HIGH_WATERMARK) {
            memset(pcm_chunk, 0, RADIO_AUDIO_WRITE_CHUNK_SIZE);
            audio_write_pcm(pcm_chunk, RADIO_AUDIO_WRITE_CHUNK_SIZE);
            return;
        }

        radio_set_buffering(false);
        if (!radio_muted) {
            audio_unmute();
        }
    } else if (!input_ended && available < RADIO_PCM_LOW_WATERMARK) {
        radio_set_buffering(true);
        audio_mute();
        memset(pcm_chunk, 0, RADIO_AUDIO_WRITE_CHUNK_SIZE);
        audio_write_pcm(pcm_chunk, RADIO_AUDIO_WRITE_CHUNK_SIZE);
        return;
    }

    size_t received = xStreamBufferReceive(
        radio_pcm_stream,
        pcm_chunk,
        RADIO_AUDIO_WRITE_CHUNK_SIZE,
        pdMS_TO_TICKS(100));

    if (received == 0) {
        radio_set_buffering(true);
        audio_mute();
        memset(pcm_chunk, 0, RADIO_AUDIO_WRITE_CHUNK_SIZE);
        audio_write_pcm(pcm_chunk, RADIO_AUDIO_WRITE_CHUNK_SIZE);
        return;
    }

    esp_err_t res = audio_write_pcm(pcm_chunk, received);
    radio_pcm_position += received;
    if (res == ESP_OK) radio_publish_pcm_title(radio_pcm_position - 1);

    if (res != ESP_OK) {
        ESP_LOGE(TAG,
                 "PCM audio output failed: %s",
                 esp_err_to_name(res));
    }
}

static void radio_audio_task(void *arg)
{
    (void)arg;

    uint8_t *pcm_chunk = heap_caps_malloc(
        RADIO_AUDIO_WRITE_CHUNK_SIZE,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (pcm_chunk == NULL) {
        ESP_LOGE(TAG, "Failed to allocate PCM output chunk");
        radio_audio_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    memset(pcm_chunk, 0, RADIO_AUDIO_WRITE_CHUNK_SIZE);

    while (true) {
        xSemaphoreTake(radio_output_mutex, portMAX_DELAY);
        radio_audio_step(pcm_chunk);
        xSemaphoreGive(radio_output_mutex);
        /* Allow a decoder waiting to change format/reset the queue to run. */
        vTaskDelay(1);
    }
}

/**
 * @brief Queue PCM for the dedicated I2S writer task.
 */
static bool radio_queue_stereo(const uint8_t *data, size_t size)
{
    size_t queued = 0;

    while (queued < size) {
        if (radio_stream_interrupted()) {
            return false;
        }

        size_t sent = xStreamBufferSend(
            radio_pcm_stream,
            data + queued,
            size - queued,
            pdMS_TO_TICKS(100));

        if (sent == 0) {
            continue;
        }

        queued += sent;
    }

    return true;
}

static bool radio_queue_pcm(const uint8_t *data, size_t size, unsigned channels)
{
    if (channels == 2) return size % 4 == 0 && radio_queue_stereo(data, size);
    if (channels != 1 || size % 2) return false;
    int16_t stereo[512];
    while (size) {
        size_t bytes = size < 512 ? size : 512;
        size_t frames = audio_pcm_stereo(data, bytes, channels, stereo, 256);
        if (!frames || !radio_queue_stereo((const uint8_t *)stereo, frames * 4)) return false;
        data += bytes;
        size -= bytes;
    }
    return true;
}

static bool radio_queue_resampled(const uint8_t *data, size_t size, unsigned channels,
                                  audio_pcm_resampler_t *resampler, uint64_t *position)
{
    const size_t stride = channels * sizeof(int16_t);
    if (!stride || size % stride) return false;
    int16_t stereo[64], converted[64 * 6];
    while (size) {
        size_t bytes = size < 32 * stride ? size : 32 * stride;
        size_t frames = audio_pcm_stereo(data, bytes, channels, stereo, 32);
        if (!frames) return false;
        size_t output = audio_pcm_resample(resampler, stereo, frames, converted, 32 * 6) *
                        2 * sizeof(int16_t);
        if (!output || !radio_queue_stereo((const uint8_t *)converted, output)) return false;
        *position += output;
        data += bytes;
        size -= bytes;
    }
    return true;
}

static void radio_discard_pcm(void)
{
    if (radio_output_mutex) xSemaphoreTake(radio_output_mutex, portMAX_DELAY);
    radio_output_ready = false;
    radio_input_ended = false;
    (void)audio_mute();
    if (radio_pcm_stream) xStreamBufferReset(radio_pcm_stream);
    if (radio_pcm_titles) xQueueReset(radio_pcm_titles);
    radio_pcm_position = 0;
    if (radio_output_mutex) xSemaphoreGive(radio_output_mutex);
}

static esp_err_t radio_prepare_output(uint32_t rate, uint32_t connection)
{
    xSemaphoreTake(radio_output_mutex, portMAX_DELAY);
    radio_output_ready = false;
    radio_set_buffering(true);
    xStreamBufferReset(radio_pcm_stream);
    if (radio_pcm_titles) xQueueReset(radio_pcm_titles);
    radio_pcm_position = 0;
    radio_pcm_connection = connection;
    esp_err_t res = audio_set_sample_rate(rate);
    radio_output_ready = res == ESP_OK;
    xSemaphoreGive(radio_output_mutex);
    return res;
}

static void radio_finish_stream(uint32_t connection, radio_error_t error)
{
    xSemaphoreTake(radio_station_mutex, portMAX_DELAY);
    if (connection == radio_connection && !radio_stream_interrupted()) {
        atomic_store(&radio_audio_error, error);
        radio_stop_requested = true;
        radio_set_buffering(false);
    }
    xSemaphoreGive(radio_station_mutex);
}

static radio_error_t radio_hls_error(const radio_download_context_t *context)
{
    switch (context->hls_result) {
        case HLS_STREAM_UNSUPPORTED: return RADIO_ERROR_HLS;
        case HLS_STREAM_UNAVAILABLE: return RADIO_ERROR_UNAVAILABLE;
        case HLS_STREAM_TLS: return RADIO_ERROR_TLS;
        default: return RADIO_ERROR_NONE;
    }
}

static esp_audio_simple_dec_type_t radio_decoder_type(stream_format_t format)
{
    switch (format) {
        case STREAM_MP3: return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
        case STREAM_AAC: return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
        case STREAM_OPUS:
        case STREAM_OGG:
        case STREAM_VORBIS: return ESP_AUDIO_SIMPLE_DEC_TYPE_OGG;
        case STREAM_FLAC: return ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
        case STREAM_TS: return ESP_AUDIO_SIMPLE_DEC_TYPE_TS;
        default: return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
    }
}

static stream_format_t radio_probe_stream(radio_download_context_t *context, uint8_t *input, size_t *size)
{
    TickType_t first_byte = 0;
    while (*size < RADIO_STREAM_BUFFER_SIZE && !radio_stream_interrupted()) {
        size_t received = xStreamBufferReceive(radio_compressed_stream, input + *size,
            RADIO_STREAM_BUFFER_SIZE - *size, pdMS_TO_TICKS(250));
        if (received) {
            if (!*size) first_byte = xTaskGetTickCount();
            *size += received;
            stream_format_t format = stream_format_probe(input, *size);
            if (format != STREAM_UNKNOWN) return format;
        }
        if ((atomic_load(&context->finished) && !xStreamBufferBytesAvailable(radio_compressed_stream)) ||
            (*size && xTaskGetTickCount() - first_byte >= pdMS_TO_TICKS(RADIO_PROBE_MS))) break;
    }
    return STREAM_UNKNOWN;
}

static void radio_drain_output(void)
{
    radio_input_ended = true;
    TickType_t start = xTaskGetTickCount();
    while (!radio_stream_interrupted() && xStreamBufferBytesAvailable(radio_pcm_stream) &&
           xTaskGetTickCount() - start < pdMS_TO_TICKS(2000)) vTaskDelay(pdMS_TO_TICKS(20));
    /* Give the DMA buffer time to emit the final PCM after the queue empties. */
    for (unsigned i = 0; i < 10 && !radio_stream_interrupted(); i++) vTaskDelay(pdMS_TO_TICKS(20));
}

/** Own the HTTP connection, decoder and the compressed-to-PCM timeline. */
static void radio_stream_task(void *arg)
{
    (void)arg;

    size_t station_index = radio_playing_station;
    radio_station_t stream_station;
    bool plain_retry = false;
    unsigned retry_ms = 1000;

    while (true) {
        /*
         * F3 stops the current online stream but keeps the radio task
         * alive. The task waits here until F4 clears the stop request.
         */
        if (radio_stop_requested && !radio_exit_requested) {
            retry_ms = 1000;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (radio_exit_requested) {
            ESP_LOGI(TAG,
                     "Radio exit request received");
            break;
        }

        /* Startup no longer waits for saved-network retries. Keep an early
         * Play request buffered and responsive to Stop/Exit until Wi-Fi is up. */
        if (!network_is_connected()) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (radio_switch_requested) {
            retry_ms = 1000;
            radio_set_buffering(true);
        }

        if (radio_resume_requested) {
            radio_resume_requested = false;

            /* Selection can change while stopped, including before first play. */
            station_index = radio_playing_station;

            radio_set_buffering(true);

            ESP_LOGI(
                TAG,
                "Radio playback resume requested; audio unmute will be scheduled after fresh PCM");
        }

        /* HTTP owns this value until the client is destroyed. Neither a new
         * search, a save, nor another selection can mutate its URL storage. */
        xSemaphoreTake(radio_station_mutex, portMAX_DELAY);
        stream_station = radio_requested_station;
        station_index = radio_selected_station;
        radio_playing_station = station_index;
        radio_current_station = stream_station;
        radio_reset_metadata_locked();
        uint32_t connection = radio_connection;
        radio_switch_requested = false;
        xSemaphoreGive(radio_station_mutex);
        const radio_station_t *station = &stream_station;
        if (station->url[0] == '\0') {
            ESP_LOGE(TAG, "Selected station no longer exists: %zu", station_index);
            radio_stop_requested = true;
            continue;
        }

        ESP_LOGI(TAG,
                 "Radio playback started: %s", station->name);
        if (radio_exit_requested) {
            ESP_LOGI(TAG,
                     "Radio exit request received");
            break;
        }

        esp_http_client_handle_t client = NULL;
        esp_audio_simple_dec_handle_t decoder = NULL;
        radio_download_context_t download_context = {.connection = connection};
        radio_stream_headers_t headers = {0};
        bool request_icy = !plain_retry && radio_wire_titles && radio_pcm_titles;
        plain_retry = false;
        TaskHandle_t download_task_handle = NULL;

        uint8_t *buffer = NULL;
        uint8_t *pcm_buffer = NULL;
        size_t pcm_buffer_size = 8192;

        bool audio_decoder_registered = false;
        bool simple_decoder_registered = false;

        stream_http_result_t opened = stream_http_open(&client, station->url, radio_stream_header,
                                                         &headers, request_icy, radio_connection_active, NULL);
        if (opened != STREAM_HTTP_OK) {
            if (opened == STREAM_HTTP_UNAVAILABLE) radio_finish_stream(connection, RADIO_ERROR_UNAVAILABLE);
            if (opened == STREAM_HTTP_TLS) radio_finish_stream(connection, RADIO_ERROR_TLS);
            goto stream_cleanup;
        }

        if (headers.interval_invalid) {
            ESP_LOGW(TAG, "Invalid ICY interval%s", request_icy ? ". Retrying without metadata." : "");
            plain_retry = request_icy;
            goto stream_cleanup;
        }
        download_context.mime = headers.mime;
        if (headers.interval) {
            download_context.icy = heap_caps_malloc(sizeof(*download_context.icy),
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!download_context.icy) {
                plain_retry = request_icy;
                goto stream_cleanup;
            }
            icy_filter_init(download_context.icy, headers.interval);
        }
        xSemaphoreTake(radio_station_mutex, portMAX_DELAY);
        if (connection == radio_connection && !radio_stop_requested && !radio_exit_requested) {
            radio_metadata = headers.metadata;
            ++radio_metadata_revision;
        }
        xSemaphoreGive(radio_station_mutex);

        buffer = heap_caps_malloc(
            RADIO_STREAM_BUFFER_SIZE,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

        pcm_buffer = heap_caps_malloc(
            pcm_buffer_size,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

        download_context.buffer = heap_caps_malloc(RADIO_STREAM_BUFFER_SIZE,
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buffer || !pcm_buffer || !download_context.buffer) {
            ESP_LOGE(TAG,
                     "Failed to allocate radio audio buffers");
            goto stream_cleanup;
        }

        size_t total_bytes = 0;
        size_t total_pcm_bytes = 0;
        size_t decoded_frames = 0;
        uint64_t decoded_position = 0, queued_pcm_position = 0;
        radio_title_marker_t next_title = {0};
        bool have_title = false, title_pending = false;
        esp_audio_simple_dec_info_t output_format = {0};

        TickType_t last_log_time = xTaskGetTickCount();

        xStreamBufferReset(radio_compressed_stream);
        if (radio_wire_titles) xQueueReset(radio_wire_titles);
        download_context.client = client;

        BaseType_t download_task_res = xTaskCreate(
            radio_download_task,
            "tanradio_net",
            8192,
            &download_context,
            5,
            &download_task_handle);

        if (download_task_res != pdPASS) {
            ESP_LOGE(TAG, "Failed to create radio download task");
            goto stream_cleanup;
        }

        radio_set_buffering(true);
        audio_mute();

        size_t bytes_read = 0;
        stream_format_t format = radio_probe_stream(&download_context, buffer, &bytes_read);
        if (radio_stream_interrupted()) goto stream_cleanup;
        if (atomic_load(&download_context.finished)) {
            radio_error_t error = radio_hls_error(&download_context);
            if (error) {
                radio_finish_stream(connection, error);
                goto stream_cleanup;
            }
        }
        if (!bytes_read) goto stream_cleanup;
        if (format == STREAM_UNKNOWN && !atomic_load(&download_context.playing_hls)) {
            format = headers.mime;
            if (format == STREAM_UNKNOWN) format = stream_format_from_codec(station->codec);
        }
        esp_audio_simple_dec_type_t decoder_type = radio_decoder_type(format);
        if (decoder_type == ESP_AUDIO_SIMPLE_DEC_TYPE_NONE) {
            radio_finish_stream(connection, format == STREAM_PLAYLIST ? RADIO_ERROR_HLS : RADIO_ERROR_FORMAT);
            goto stream_cleanup;
        }
        const char *codec_name = stream_format_name(format);
        bool hls = atomic_load(&download_context.playing_hls);
        xSemaphoreTake(radio_station_mutex, portMAX_DELAY);
        if (connection == radio_connection) {
            snprintf(radio_metadata.format, sizeof(radio_metadata.format), "%s%s",
                     hls && format != STREAM_TS ? "HLS " : "", codec_name);
            ++radio_metadata_revision;
        }
        xSemaphoreGive(radio_station_mutex);

        esp_aac_dec_cfg_t aac_cfg = ESP_AAC_DEC_CONFIG_DEFAULT();
        aac_cfg.aac_plus_enable = true;
        esp_ts_dec_cfg_t ts_cfg = {.aac_plus_enable = true};
        esp_audio_simple_dec_cfg_t decoder_cfg = {.dec_type = decoder_type};
        if (format == STREAM_AAC) {
            decoder_cfg.dec_cfg = &aac_cfg;
            decoder_cfg.cfg_size = sizeof(aac_cfg);
        } else if (format == STREAM_TS) {
            decoder_cfg.dec_cfg = &ts_cfg;
            decoder_cfg.cfg_size = sizeof(ts_cfg);
        }

        esp_audio_err_t audio_res = esp_audio_dec_register_default();
        if (audio_res != ESP_AUDIO_ERR_OK) goto stream_cleanup;
        audio_decoder_registered = true;
        audio_res = esp_audio_simple_dec_register_default();
        if (audio_res != ESP_AUDIO_ERR_OK) goto stream_cleanup;
        simple_decoder_registered = true;
        audio_res = esp_audio_simple_dec_open(&decoder_cfg, &decoder);
        if (audio_res != ESP_AUDIO_ERR_OK || !decoder) {
            ESP_LOGE(TAG, "%s decoder open failed: %d", codec_name, audio_res);
            if (audio_res != ESP_AUDIO_ERR_MEM_LACK) radio_finish_stream(connection, RADIO_ERROR_DECODE);
            goto stream_cleanup;
        }

        audio_pcm_resampler_t resampler = {0};
        while (true) {
            if (!bytes_read) {
                bytes_read = xStreamBufferReceive(radio_compressed_stream, buffer,
                    RADIO_STREAM_BUFFER_SIZE, pdMS_TO_TICKS(250));
            }
            if (bytes_read > 0) {
                if (radio_exit_requested) {
                    ESP_LOGI(TAG,
                             "Radio exit request received during playback");
                    goto stream_cleanup;
                }

                if (radio_switch_requested) {
                    ESP_LOGI(
                        TAG,
                        "Radio station switch requested during playback");
                    goto stream_cleanup;
                }

                if (radio_stop_requested || radio_resume_requested) {
                    ESP_LOGI(
                        TAG,
                        "Radio playback stop requested during playback");
                    goto stream_cleanup;
                }

                total_bytes += bytes_read;

                esp_audio_simple_dec_raw_t raw = {
                    .buffer = buffer,
                    .len = (uint32_t)bytes_read,
                    .eos = atomic_load(&download_context.finished) &&
                           !xStreamBufferBytesAvailable(radio_compressed_stream),
                    .consumed = 0,
                    .frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE,
                };

                unsigned buffered_frames = 0;
                while (raw.len > 0 && !radio_stream_interrupted()) {
                    esp_audio_simple_dec_out_t output = {
                        .buffer = pcm_buffer,
                        .len = pcm_buffer_size,
                        .needed_size = 0,
                        .decoded_size = 0,
                    };

                    audio_res = esp_audio_simple_dec_process(
                        decoder,
                        &raw,
                        &output);

                    if (audio_res == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                        if (output.needed_size <= pcm_buffer_size || output.needed_size > RADIO_MAX_DECODE_BYTES) {
                            ESP_LOGE(TAG,
                                     "%s decoder reported invalid required output size: %lu",
                                     codec_name, (unsigned long)output.needed_size);
                            radio_finish_stream(connection, RADIO_ERROR_DECODE);
                            goto stream_cleanup;
                        }

                        uint8_t *new_buffer =
                            heap_caps_realloc(pcm_buffer, output.needed_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

                        if (new_buffer == NULL) {
                            ESP_LOGE(TAG,
                                     "Failed to resize PCM output buffer to %lu bytes",
                                     (unsigned long)output.needed_size);
                            goto stream_cleanup;
                        }

                        pcm_buffer = new_buffer;
                        pcm_buffer_size = output.needed_size;

                        ESP_LOGI(TAG,
                                 "PCM output buffer resized to %lu bytes",
                                 (unsigned long)pcm_buffer_size);

                        continue;
                    }

                    if (audio_res != ESP_AUDIO_ERR_OK || raw.consumed > raw.len) {
                        ESP_LOGE(TAG,
                                 "%s decode failed: %d", codec_name, audio_res);
                        if (!decoded_frames) radio_finish_stream(connection, RADIO_ERROR_DECODE);
                        goto stream_cleanup;
                    }

                    decoded_position += raw.consumed;
                    if (decoded_position &&
                        radio_take_title(radio_wire_titles, connection, decoded_position - 1, &next_title)) {
                        have_title = true;
                        title_pending = true;
                    }
                    if (output.decoded_size > 0) {
                        esp_audio_simple_dec_info_t info = {0};
                        audio_res = esp_audio_simple_dec_get_info(decoder, &info);
                        uint32_t rate = audio_pcm_output_rate(info.sample_rate, info.bits_per_sample, info.channel);
                        if (audio_res != ESP_AUDIO_ERR_OK || !rate) {
                            ESP_LOGE(TAG, "Unsupported PCM: %lu Hz, %u bit, %u channels",
                                     (unsigned long)info.sample_rate, info.bits_per_sample, info.channel);
                            radio_finish_stream(connection, RADIO_ERROR_PCM);
                            goto stream_cleanup;
                        }
                        if (!decoded_frames || info.sample_rate != output_format.sample_rate ||
                            info.bits_per_sample != output_format.bits_per_sample ||
                            info.channel != output_format.channel) {
                            esp_err_t format_res = radio_prepare_output(rate, connection);
                            if (format_res != ESP_OK) {
                                ESP_LOGE(TAG, "PCM configuration failed: %s", esp_err_to_name(format_res));
                                radio_finish_stream(connection, RADIO_ERROR_OUTPUT);
                                goto stream_cleanup;
                            }
                            queued_pcm_position = 0;
                            title_pending = have_title;
                            output_format = info;
                            resampler = (audio_pcm_resampler_t){.input_rate = info.sample_rate};
                            ESP_LOGI(TAG, "%s audio: %lu Hz to %lu Hz, %u bit, %u channel, stack margin %u",
                                     codec_name, (unsigned long)info.sample_rate, (unsigned long)rate,
                                     info.bits_per_sample, info.channel,
                                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
                        }
                        if (title_pending && radio_pcm_titles) {
                            next_title.position = queued_pcm_position;
                            title_pending = xQueueSend(radio_pcm_titles, &next_title, 0) != pdTRUE;
                        }
                        total_pcm_bytes += output.decoded_size;
                        if (rate == info.sample_rate) {
                            if (!radio_queue_pcm(output.buffer, output.decoded_size, info.channel))
                                goto stream_cleanup;
                            queued_pcm_position += output.decoded_size * (info.channel == 1 ? 2 : 1);
                        } else if (!radio_queue_resampled(output.buffer, output.decoded_size,
                                                          info.channel, &resampler, &queued_pcm_position)) {
                            goto stream_cleanup;
                        }
                        decoded_frames++;
                        retry_ms = 1000;
                    }

                    if (raw.consumed == 0) {
                        if (output.decoded_size && ++buffered_frames < 64) continue;
                        ESP_LOGW(TAG, "%s decoder stalled", codec_name);
                        if (!decoded_frames) radio_finish_stream(connection, RADIO_ERROR_DECODE);
                        goto stream_cleanup;
                    }

                    buffered_frames = 0;
                    raw.len -= raw.consumed;
                    raw.buffer += raw.consumed;
                    raw.consumed = 0;
                }

                if (radio_stream_interrupted()) goto stream_cleanup;
                if (!decoded_frames && total_bytes >= RADIO_FIRST_PCM_LIMIT) {
                    radio_finish_stream(connection, RADIO_ERROR_DECODE);
                    goto stream_cleanup;
                }

                TickType_t now = xTaskGetTickCount();

                if ((now - last_log_time) >=
                    pdMS_TO_TICKS(10000)) {

                    ESP_LOGI(TAG,
                             "Radio playback: %u frames, %u PCM bytes, %u input bytes",
                             (unsigned int)decoded_frames,
                             (unsigned int)total_pcm_bytes,
                             (unsigned int)total_bytes);

                    last_log_time = now;
                }

                bytes_read = 0;
                continue;
            }

            if (radio_stream_interrupted()) {
                goto stream_cleanup;
            }

            if (atomic_load(&download_context.finished) &&
                xStreamBufferBytesAvailable(radio_compressed_stream) == 0) {
                radio_error_t error = radio_hls_error(&download_context);
                if (error) {
                    radio_finish_stream(connection, error);
                } else if (download_context.hls_result == HLS_STREAM_COMPLETE) {
                    radio_drain_output();
                    radio_finish_stream(connection, RADIO_ERROR_NONE);
                } else if (download_context.result == 0) {
                    ESP_LOGW(TAG, "Radio stream closed by server");
                } else {
                    ESP_LOGE(TAG,
                             "Radio stream read failed: %d",
                             download_context.result);
                }
                break;
            }
        }

stream_cleanup:
        if (download_task_handle != NULL && !atomic_load(&download_context.finished)) {
            atomic_store(&download_context.stop_requested, true);

            TickType_t download_stop_deadline =
                xTaskGetTickCount() + pdMS_TO_TICKS(3500);

            while (!atomic_load(&download_context.finished) &&
                   (int32_t)(xTaskGetTickCount() -
                             download_stop_deadline) < 0) {
                vTaskDelay(pdMS_TO_TICKS(20));
            }

            if (!atomic_load(&download_context.finished)) {
                ESP_LOGW(TAG, "Forcing stalled download task to stop");
                vTaskDelete(download_task_handle);
            }
        }

        free(download_context.buffer);
        free(download_context.icy);
        hls_stream_destroy(download_context.hls);

        xSemaphoreTake(radio_station_mutex, portMAX_DELAY);
        if (connection == radio_connection) radio_reset_metadata_locked();
        xSemaphoreGive(radio_station_mutex);

        if (radio_compressed_stream != NULL) {
            xStreamBufferReset(radio_compressed_stream);
        }
        if (radio_pcm_stream != NULL) {
            radio_discard_pcm();
        }

        if (decoder != NULL) {
            esp_audio_simple_dec_close(decoder);
            decoder = NULL;
        }

        if (simple_decoder_registered) {
            esp_audio_simple_dec_unregister_default();
            simple_decoder_registered = false;
        }

        if (audio_decoder_registered) {
            esp_audio_dec_unregister_default();
            audio_decoder_registered = false;
        }

        if (buffer != NULL) {
            free(buffer);
            buffer = NULL;
        }

        if (pcm_buffer != NULL) {
            free(pcm_buffer);
            pcm_buffer = NULL;
        }

        stream_http_close(&client);

        if (radio_exit_requested) {
            ESP_LOGI(TAG,
                     "Radio playback cleanup complete");
            break;
        }

        if (radio_switch_requested || radio_resume_requested) {
            plain_retry = false;
            retry_ms = 1000;
            radio_set_buffering(true);
            continue;
        }

        /*
         * F3 requested a real stream stop. Do not reconnect until
         * F4 clears radio_stop_requested.
         */
        if (radio_stop_requested) {
            ESP_LOGI(
                TAG,
                "Radio playback stopped; waiting for play request");

            continue;
        }
        if (plain_retry) continue;

        if (radio_exit_requested) {
            ESP_LOGI(TAG,
                     "Radio exit requested; skipping reconnect");
            break;
        }

        radio_set_buffering(true);
        audio_mute();

        ESP_LOGW(TAG,
                 "Radio playback reconnecting in %u ms", retry_ms);
        for (unsigned waited = 0; waited < retry_ms && !radio_stream_interrupted(); waited += 100)
            vTaskDelay(pdMS_TO_TICKS(100));
        if (retry_ms < 16000) retry_ms *= 2;
    }

    radio_task_handle = NULL;

    /*
     * Do not stop the ESP-Hosted Wi-Fi driver before restarting into the
     * launcher.  On Tanmatsu the Wi-Fi radio is a separate coprocessor and a
     * CPU-only restart does not reset all of its state.  Stopping the remote
     * driver here can therefore leave the launcher with an apparently
     * connected interface whose TLS traffic is unusable until a power cycle.
     *
     * The stream and HTTP client have already been closed above.  Hand off
     * directly to the launcher, which owns initialization of its networking
     * stack after the restart.
     */
    ESP_LOGI(TAG,
             "Radio task stopped; returning to launcher");

    audio_shutdown();
    bsp_device_restart_to_launcher();

    vTaskDelete(NULL);
}

/* ---------------------------------------------------------------------------
 * Public Functions
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Return the number of configured stations.
 */
size_t radio_station_count(void)
{
    return stations_count();
}

/**
 * @brief Return the name of a station.
 *
 * @param index Station index.
 * @return Station name, or NULL if index is invalid.
 */
const char *radio_station_get_name(size_t index)
{
    const radio_station_t *station = stations_get(index);
    return station == NULL ? NULL : station->name;
}

/**
 * @brief Return the URL of a station.
 *
 * @param index Station index.
 * @return Station URL, or NULL if index is invalid.
 */
const char *radio_station_get_url(size_t index)
{
    const radio_station_t *station = stations_get(index);
    return station == NULL ? NULL : station->url;
}

const char *radio_station_get_country(size_t index)
{
    const radio_station_t *station = stations_get(index);
    return station == NULL || station->country[0] == '\0'
               ? NULL
               : station->country;
}

uint16_t radio_station_get_bitrate_kbps(size_t index)
{
    const radio_station_t *station = stations_get(index);
    return station == NULL ? 0 : station->bitrate_kbps;
}

/**
 * @brief Return the currently selected station index.
 */
size_t radio_station_get_selected(void)
{
    return radio_selected_station;
}

void radio_apply_catalog_removal(size_t index)
{
    if (!radio_station_mutex) {
        stations_publish_removal(index);
        return;
    }
    xSemaphoreTake(radio_station_mutex, portMAX_DELAY);
    stations_publish_removal(index);
    radio_selected_station = SIZE_MAX;
    radio_playing_station = SIZE_MAX;
    stations_find(&radio_requested_station, &radio_selected_station);
    stations_find(&radio_current_station, &radio_playing_station);
    xSemaphoreGive(radio_station_mutex);
}

/**
 * @brief Return the currently playing station index.
 */
size_t radio_station_get_playing(void)
{
    return radio_playing_station;
}

/**
 * @brief Select a station.
 *
 * @param index Station index.
 * @return true if selected, false if index is invalid.
 */
bool radio_station_select(size_t index)
{
    if (!radio_station_mutex) return false;
    xSemaphoreTake(radio_station_mutex, portMAX_DELAY);
    const radio_station_t *saved = stations_get(index);
    if (!saved) {
        xSemaphoreGive(radio_station_mutex);
        return false;
    }
    radio_station_t station = *saved;
    bool changed = !stations_same(&radio_requested_station, &station);
    radio_requested_station = station;
    radio_selected_station = index;
    bool switching = radio_task_handle != NULL && !radio_stop_requested && changed;
    if (switching) {
        radio_switch_requested = true;
        radio_reset_metadata_locked();
        radio_current_station = station;
        radio_playing_station = index;
    }
    xSemaphoreGive(radio_station_mutex);

    if (switching) {
        radio_browser_count_play(station.stationuuid);
        radio_set_buffering(true);

        esp_err_t mute_res = audio_mute();
        if (mute_res != ESP_OK) {
            ESP_LOGW(
                TAG,
                "Failed to mute audio during station switch: %s",
                esp_err_to_name(mute_res));
        }

        ESP_LOGI(
            TAG,
            "Radio station switch requested: %zu (%s)",
            index,
            station.name);
    }

    return true;
}

bool radio_get_playing_station(radio_station_t *station)
{
    if (!radio_station_mutex || !station) return false;
    xSemaphoreTake(radio_station_mutex, portMAX_DELAY);
    *station = radio_current_station;
    xSemaphoreGive(radio_station_mutex);
    return station->url[0] != '\0';
}

bool radio_play_preview(const radio_station_t *station)
{
    return radio_play_record(station);
}

static void radio_copy_playback_locked(radio_playback_status_t *status)
{
    status->station = radio_current_station;
    status->stopped = radio_stop_requested;
    status->muted = radio_muted;
    status->buffering = radio_buffering;
}

void radio_get_playback_status(radio_playback_status_t *status)
{
    *status = (radio_playback_status_t){.stopped = true};
    if (!radio_station_mutex) return;
    xSemaphoreTake(radio_station_mutex, portMAX_DELAY);
    radio_copy_playback_locked(status);
    xSemaphoreGive(radio_station_mutex);
}

void radio_get_now_playing(radio_now_playing_t *snapshot)
{
    *snapshot = (radio_now_playing_t){.playback.stopped = true};
    if (!radio_station_mutex) return;
    xSemaphoreTake(radio_station_mutex, portMAX_DELAY);
    radio_copy_playback_locked(&snapshot->playback);
    snapshot->connection = radio_connection;
    snapshot->metadata_revision = radio_metadata_revision;
    snapshot->metadata = radio_metadata;
    xSemaphoreGive(radio_station_mutex);
}

bool radio_play_record(const radio_station_t *station)
{
    if (!radio_task_handle || !stations_valid(station)) return false;
    size_t index = SIZE_MAX;
    xSemaphoreTake(radio_station_mutex, portMAX_DELAY);
    stations_find(station, &index);
    bool switching = !radio_stop_requested &&
                     !stations_same(&radio_requested_station, station);
    radio_requested_station = *station;
    radio_selected_station = index;
    /* Publish the requested target immediately so a rapid second F4 toggles
     * mute for that target instead of treating the old stream as current. */
    radio_current_station = *station;
    radio_playing_station = index;
    if (switching) {
        radio_switch_requested = true;
        radio_reset_metadata_locked();
    }
    xSemaphoreGive(radio_station_mutex);
    if (switching) {
        xSemaphoreTake(radio_output_mutex, portMAX_DELAY);
        radio_set_buffering(true);
        audio_mute();
        xSemaphoreGive(radio_output_mutex);
        radio_browser_count_play(station->stationuuid);
    }
    return radio_play();
}

void radio_stop_for_exit(void)
{
    radio_stop_requested = true;
    audio_shutdown();
}

/**
 * @brief Request playback cleanup and return to the launcher.
 */
void radio_request_exit(void)
{
    /* Stop the PCM loop before closing audio admission. Both launcher paths
     * run the serialized shutdown protocol before restarting. */
    radio_exit_requested = true;
    audio_shutdown();

    ESP_LOGI(TAG,
             "Radio exit requested");

    /*
     * The stream task normally performs the launcher restart after cleaning
     * up an active connection.  Initialization can fail before that task is
     * created (for example, when no SD card is inserted), so there is no
     * worker to observe radio_exit_requested in that case.
     */
    if (radio_task_handle == NULL) {
        ESP_LOGI(TAG,
                 "Radio task is not running; returning to launcher directly");
        bsp_device_restart_to_launcher();
    }
}

/**
 * @brief Start or restart radio playback.
 */
bool radio_play(void)
{
    atomic_store(&radio_audio_error, 0);
    if (radio_task_handle == NULL) {
        ESP_LOGW(
            TAG,
            "Cannot start playback: radio task is not running");
        return false;
    }

    if (!radio_stop_requested) {
        return true;
    }

    /*
     * Discard PCM that may have accumulated before the stop request.
     * The next stream connection must begin with fresh decoded audio.
     */
    if (radio_pcm_stream != NULL) {
        radio_discard_pcm();
    }
    xSemaphoreTake(radio_station_mutex, portMAX_DELAY);
    radio_playing_station = radio_selected_station;
    radio_current_station = radio_requested_station;
    radio_reset_metadata_locked();
    radio_browser_count_play(radio_requested_station.stationuuid);
    radio_resume_requested = true;
    radio_stop_requested = false;
    xSemaphoreGive(radio_station_mutex);
    radio_set_buffering(true);
    audio_mute();

    /* The audio task unmutes only after the PCM high watermark is reached. */
    ESP_LOGI(
        TAG,
        "Radio playback start requested: %zu (%s)",
        radio_playing_station,
        radio_current_station.name);

    return true;
}

/**
 * @brief Stop radio playback without exiting the application.
 */
bool radio_stop(void)
{
    if (radio_task_handle == NULL) {
        ESP_LOGW(
            TAG,
            "Cannot stop playback: radio task is not running");
        return false;
    }

    if (radio_stop_requested) {
        ESP_LOGI(
            TAG,
            "Radio playback already stopped");
        return true;
    }

    xSemaphoreTake(radio_station_mutex, portMAX_DELAY);
    radio_stop_requested = true;
    radio_reset_metadata_locked();
    xSemaphoreGive(radio_station_mutex);
    radio_set_buffering(false);

    /*
     * Mute immediately while the HTTP/decoder task reaches its
     * cleanup point.
     */
    esp_err_t res = audio_mute();

    if (res != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Failed to mute audio during stop: %s",
            esp_err_to_name(res));
    }

    if (radio_pcm_stream != NULL) {
        radio_discard_pcm();
    }
    ESP_LOGI(
        TAG,
        "Radio playback stop requested");

    return true;
}

/**
 * @brief Toggle radio audio mute.
 */
static bool radio_toggle_mute_locked(void)
{
    if (radio_task_handle == NULL || radio_stop_requested) {
        ESP_LOGW(
            TAG,
            "Cannot toggle mute while radio playback is stopped");
        return false;
    }

    if (radio_muted) {
        if (radio_buffering || !radio_output_ready) {
            radio_muted = false;
            return false;
        }

        esp_err_t res = audio_unmute();

        if (res != ESP_OK) {
            ESP_LOGW(
                TAG,
                "Failed to unmute audio: %s",
                esp_err_to_name(res));
            return true;
        }

        radio_muted = false;

        ESP_LOGI(
            TAG,
            "Radio audio unmuted");

        return false;
    }

    esp_err_t res = audio_mute();

    if (res != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Failed to mute audio: %s",
            esp_err_to_name(res));
        return false;
    }

    radio_muted = true;

    ESP_LOGI(
        TAG,
        "Radio audio muted");

    return true;
}

bool radio_toggle_mute(void)
{
    if (!radio_output_mutex) return false;
    xSemaphoreTake(radio_output_mutex, portMAX_DELAY);
    bool muted = radio_toggle_mute_locked();
    xSemaphoreGive(radio_output_mutex);
    return muted;
}

/**
 * @brief Return whether radio playback is currently stopped.
 */
bool radio_is_stopped(void)
{
    return radio_stop_requested;
}

/**
 * @brief Return whether radio audio is currently muted.
 */
bool radio_is_muted(void)
{
    return radio_muted;
}

bool radio_is_buffering(void)
{
    return radio_buffering;
}

void radio_init(void)
{
    radio_exit_requested = false;
    radio_switch_requested = false;
    /* Keep the stream task idle until the user explicitly starts playback. */
    radio_stop_requested = true;
    radio_resume_requested = false;
    radio_muted = false;
    radio_buffering = false;
    radio_selected_station = 0;
    radio_playing_station = 0;

    esp_err_t storage_res = tanradio_sd_mount();
    if (storage_res != ESP_OK) {
        ESP_LOGE(TAG, "SD-card station storage is unavailable: %s",
                 esp_err_to_name(storage_res));
        return;
    }

    static const char station_file[] = STATIONS_FILE_PATH;

    storage_res = stations_ensure_file(station_file);
    if (storage_res != ESP_OK) {
        ESP_LOGE(TAG, "Unable to prepare station catalog at %s", station_file);
        return;
    }

    storage_res = stations_load_file(station_file);
    /* An empty catalog must still allow directory previews and saving. */
    if (storage_res != ESP_OK && storage_res != ESP_ERR_NOT_FOUND) {
        ESP_LOGE(TAG, "No station catalog available at %s", station_file);
        return;
    }

    radio_station_mutex = xSemaphoreCreateMutex();
    radio_output_mutex = xSemaphoreCreateMutex();
    if (!radio_station_mutex || !radio_output_mutex) return;
    const radio_station_t *initial = stations_get(0);
    if (initial) {
        radio_requested_station = *initial;
        radio_current_station = *initial;
    }

    radio_pcm_stream_memory = heap_caps_malloc(
        RADIO_PCM_STREAM_BUFFER_SIZE,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (radio_pcm_stream_memory == NULL) {
        ESP_LOGE(TAG, "Failed to allocate PCM stream buffer");
        return;
    }

    radio_pcm_stream = xStreamBufferCreateStatic(
        RADIO_PCM_STREAM_BUFFER_SIZE,
        RADIO_AUDIO_WRITE_CHUNK_SIZE,
        radio_pcm_stream_memory,
        &radio_pcm_stream_storage);

    if (radio_pcm_stream == NULL) {
        ESP_LOGE(TAG, "Failed to create PCM stream buffer");
        free(radio_pcm_stream_memory);
        radio_pcm_stream_memory = NULL;
        return;
    }

    radio_compressed_stream_memory = heap_caps_malloc(
        RADIO_COMPRESSED_BUFFER_SIZE,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (radio_compressed_stream_memory == NULL) {
        ESP_LOGE(TAG, "Failed to allocate compressed audio buffer");
        radio_pcm_stream = NULL;
        free(radio_pcm_stream_memory);
        radio_pcm_stream_memory = NULL;
        return;
    }

    radio_compressed_stream = xStreamBufferCreateStatic(
        RADIO_COMPRESSED_BUFFER_SIZE,
        1,
        radio_compressed_stream_memory,
        &radio_compressed_stream_storage);

    if (radio_compressed_stream == NULL) {
        ESP_LOGE(TAG, "Failed to create compressed audio buffer");
        free(radio_compressed_stream_memory);
        radio_compressed_stream_memory = NULL;
        radio_pcm_stream = NULL;
        free(radio_pcm_stream_memory);
        radio_pcm_stream_memory = NULL;
        return;
    }

    radio_wire_titles = xQueueCreate(12, sizeof(radio_title_marker_t));
    radio_pcm_titles = xQueueCreate(6, sizeof(radio_title_marker_t));
    if (!radio_wire_titles || !radio_pcm_titles) {
        if (radio_wire_titles) vQueueDelete(radio_wire_titles);
        if (radio_pcm_titles) vQueueDelete(radio_pcm_titles);
        radio_wire_titles = radio_pcm_titles = NULL;
        ESP_LOGW(TAG, "Unable to allocate ICY title queues");
    }

    BaseType_t audio_task_res = xTaskCreate(
        radio_audio_task,
        "tanradio_audio",
        4096,
        NULL,
        6,
        &radio_audio_task_handle);

    if (audio_task_res != pdPASS) {
        radio_audio_task_handle = NULL;
        radio_compressed_stream = NULL;
        free(radio_compressed_stream_memory);
        radio_compressed_stream_memory = NULL;
        radio_pcm_stream = NULL;
        free(radio_pcm_stream_memory);
        radio_pcm_stream_memory = NULL;
        ESP_LOGE(TAG, "Failed to create PCM output task");
        return;
    }

    BaseType_t task_res = xTaskCreate(
        radio_stream_task,
        "tanradio_radio",
        20 * 1024,
        NULL,
        5,
        &radio_task_handle);

    if (task_res != pdPASS) {
        radio_task_handle = NULL;

        ESP_LOGE(TAG,
                 "Failed to create radio stream task");

        return;
    }

    ESP_LOGI(TAG,
             "Radio subsystem initialized");
}

const char *radio_get_audio_error(void)
{
    switch (atomic_load(&radio_audio_error)) {
        case RADIO_ERROR_PCM: return "Unsupported sample rate, depth or channel count";
        case RADIO_ERROR_OUTPUT: return "Audio output error";
        case RADIO_ERROR_FORMAT: return "Unsupported stream format";
        case RADIO_ERROR_UNAVAILABLE: return "Station unavailable";
        case RADIO_ERROR_TLS: return "Station certificate could not be verified";
        case RADIO_ERROR_HLS: return "Unsupported HLS playlist";
        case RADIO_ERROR_DECODE: return "Could not decode this station";
        default: return NULL;
    }
}

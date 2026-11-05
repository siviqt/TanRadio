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
#include <string.h>

/* ESP-IDF */
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
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
static atomic_int radio_audio_error; /* 0=none, 1=format, 2=output configuration */
static radio_station_t radio_requested_station;
static radio_station_t radio_current_station;

#define RADIO_STREAM_BUFFER_SIZE 16384
#define RADIO_COMPRESSED_BUFFER_SIZE (256 * 1024)
#define RADIO_COMPRESSED_START_LEVEL (16 * 1024)
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
    int result;
} radio_download_context_t;

typedef struct {
    radio_metadata_t metadata;
    uint32_t interval;
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
    if (event->event_id != HTTP_EVENT_ON_HEADER) return ESP_OK;
    radio_stream_headers_t *headers = event->user_data;
    const char *key = event->header_key, *value = event->header_value;
    if (!strcasecmp(key, "icy-metaint")) {
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

static bool radio_download_active(const radio_download_context_t *context)
{
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
    while (radio_download_active(context)) {
        int count = esp_http_client_read(context->client, (char *)context->buffer,
                                         RADIO_STREAM_BUFFER_SIZE);
        if (count <= 0) {
            context->result = count;
            break;
        }
        if (!icy_filter_feed(context->icy, context->buffer, count,
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

    if (radio_buffering) {
        if (available < RADIO_PCM_HIGH_WATERMARK) {
            memset(pcm_chunk, 0, RADIO_AUDIO_WRITE_CHUNK_SIZE);
            audio_write_pcm(pcm_chunk, RADIO_AUDIO_WRITE_CHUNK_SIZE);
            return;
        }

        radio_set_buffering(false);
        if (!radio_muted) {
            audio_unmute();
        }
    } else if (available < RADIO_PCM_LOW_WATERMARK) {
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

static void radio_discard_pcm(void)
{
    if (radio_output_mutex) xSemaphoreTake(radio_output_mutex, portMAX_DELAY);
    radio_output_ready = false;
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

/** Own the HTTP connection, decoder and the compressed-to-PCM timeline. */
static void radio_stream_task(void *arg)
{
    (void)arg;

    size_t station_index = radio_playing_station;
    radio_station_t stream_station;
    bool plain_retry = false;

    while (true) {
        /*
         * F3 stops the current online stream but keeps the radio task
         * alive. The task waits here until F4 clears the stop request.
         */
        if (radio_stop_requested && !radio_exit_requested) {
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
                 "Radio playback started: %s (%s)",
                 station->name,
                 station->url);
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

        esp_http_client_config_t config = {
            .url = station->url,
            .timeout_ms = 3000,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .buffer_size = RADIO_STREAM_BUFFER_SIZE,
            .event_handler = radio_stream_header,
            .user_data = &headers,
        };

        client = esp_http_client_init(&config);

        if (client == NULL) {
            ESP_LOGE(TAG, "Failed to initialize HTTP client");
            goto reconnect;
        }

        esp_err_t res = esp_http_client_set_header(
            client,
            "User-Agent",
            "TanRadio/0.1");

        if (res != ESP_OK) {
            ESP_LOGE(TAG,
                     "Failed to set HTTP User-Agent: %s",
                     esp_err_to_name(res));
            goto stream_cleanup;
        }

        if (request_icy && esp_http_client_set_header(client, "Icy-MetaData", "1") != ESP_OK)
            goto stream_cleanup;

        /*
         * The radio player uses the HTTP client's streaming API
         * (open -> fetch_headers -> read), rather than
         * esp_http_client_perform(). Automatic redirect handling is
         * performed by esp_http_client_perform(), so it does not occur
         * automatically in this streaming path.
         */
        enum {
            RADIO_MAX_REDIRECTS = 10,
        };

        int redirect_count = 0;
        int status_code = 0;
        int64_t content_length = -1;

        while (true) {
            headers = (radio_stream_headers_t){0};
            res = esp_http_client_open(client, 0);

            if (res != ESP_OK) {
                ESP_LOGE(TAG,
                         "Failed to open radio stream: %s",
                         esp_err_to_name(res));

                if (esp_http_client_get_transport_type(client) ==
                    HTTP_TRANSPORT_OVER_SSL) {
                    int tls_error = 0;
                    int tls_flags = 0;

                    if (esp_http_client_get_and_clear_last_tls_error(
                            client,
                            &tls_error,
                            &tls_flags) == ESP_OK) {
                        ESP_LOGE(TAG,
                                 "TLS diagnostics: error=0x%x flags=0x%x",
                                 tls_error,
                                 tls_flags);
                    }
                }

                goto stream_cleanup;
            }

            content_length =
                esp_http_client_fetch_headers(client);

            if (content_length < 0) {
                ESP_LOGE(TAG,
                         "Failed to read HTTP response headers: %lld",
                         (long long)content_length);
                goto stream_cleanup;
            }

            status_code =
                esp_http_client_get_status_code(client);

            if (status_code == 301 ||
                status_code == 302 ||
                status_code == 303 ||
                status_code == 307 ||
                status_code == 308) {

                if (redirect_count >= RADIO_MAX_REDIRECTS) {
                    ESP_LOGE(TAG,
                             "Radio redirect limit reached (%d)",
                             RADIO_MAX_REDIRECTS);
                    goto stream_cleanup;
                }

                /*
                 * Location is stored internally by esp_http_client for
                 * redirect processing. It is not obtained through
                 * esp_http_client_get_response_header(), so the saved
                 * response-header size limit does not truncate the
                 * redirect URL used here.
                 */
                res = esp_http_client_close(client);

                if (res != ESP_OK) {
                    ESP_LOGE(TAG,
                             "Failed to close redirected radio connection: %s",
                             esp_err_to_name(res));
                    goto stream_cleanup;
                }

                res = esp_http_client_clear_response_buffer(client);

                if (res != ESP_OK) {
                    ESP_LOGE(TAG,
                             "Failed to clear redirected radio response buffer: %s",
                             esp_err_to_name(res));
                    goto stream_cleanup;
                }

                res = esp_http_client_set_redirection(client);

                if (res != ESP_OK) {
                    ESP_LOGE(TAG,
                             "HTTP %d redirect did not provide a usable Location: %s",
                             status_code,
                             esp_err_to_name(res));
                    goto stream_cleanup;
                }

                redirect_count++;

                ESP_LOGI(TAG,
                         "Following radio redirect %d/%d",
                         redirect_count,
                         RADIO_MAX_REDIRECTS);

                continue;
            }

            break;
        }

        ESP_LOGI(TAG,
                 "Radio stream connected: HTTP %d, content-length=%lld",
                 status_code,
                 (long long)content_length);

        if (status_code < 200 || status_code >= 300) {
            ESP_LOGE(TAG, "Radio server returned HTTP status %d", status_code);
            goto stream_cleanup;
        }

        if (headers.interval_invalid) {
            ESP_LOGW(TAG, "Invalid ICY interval%s", request_icy ? ". Retrying without metadata." : "");
            plain_retry = request_icy;
            goto stream_cleanup;
        }
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

        esp_audio_simple_dec_cfg_t decoder_cfg = {
            .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,
            .dec_cfg = NULL,
            .cfg_size = 0,
            .use_frame_dec = false,
        };

        esp_audio_err_t audio_res =
            esp_audio_dec_register_default();

        if (audio_res != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG,
                     "Failed to register audio decoders: %d",
                     audio_res);
            goto stream_cleanup;
        }

        audio_decoder_registered = true;

        audio_res = esp_audio_simple_dec_register_default();

        if (audio_res != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG,
                     "Failed to register simple audio decoders: %d",
                     audio_res);
            goto stream_cleanup;
        }

        simple_decoder_registered = true;

        audio_res = esp_audio_simple_dec_open(
            &decoder_cfg,
            &decoder);

        if (audio_res != ESP_AUDIO_ERR_OK || decoder == NULL) {
            ESP_LOGE(TAG,
                     "Failed to open MP3 decoder: %d",
                     audio_res);
            goto stream_cleanup;
        }

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
            4096,
            &download_context,
            5,
            &download_task_handle);

        if (download_task_res != pdPASS) {
            ESP_LOGE(TAG, "Failed to create radio download task");
            goto stream_cleanup;
        }

        radio_set_buffering(true);
        audio_mute();

        while (xStreamBufferBytesAvailable(radio_compressed_stream) <
                   RADIO_COMPRESSED_START_LEVEL &&
               !atomic_load(&download_context.finished) &&
               !radio_stream_interrupted()) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        while (true) {
            size_t bytes_read = xStreamBufferReceive(
                radio_compressed_stream,
                buffer,
                RADIO_STREAM_BUFFER_SIZE,
                pdMS_TO_TICKS(250));

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
                    .eos = false,
                    .consumed = 0,
                    .frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE,
                };

                while (raw.len > 0) {
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
                        if (output.needed_size <= pcm_buffer_size) {
                            ESP_LOGE(TAG,
                                     "MP3 decoder reported invalid required output size: %lu",
                                     (unsigned long)output.needed_size);
                            goto stream_cleanup;
                        }

                        uint8_t *new_buffer =
                            realloc(pcm_buffer, output.needed_size);

                        if (new_buffer == NULL) {
                            ESP_LOGE(TAG,
                                     "Failed to resize MP3 PCM output buffer to %lu bytes",
                                     (unsigned long)output.needed_size);
                            goto stream_cleanup;
                        }

                        pcm_buffer = new_buffer;
                        pcm_buffer_size = output.needed_size;

                        ESP_LOGI(TAG,
                                 "MP3 PCM output buffer resized to %lu bytes",
                                 (unsigned long)pcm_buffer_size);

                        continue;
                    }

                    if (audio_res != ESP_AUDIO_ERR_OK) {
                        ESP_LOGE(TAG,
                                 "MP3 decode failed: %d",
                                 audio_res);
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
                        if (audio_res != ESP_AUDIO_ERR_OK ||
                            !audio_pcm_supported(info.sample_rate, info.bits_per_sample, info.channel)) {
                            ESP_LOGE(TAG, "Unsupported PCM: %lu Hz, %u bit, %u channels",
                                     (unsigned long)info.sample_rate, info.bits_per_sample, info.channel);
                            atomic_store(&radio_audio_error, 1);
                            radio_stop_requested = true;
                            radio_set_buffering(false);
                            audio_mute();
                            goto stream_cleanup;
                        }
                        if (!decoded_frames || info.sample_rate != output_format.sample_rate ||
                            info.bits_per_sample != output_format.bits_per_sample ||
                            info.channel != output_format.channel) {
                            esp_err_t format_res = radio_prepare_output(info.sample_rate, connection);
                            if (format_res != ESP_OK) {
                                ESP_LOGE(TAG, "PCM configuration failed: %s", esp_err_to_name(format_res));
                                atomic_store(&radio_audio_error, 2);
                                radio_stop_requested = true;
                                radio_set_buffering(false);
                                goto stream_cleanup;
                            }
                            queued_pcm_position = 0;
                            title_pending = have_title;
                            output_format = info;
                            ESP_LOGI(TAG, "MP3 audio: %lu Hz, %u bit, %u channel, %lu bitrate",
                                     (unsigned long)info.sample_rate, info.bits_per_sample,
                                     info.channel, (unsigned long)info.bitrate);
                        }
                        if (title_pending && radio_pcm_titles) {
                            next_title.position = queued_pcm_position;
                            title_pending = xQueueSend(radio_pcm_titles, &next_title, 0) != pdTRUE;
                        }
                        total_pcm_bytes += output.decoded_size;
                        if (!radio_queue_pcm(output.buffer, output.decoded_size, info.channel))
                            goto stream_cleanup;
                        queued_pcm_position += output.decoded_size * (info.channel == 1 ? 2 : 1);
                        decoded_frames++;
                    }

                    if (raw.consumed == 0) {
                        ESP_LOGW(TAG,
                                 "MP3 decoder consumed no input");
                        break;
                    }

                    raw.len -= raw.consumed;
                    raw.buffer += raw.consumed;
                    raw.consumed = 0;
                }

                if (raw.len != 0) {
                    ESP_LOGW(TAG,
                             "MP3 decoder left %lu input bytes unconsumed",
                             (unsigned long)raw.len);
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

                continue;
            }

            if (radio_stream_interrupted()) {
                goto stream_cleanup;
            }

            if (atomic_load(&download_context.finished) &&
                xStreamBufferBytesAvailable(radio_compressed_stream) == 0) {
                if (download_context.result == 0) {
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

        if (client != NULL) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            client = NULL;
        }

        if (radio_exit_requested) {
            ESP_LOGI(TAG,
                     "Radio playback cleanup complete");
            break;
        }

        if (radio_switch_requested || radio_resume_requested) {
            plain_retry = false;
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

reconnect:
        if (radio_exit_requested) {
            ESP_LOGI(TAG,
                     "Radio exit requested; skipping reconnect");
            break;
        }

        radio_set_buffering(true);
        audio_mute();

        ESP_LOGW(TAG,
                 "Radio playback reconnecting in 2 seconds");

        vTaskDelay(pdMS_TO_TICKS(2000));
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

void radio_catalog_changed(void)
{
    if (!radio_station_mutex) return;
    xSemaphoreTake(radio_station_mutex, portMAX_DELAY);
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
    const radio_station_t *station = stations_get(index);
    if (station == NULL || radio_station_mutex == NULL) {
        return false;
    }

    xSemaphoreTake(radio_station_mutex, portMAX_DELAY);
    bool changed = !stations_same(&radio_requested_station, station);
    radio_requested_station = *station;
    radio_selected_station = index;
    bool switching = radio_task_handle != NULL && !radio_stop_requested && changed;
    if (switching) {
        radio_switch_requested = true;
        radio_reset_metadata_locked();
        radio_current_station = *station;
        radio_playing_station = index;
    }
    xSemaphoreGive(radio_station_mutex);

    if (switching) {
        radio_browser_count_play(station->stationuuid);
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
            radio_selected_station,
            radio_station_get_name(radio_selected_station));
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
    return stations_valid(station) && strcmp(station->codec, "MP3") == 0 &&
           radio_play_record(station);
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
    stations_find(station, &index);
    xSemaphoreTake(radio_station_mutex, portMAX_DELAY);
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
    if (radio_compressed_stream != NULL) {
        xStreamBufferReset(radio_compressed_stream);
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
    if (radio_compressed_stream != NULL) {
        xStreamBufferReset(radio_compressed_stream);
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
        8192,
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
        case 1: return "Unsupported audio format";
        case 2: return "Audio output error";
        default: return NULL;
    }
}

#include "radio/hls_stream.h"

#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "radio/hls_manifest.h"

#define HLS_MANIFEST_LIMIT (48 * 1024)
#define HLS_SEGMENT_LIMIT (4 * 1024 * 1024)
#define HLS_REQUEST_US (12 * 1000000LL)

struct hls_stream {
    esp_http_client_handle_t client;
    char playlist_url[STREAM_URL_CAPACITY];
    char base_url[STREAM_URL_CAPACITY];
    char request_url[STREAM_URL_CAPACITY];
    char text[HLS_MANIFEST_LIMIT + 1];
    hls_manifest_t manifest;
};

hls_stream_t *hls_stream_create(void)
{
    return heap_caps_calloc(1, sizeof(hls_stream_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void hls_stream_destroy(hls_stream_t *stream)
{
    if (!stream) return;
    stream_http_close(&stream->client);
    free(stream);
}

static hls_stream_result_t http_failure(stream_http_result_t result, bool segment)
{
    if (result == STREAM_HTTP_TLS) return HLS_STREAM_TLS;
    if (result == STREAM_HTTP_UNAVAILABLE && !segment) return HLS_STREAM_UNAVAILABLE;
    return HLS_STREAM_RETRY;
}

static bool manifest_read(hls_stream_t *stream, esp_http_client_handle_t client, size_t used,
                          stream_active_fn active, void *context)
{
    if (esp_http_client_get_url(client, stream->base_url, sizeof(stream->base_url)) != ESP_OK)
        return false;
    int64_t deadline = esp_timer_get_time() + HLS_REQUEST_US;
    bool complete = esp_http_client_is_complete_data_received(client);
    while (!complete && used < HLS_MANIFEST_LIMIT && active(context) && esp_timer_get_time() < deadline) {
        size_t limit = HLS_MANIFEST_LIMIT - used;
        if (limit > STREAM_HTTP_CHUNK) limit = STREAM_HTTP_CHUNK;
        int received = esp_http_client_read(client, stream->text + used, limit);
        if (received < 0) return false;
        if (!received) {
            complete = esp_http_client_is_complete_data_received(client) ||
                       (!esp_http_client_is_chunked_response(client) && esp_http_client_get_content_length(client) <= 0);
            break;
        }
        used += (size_t)received;
        complete = esp_http_client_is_complete_data_received(client);
    }
    stream->text[used] = 0;
    return complete && used && !memchr(stream->text, 0, used);
}

static hls_stream_result_t send_segment(hls_stream_t *stream, uint8_t *buffer, stream_active_fn active,
                                        stream_sink_fn sink, void *context)
{
    stream_http_result_t result = stream_http_open(&stream->client, stream->request_url,
                                                    NULL, NULL, false, active, context);
    if (result != STREAM_HTTP_OK) return http_failure(result, true);
    int64_t length = esp_http_client_get_content_length(stream->client);
    if (length > HLS_SEGMENT_LIMIT) return HLS_STREAM_UNSUPPORTED;
    size_t read_size = 0, skip = 0, prefix = 0;
    bool delivered = false;
    while (active(context)) {
        int count = esp_http_client_read(stream->client, (char *)buffer + prefix, STREAM_HTTP_CHUNK - prefix);
        if (count < 0) return HLS_STREAM_RETRY;
        if (!count) {
            bool complete = esp_http_client_is_complete_data_received(stream->client) ||
                            (!esp_http_client_is_chunked_response(stream->client) && length <= 0);
            return delivered && complete ? HLS_STREAM_COMPLETE : HLS_STREAM_RETRY;
        }
        if ((size_t)count > HLS_SEGMENT_LIMIT - read_size) return HLS_STREAM_UNSUPPORTED;
        bool first = read_size == prefix;
        read_size += (size_t)count;
        size_t size = prefix + (size_t)count;
        if (first && size < 10) {
            prefix = size;
            continue;
        }
        if (first) skip = stream_id3_size(buffer, size);
        prefix = 0;
        size_t ignored = skip < size ? skip : size;
        skip -= ignored;
        if (ignored < size) {
            if (!sink(context, buffer + ignored, size - ignored)) return HLS_STREAM_RETRY;
            delivered = true;
        }
        if (esp_http_client_is_complete_data_received(stream->client))
            return delivered ? HLS_STREAM_COMPLETE : HLS_STREAM_RETRY;
    }
    return HLS_STREAM_RETRY;
}

static bool wait_for_refresh(unsigned seconds, stream_active_fn active, void *context)
{
    unsigned ms = seconds > 20 ? 10000 : seconds * 500;
    while (ms && active(context)) {
        unsigned step = ms < 100 ? ms : 100;
        vTaskDelay(pdMS_TO_TICKS(step));
        ms -= step;
    }
    return active(context);
}

hls_stream_result_t hls_stream_run(hls_stream_t *stream, esp_http_client_handle_t initial,
                                   uint8_t *buffer, size_t initial_bytes,
                                   stream_active_fn active, stream_sink_fn sink, void *context)
{
    if (initial_bytes > HLS_MANIFEST_LIMIT) return HLS_STREAM_UNSUPPORTED;
    memcpy(stream->text, buffer, initial_bytes);
    if (!manifest_read(stream, initial, initial_bytes, active, context)) return HLS_STREAM_RETRY;
    /* Release this connection before fetching a variant or segments. Its
     * owner still destroys the HTTP handle after the worker has stopped. */
    esp_http_client_close(initial);
    memcpy(stream->playlist_url, stream->base_url, strlen(stream->base_url) + 1);
    bool started = false;
    uint64_t next_sequence = 0;
    unsigned master_depth = 0;
    while (active(context)) {
        hls_manifest_t *manifest = &stream->manifest;
        hls_manifest_type_t type = hls_parse_manifest(stream->text, manifest);
        if (type == HLS_MANIFEST_MASTER) {
            if (++master_depth > 3 || started ||
                !hls_resolve_uri(stream->base_url, manifest->variant, stream->playlist_url,
                                 sizeof(stream->playlist_url))) return HLS_STREAM_UNSUPPORTED;
        } else if (type == HLS_MANIFEST_MEDIA) {
            if (!started) next_sequence = manifest->sequence + hls_start_segment(manifest);
            if (next_sequence < manifest->sequence) return HLS_STREAM_RETRY;
            uint64_t end = manifest->sequence + manifest->count;
            while (next_sequence < end && active(context)) {
                const hls_segment_t *segment = &manifest->segments[next_sequence - manifest->sequence];
                if (started && segment->discontinuity)
                    return manifest->end_list ? HLS_STREAM_UNSUPPORTED : HLS_STREAM_RETRY;
                if (!hls_resolve_uri(stream->base_url, segment->uri, stream->request_url,
                                     sizeof(stream->request_url))) return HLS_STREAM_UNSUPPORTED;
                hls_stream_result_t sent = send_segment(stream, buffer, active, sink, context);
                stream_http_close(&stream->client);
                if (sent != HLS_STREAM_COMPLETE) return sent;
                started = true;
                next_sequence++;
            }
            if (manifest->end_list && next_sequence >= end) return HLS_STREAM_COMPLETE;
            if (!wait_for_refresh(manifest->target_seconds, active, context)) return HLS_STREAM_RETRY;
        } else {
            return type == HLS_MANIFEST_UNSUPPORTED || !started ? HLS_STREAM_UNSUPPORTED : HLS_STREAM_RETRY;
        }
        stream_http_result_t result = stream_http_open(&stream->client, stream->playlist_url,
                                                        NULL, NULL, false, active, context);
        if (result != STREAM_HTTP_OK) return http_failure(result, false);
        bool read = manifest_read(stream, stream->client, 0, active, context);
        stream_http_close(&stream->client);
        if (!read) return HLS_STREAM_RETRY;
    }
    return HLS_STREAM_RETRY;
}

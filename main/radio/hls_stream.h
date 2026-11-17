#ifndef TANRADIO_HLS_STREAM_H
#define TANRADIO_HLS_STREAM_H

#include "radio/stream_http.h"

typedef struct hls_stream hls_stream_t;

typedef enum {
    HLS_STREAM_RETRY,
    HLS_STREAM_COMPLETE,
    HLS_STREAM_UNSUPPORTED,
    HLS_STREAM_UNAVAILABLE,
    HLS_STREAM_TLS,
} hls_stream_result_t;

hls_stream_t *hls_stream_create(void);
void hls_stream_destroy(hls_stream_t *stream);
/* The caller owns the initial response. The HLS stream owns later requests. */
hls_stream_result_t hls_stream_run(hls_stream_t *stream, esp_http_client_handle_t initial,
                                   uint8_t *buffer, size_t initial_bytes,
                                   stream_active_fn active, stream_sink_fn sink, void *context);

#endif

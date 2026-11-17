#ifndef TANRADIO_STREAM_HTTP_H
#define TANRADIO_STREAM_HTTP_H

#include "esp_http_client.h"
#include "radio/stream_format.h"

#define STREAM_HTTP_CHUNK 2048

typedef enum {
    STREAM_HTTP_OK,
    STREAM_HTTP_RETRY,
    STREAM_HTTP_UNAVAILABLE,
    STREAM_HTTP_TLS,
} stream_http_result_t;

typedef bool (*stream_active_fn)(void *context);
typedef bool (*stream_sink_fn)(void *context, const uint8_t *bytes, size_t size);

/* The caller owns the handle, including on errors, until stream_http_close. */
stream_http_result_t stream_http_open(esp_http_client_handle_t *client, const char *url,
                                      http_event_handle_cb events, void *headers, bool icy,
                                      stream_active_fn active, void *context);
void stream_http_close(esp_http_client_handle_t *client);

#endif

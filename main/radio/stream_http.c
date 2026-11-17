#include "radio/stream_http.h"

#include "esp_crt_bundle.h"
#include "esp_log.h"
#include <strings.h>

static const char *TAG = "tanradio.http";

static bool http_url(const char *url)
{
    return !strncasecmp(url, "http://", 7) || !strncasecmp(url, "https://", 8);
}

stream_http_result_t stream_http_open(esp_http_client_handle_t *client, const char *url,
                                      http_event_handle_cb events, void *headers, bool icy,
                                      stream_active_fn active, void *context)
{
    if (!http_url(url)) return STREAM_HTTP_UNAVAILABLE;
    const esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 3000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = "TanRadio/0.9",
        .disable_auto_redirect = true,
        .buffer_size = STREAM_HTTP_CHUNK,
        .event_handler = events,
        .user_data = headers,
    };
    *client = esp_http_client_init(&config);
    if (!*client || (icy && esp_http_client_set_header(*client, "Icy-MetaData", "1") != ESP_OK))
        return STREAM_HTTP_RETRY;

    /* ESP-IDF's streaming interface requires explicit redirect handling. */
    char redirect_url[STREAM_URL_CAPACITY];
    for (unsigned redirects = 0; redirects <= 8 && active(context); redirects++) {
        esp_err_t opened = esp_http_client_open(*client, 0);
        if (opened != ESP_OK) {
            int tls_error = 0, tls_flags = 0;
            esp_http_client_get_and_clear_last_tls_error(*client, &tls_error, &tls_flags);
            ESP_LOGW(TAG, "Stream connection failed: %s (TLS %d flags 0x%x)",
                     esp_err_to_name(opened), tls_error, tls_flags);
            return tls_flags ? STREAM_HTTP_TLS : STREAM_HTTP_RETRY;
        }
        int64_t length = esp_http_client_fetch_headers(*client);
        if (length < 0) return STREAM_HTTP_RETRY;
        int status = esp_http_client_get_status_code(*client);
        if (status >= 200 && status < 300) return STREAM_HTTP_OK;
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            if (esp_http_client_close(*client) != ESP_OK ||
                esp_http_client_clear_response_buffer(*client) != ESP_OK ||
                esp_http_client_set_redirection(*client) != ESP_OK ||
                esp_http_client_get_url(*client, redirect_url, sizeof(redirect_url)) != ESP_OK ||
                !http_url(redirect_url)) return STREAM_HTTP_UNAVAILABLE;
            continue;
        }
        ESP_LOGW(TAG, "Station server returned HTTP %d", status);
        return status >= 400 && status < 500 && status != 408 && status != 429
            ? STREAM_HTTP_UNAVAILABLE : STREAM_HTTP_RETRY;
    }
    return active(context) ? STREAM_HTTP_UNAVAILABLE : STREAM_HTTP_RETRY;
}

void stream_http_close(esp_http_client_handle_t *client)
{
    if (!*client) return;
    esp_http_client_close(*client);
    esp_http_client_cleanup(*client);
    *client = NULL;
}

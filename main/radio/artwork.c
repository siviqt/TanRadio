#include "radio/artwork.h"
#include "radio/browser.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "driver/jpeg_decode.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "psa/crypto.h"
#include "spng.h"

#define ARTWORK_MAX_BYTES (128 * 1024)
#define ARTWORK_MAX_DIMENSION 512
#define ARTWORK_CACHE_SIZE 3
#define ARTWORK_TIMEOUT_US (6000 * 1000LL)

typedef struct {
    radio_station_t station;
    unsigned sequence;
    bool online;
} artwork_job_t;

typedef struct {
    char key[65];
    bool online;
    station_logo_t logo;
} artwork_cache_t;

static artwork_job_t submitted;
static QueueHandle_t jobs;
static SemaphoreHandle_t lock;
static atomic_uint wanted;
static unsigned published;
static station_logo_t *result;
/* Only the worker reads and writes the cache. */
static artwork_cache_t *cache;
static unsigned next_slot;

static bool artwork_current(const artwork_job_t *job)
{
    return atomic_load(&wanted) == job->sequence;
}

static bool artwork_https_url(const char *url)
{
    if (strncmp(url, "https://", 8)) return false;
    const char *host = url + 8;
    size_t authority = strcspn(host, "/?#");
    if (!authority || memchr(host, '@', authority)) return false;
    for (; *host; host++) {
        if ((unsigned char)*host <= ' ' || *host == '\\' || *host == 127) return false;
    }
    return true;
}

static size_t artwork_fetch(const artwork_job_t *job, const char *url, uint8_t *body)
{
    if (!artwork_https_url(url) || !artwork_current(job)) return 0;
    const esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
        .timeout_ms = 1800,
        .buffer_size = 2048,
        .user_agent = "TanRadio (station logos)",
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return 0;
    int64_t deadline = esp_timer_get_time() + ARTWORK_TIMEOUT_US;
    size_t used = 0;
    bool complete = false;
    for (unsigned hop = 0; hop <= 3 && artwork_current(job) && esp_timer_get_time() < deadline; hop++) {
        if (esp_http_client_open(client, 0) != ESP_OK) break;
        int64_t length = esp_http_client_fetch_headers(client);
        if (length < 0) break;
        int status = esp_http_client_get_status_code(client);
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            char redirect[STATIONS_MAX_URL_LENGTH + 2];
            if (esp_http_client_close(client) != ESP_OK ||
                esp_http_client_clear_response_buffer(client) != ESP_OK ||
                esp_http_client_set_redirection(client) != ESP_OK ||
                esp_http_client_get_url(client, redirect, sizeof(redirect)) != ESP_OK ||
                strlen(redirect) > STATIONS_MAX_URL_LENGTH || !artwork_https_url(redirect)) break;
            continue;
        }
        if (status != 200 || length > ARTWORK_MAX_BYTES) break;
        while (used < ARTWORK_MAX_BYTES && artwork_current(job) && esp_timer_get_time() < deadline) {
            size_t limit = ARTWORK_MAX_BYTES - used;
            int count = esp_http_client_read(client, (char *)body + used, limit > 2048 ? 2048 : limit);
            if (count < 0) break;
            used += count;
            complete = esp_http_client_is_complete_data_received(client) || (!count && !length);
            if (complete || !count) break;
        }
        break;
    }
    esp_http_client_cleanup(client);
    return complete && artwork_current(job) ? used : 0;
}

static bool artwork_url_key(const char *url, char key[65])
{
    uint8_t hash[32];
    size_t length;
    if (psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t *)url, strlen(url),
                         hash, sizeof(hash), &length) != PSA_SUCCESS || length != sizeof(hash)) return false;
    const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(hash); i++) {
        key[i * 2] = hex[hash[i] >> 4];
        key[i * 2 + 1] = hex[hash[i] & 15];
    }
    key[64] = 0;
    return true;
}

static size_t artwork_read(const char *key, const char *extension, uint8_t *body)
{
    char path[100];
    snprintf(path, sizeof(path), "/sd/tanradio/logos/%s.%s", key, extension);
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    size_t size = fread(body, 1, ARTWORK_MAX_BYTES, file);
    bool valid = fgetc(file) == EOF && !ferror(file);
    fclose(file);
    return valid ? size : 0;
}

/* Fit within a square, composite alpha on white and build both thumbnails. */
static void artwork_thumbnail(station_logo_t *logo, const uint8_t *pixels,
                              unsigned width, unsigned height, unsigned stride, unsigned channels)
{
    unsigned fit_width = width >= height ? STATION_LOGO_SIZE : width * STATION_LOGO_SIZE / height;
    unsigned fit_height = height >= width ? STATION_LOGO_SIZE : height * STATION_LOGO_SIZE / width;
    if (!fit_width) fit_width = 1;
    if (!fit_height) fit_height = 1;
    unsigned left = (STATION_LOGO_SIZE - fit_width) / 2;
    unsigned top = (STATION_LOGO_SIZE - fit_height) / 2;
    memset(logo->color, 0xff, sizeof(logo->color));
    memset(logo->mono, 0, sizeof(logo->mono));
    for (unsigned y = 0; y < fit_height; y++) {
        for (unsigned x = 0; x < fit_width; x++) {
            const uint8_t *p = pixels + ((y * height / fit_height) * stride +
                                        x * width / fit_width) * channels;
            unsigned a = channels == 4 ? p[3] : 255;
            unsigned r = (p[0] * a + 255 * (255 - a)) / 255;
            unsigned g = (p[1] * a + 255 * (255 - a)) / 255;
            unsigned b = (p[2] * a + 255 * (255 - a)) / 255;
            logo->color[(top + y) * STATION_LOGO_SIZE + left + x] =
                ((r & 248) << 8) | ((g & 252) << 3) | (b >> 3);
        }
    }
    /* Average a 3x3 block before thresholding the monochrome icon. */
    for (unsigned y = 0; y < EXTERNAL_LOGO_SIZE; y++) {
        for (unsigned x = 0; x < EXTERNAL_LOGO_SIZE; x++) {
            unsigned sum = 0;
            for (unsigned row = 0; row < 3; row++) {
                for (unsigned column = 0; column < 3; column++) {
                    uint16_t p = logo->color[(y * 3 + row) * STATION_LOGO_SIZE + x * 3 + column];
                    sum += (p >> 11) * 16 + ((p >> 5) & 63) * 9 + (p & 31) * 5;
                }
            }
            if (sum < 6100) logo->mono[y * 2 + x / 8] |= 0x80 >> (x % 8);
        }
    }
}

static bool artwork_png(station_logo_t *logo, const uint8_t *data, size_t size)
{
    spng_ctx *context = spng_ctx_new(0);
    if (!context) return false;
    struct spng_ihdr header;
    size_t bytes;
    bool ok = false;
    if (!spng_set_image_limits(context, ARTWORK_MAX_DIMENSION, ARTWORK_MAX_DIMENSION) &&
        !spng_set_chunk_limits(context, ARTWORK_MAX_BYTES, ARTWORK_MAX_BYTES) &&
        !spng_set_png_buffer(context, data, size) && !spng_get_ihdr(context, &header) &&
        !spng_decoded_image_size(context, SPNG_FMT_RGBA8, &bytes) &&
        bytes <= ARTWORK_MAX_DIMENSION * ARTWORK_MAX_DIMENSION * 4) {
        uint8_t *pixels = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (pixels && !spng_decode_image(context, pixels, bytes, SPNG_FMT_RGBA8, SPNG_DECODE_TRNS)) {
            artwork_thumbnail(logo, pixels, header.width, header.height, header.width, 4);
            ok = true;
        }
        free(pixels);
    }
    spng_ctx_free(context);
    return ok;
}

static bool artwork_jpeg(station_logo_t *logo, const uint8_t *data, size_t size)
{
    jpeg_decode_picture_info_t info;
    if (jpeg_decoder_get_info(data, size, &info) != ESP_OK ||
        !info.width || !info.height || info.width > ARTWORK_MAX_DIMENSION ||
        info.height > ARTWORK_MAX_DIMENSION) return false;

    unsigned horizontal = info.sample_method == JPEG_DOWN_SAMPLING_YUV420 ||
                          info.sample_method == JPEG_DOWN_SAMPLING_YUV422 ? 16 : 8;
    unsigned vertical = info.sample_method == JPEG_DOWN_SAMPLING_YUV420 ? 16 : 8;
    unsigned stride = (info.width + horizontal - 1) & ~(horizontal - 1);
    unsigned height = (info.height + vertical - 1) & ~(vertical - 1);
    size_t allocated;
    const jpeg_decode_memory_alloc_cfg_t memory = {.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER};
    uint8_t *pixels = jpeg_alloc_decoder_mem(stride * height * 3, &memory, &allocated);
    if (!pixels) return false;
    const jpeg_decode_engine_cfg_t engine = {.timeout_ms = 100};
    const jpeg_decode_cfg_t output = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB888,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_RGB,
    };
    jpeg_decoder_handle_t decoder;
    uint32_t written;
    bool ok = false;
    if (jpeg_new_decoder_engine(&engine, &decoder) == ESP_OK) {
        if (jpeg_decoder_process(decoder, &output, data, size, pixels, allocated, &written) == ESP_OK &&
            written == stride * height * 3) {
            artwork_thumbnail(logo, pixels, info.width, info.height, stride, 3);
            ok = true;
        }
        jpeg_del_decoder_engine(decoder);
    }
    free(pixels);
    return ok;
}

static bool artwork_decode(station_logo_t *logo, const uint8_t *data, size_t size)
{
    if (size >= 8 && !memcmp(data, "\x89PNG\r\n\x1a\n", 8)) return artwork_png(logo, data, size);
    if (size >= 3 && data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff)
        return artwork_jpeg(logo, data, size);
    return false;
}

static bool artwork_load(const artwork_job_t *job, const char *hash, station_logo_t *logo, bool *online)
{
    uint8_t *body = heap_caps_malloc(ARTWORK_MAX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) return false;
    const char *keys[] = {job->station.stationuuid, hash};
    bool found = false;
    for (unsigned i = 0; i < 2 && !found && artwork_current(job); i++) {
        if (!keys[i][0]) continue;
        size_t size = artwork_read(keys[i], "png", body);
        found = artwork_decode(logo, body, size);
        if (!found) {
            size = artwork_read(keys[i], "jpg", body);
            found = artwork_decode(logo, body, size);
        }
    }
    if (!found && job->online && job->station.stationuuid[0] && artwork_current(job)) {
        char url[STATIONS_MAX_URL_LENGTH + 1];
        if (radio_browser_lookup_logo(job->station.stationuuid, url, sizeof(url))) {
            size_t size = artwork_fetch(job, url, body);
            found = artwork_decode(logo, body, size);
            *online = found;
        }
    }
    free(body);
    return found;
}

static void artwork_worker(void *arg)
{
    (void)arg;
    artwork_job_t job;
    station_logo_t *logo = heap_caps_malloc(sizeof(*logo), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    cache = heap_caps_calloc(ARTWORK_CACHE_SIZE, sizeof(*cache), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    while (xQueueReceive(jobs, &job, portMAX_DELAY) == pdTRUE) {
        if (!logo || !job.station.url[0] || !artwork_current(&job)) continue;
        char hash[65] = {0};
        artwork_url_key(job.station.url, hash);
        const char *key = job.station.stationuuid[0] ? job.station.stationuuid : hash;
        bool online = false, found = false;
        for (unsigned i = 0; cache && *key && i < ARTWORK_CACHE_SIZE; i++) {
            if ((!cache[i].online || job.online) && !strcasecmp(cache[i].key, key)) {
                *logo = cache[i].logo;
                found = true;
                break;
            }
        }
        if (!found && artwork_load(&job, hash, logo, &online)) {
            found = true;
            if (cache && *key) {
                artwork_cache_t *entry = &cache[next_slot++ % ARTWORK_CACHE_SIZE];
                memcpy(entry->key, key, strlen(key) + 1);
                entry->online = online;
                entry->logo = *logo;
            }
        }
        if (found && artwork_current(&job)) {
            xSemaphoreTake(lock, portMAX_DELAY);
            *result = *logo;
            published = job.sequence;
            xSemaphoreGive(lock);
        }
    }
    vTaskDelete(NULL);
}

unsigned artwork_submit(const radio_station_t *station, bool online)
{
    if ((!station && !submitted.station.url[0]) ||
        (station && submitted.online == online &&
         !strcmp(station->url, submitted.station.url) &&
         !strcasecmp(station->stationuuid, submitted.station.stationuuid)))
        return submitted.sequence;

    ++submitted.sequence;
    submitted.online = online;
    if (station) submitted.station = *station;
    else submitted.station = (radio_station_t){0};
    atomic_store(&wanted, submitted.sequence);
    if (!jobs && station) {
        jobs = xQueueCreate(1, sizeof(artwork_job_t));
        lock = xSemaphoreCreateMutex();
        result = heap_caps_malloc(sizeof(*result), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!jobs || !lock || !result ||
            xTaskCreate(artwork_worker, "tanradio_artwork", 8192, NULL, 1, NULL) != pdPASS) {
            if (jobs) vQueueDelete(jobs);
            if (lock) vSemaphoreDelete(lock);
            free(result);
            jobs = NULL;
            lock = NULL;
            result = NULL;
        }
    }
    if (jobs) xQueueOverwrite(jobs, &submitted);
    return submitted.sequence;
}

bool artwork_copy(unsigned request, station_logo_t *logo)
{
    if (!lock || !request) return false;
    xSemaphoreTake(lock, portMAX_DELAY);
    bool found = request == published;
    if (found) *logo = *result;
    xSemaphoreGive(lock);
    return found;
}

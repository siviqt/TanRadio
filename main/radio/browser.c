#include "radio/browser.h"
#include "radio/browser_parse.h"
#include "network/network.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "lwip/sockets.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define SEARCH_BODY_LIMIT (96 * 1024)
#define REQUEST_DEADLINE_US (12LL * 1000000)
static const char *TAG = "tanradio.browser";

typedef struct {
    unsigned generation; /* Zero denotes a best-effort playback click. */
    char query[BROWSER_QUERY_LENGTH + 1];
    radio_browser_search_mode_t mode;
} browser_job_t;

static QueueHandle_t jobs;
static SemaphoreHandle_t mutex;
static radio_station_t *results;
static radio_browser_status_t status;
static unsigned generation = 1;
/* Only the browser worker accesses the mirror cache. */
static char mirrors[BROWSER_MIRROR_LIMIT][BROWSER_HOST_LENGTH];
static size_t mirror_count;
static int64_t mirrors_expire;

typedef enum {
    BROWSER_FAILURE_CONNECT,
    BROWSER_FAILURE_RESPONSE,
    BROWSER_FAILURE_SIZE,
} browser_failure_t;

static bool cancelled(unsigned request)
{
    if (!request) return false;
    xSemaphoreTake(mutex, portMAX_DELAY);
    bool obsolete = request != generation;
    xSemaphoreGive(mutex);
    return obsolete;
}

static unsigned next_generation(void)
{
    if (++generation == 0) ++generation;
    return generation;
}

static size_t discover_mirrors(unsigned request,
                               char hosts[BROWSER_MIRROR_LIMIT][BROWSER_HOST_LENGTH])
{
    /* ESP-IDF's resolver does not expose SRV lookup. Query the network's DNS
     * servers directly for the documented service, retaining DNS hostnames
     * for TLS certificate verification rather than connecting to bare IPs. */
    uint16_t id = (uint16_t)esp_random();
    uint8_t query[64] = {id >> 8, id & 255, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0};
    const char *label = "_api._tcp.radio-browser.info";
    size_t used = 12;
    while (*label) {
        const char *dot = strchr(label, '.');
        size_t n = dot ? (size_t)(dot - label) : strlen(label);
        query[used++] = n;
        memcpy(query + used, label, n);
        used += n;
        label += n;
        if (*label == '.') label++;
    }
    query[used++] = 0;
    query[used++] = 0; query[used++] = 33; /* SRV */
    query[used++] = 0; query[used++] = 1;  /* IN */
    uint8_t packet[1536];
    for (unsigned server = 0; server < 2 && !cancelled(request); server++) {
        esp_netif_dns_info_t dns = {0};
        esp_netif_t *netif = esp_netif_get_default_netif();
        if (!netif || esp_netif_get_dns_info(netif, server == 0 ? ESP_NETIF_DNS_MAIN :
                                            ESP_NETIF_DNS_BACKUP, &dns) != ESP_OK ||
            dns.ip.type != ESP_IPADDR_TYPE_V4 || dns.ip.u_addr.ip4.addr == 0) continue;
        int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (fd < 0) continue;
        struct timeval timeout = {.tv_sec = 2};
        if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
            close(fd);
            continue;
        }
        struct sockaddr_in destination = {
            .sin_family = AF_INET, .sin_port = htons(53),
            .sin_addr.s_addr = dns.ip.u_addr.ip4.addr,
        };
        ssize_t received = -1;
        if (connect(fd, (struct sockaddr *)&destination, sizeof(destination)) == 0 &&
            send(fd, query, used, 0) == used) {
            received = recv(fd, packet, sizeof(packet), 0);
        }
        close(fd);
        if (received > 0) {
            size_t count = browser_parse_srv(packet, received, id, hosts);
            if (count) {
                for (size_t i = count - 1; i > 0; i--) {
                    size_t other = esp_random() % (i + 1);
                    char swap[BROWSER_HOST_LENGTH];
                    memcpy(swap, hosts[i], sizeof(swap));
                    memcpy(hosts[i], hosts[other], sizeof(swap));
                    memcpy(hosts[other], swap, sizeof(swap));
                }
                ESP_LOGI(TAG, "Discovered %zu directory mirror(s)", count);
                return count;
            }
        }
    }
    return 0;
}

static bool request_json(const char *host, const char *path, unsigned request,
                          char *body, size_t capacity, browser_failure_t *failure)
{
    *failure = BROWSER_FAILURE_CONNECT;
    char url[640];
    if (snprintf(url, sizeof(url), "https://%s%s", host, path) >= sizeof(url)) return false;
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 5000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = "TanRadio/0.1 (Radio Browser)",
        .disable_auto_redirect = true,
        .buffer_size = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return false;
    int64_t deadline = esp_timer_get_time() + REQUEST_DEADLINE_US;
    bool ok = false;
    size_t used = 0;
    if (cancelled(request)) goto done;
    esp_err_t opened = esp_http_client_open(client, 0);
    if (opened != ESP_OK) {
        int tls_error = 0, tls_flags = 0;
        esp_http_client_get_and_clear_last_tls_error(client, &tls_error, &tls_flags);
        ESP_LOGW(TAG, "Directory %s connection failed: %d, TLS=%d flags=0x%x",
                 host, (int)opened, tls_error, tls_flags);
        goto done;
    }
    *failure = BROWSER_FAILURE_RESPONSE;
    int64_t length = esp_http_client_fetch_headers(client);
    int http_status = esp_http_client_get_status_code(client);
    if (length >= 0 && (uint64_t)length >= capacity) {
        *failure = BROWSER_FAILURE_SIZE;
        ESP_LOGW(TAG, "Directory %s response exceeds %zu bytes", host, capacity - 1);
        goto done;
    }
    if (length < 0 || http_status != 200) {
        ESP_LOGW(TAG, "Directory %s HTTP %d, length=%lld", host, http_status, (long long)length);
        goto done;
    }
    while (!cancelled(request) && esp_timer_get_time() < deadline) {
        if (used == capacity - 1) {
            ok = esp_http_client_is_complete_data_received(client);
            if (!ok) *failure = BROWSER_FAILURE_SIZE;
            break;
        }
        size_t chunk = capacity - 1 - used;
        if (chunk > 1024) chunk = 1024;
        /* Return to cancellation/deadline checks between small reads rather
         * than asking the blocking client to fill the entire response. */
        int bytes = esp_http_client_read(client, body + used, (int)chunk);
        if (bytes < 0) break;
        if (bytes == 0) {
            ok = esp_http_client_is_complete_data_received(client);
            break;
        }
        used += bytes;
        if (esp_http_client_is_complete_data_received(client)) {
            ok = true;
            break;
        }
    }
done:
    body[used] = 0;
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok && !cancelled(request);
}

static void finish_search(unsigned request, const radio_station_t *found,
                           size_t count, const char *message)
{
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (request == generation) {
        if (found && count) memcpy(results, found, count * sizeof(*results));
        status.count = count;
        status.busy = false;
        status.revision++;
        snprintf(status.message, sizeof(status.message), "%s", message);
    }
    xSemaphoreGive(mutex);
}

static void browser_worker(void *arg)
{
    (void)arg;
    browser_job_t job;
    while (xQueueReceive(jobs, &job, portMAX_DELAY) == pdTRUE) {
        if (cancelled(job.generation)) continue;
        if (!network_is_connected()) {
            if (job.generation) finish_search(job.generation, NULL, 0, "Connect to Wi-Fi, then press Enter to retry.");
            continue;
        }
        if (!mirror_count || esp_timer_get_time() >= mirrors_expire) {
            mirror_count = discover_mirrors(job.generation, mirrors);
            mirrors_expire = esp_timer_get_time() + 30LL * 60 * 1000000;
        }
        if (!mirror_count) {
            if (job.generation) finish_search(job.generation, NULL, 0, "Directory discovery failed. Press Enter to retry.");
            continue;
        }
        char encoded[BROWSER_QUERY_LENGTH * 3 + 1];
        char path[480];
        if (job.generation) {
            if (!browser_encode_query(job.query, encoded, sizeof(encoded))) continue;
            const char *field = "name";
            if (job.mode == BROWSER_SEARCH_COUNTRY) {
                field = "country";
                if (strlen(job.query) == 2 &&
                    isalpha((unsigned char)job.query[0]) && isalpha((unsigned char)job.query[1])) {
                    field = "countrycode";
                    encoded[0] = (char)toupper((unsigned char)job.query[0]);
                    encoded[1] = (char)toupper((unsigned char)job.query[1]);
                    encoded[2] = 0;
                }
            }
            snprintf(path, sizeof(path), "/json/stations/search?%s=%s&codec=MP3&hidebroken=true"
                     "&limit=%d&order=clickcount&reverse=true", field, encoded, BROWSER_RESULT_LIMIT);
        } else {
            snprintf(path, sizeof(path), "/json/url/%s", job.query);
        }
        size_t capacity = job.generation ? SEARCH_BODY_LIMIT + 1 : 2048;
        char *body = heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        radio_station_t *found = job.generation ? heap_caps_calloc(
            BROWSER_RESULT_LIMIT, sizeof(*found), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
        bool success = false;
        size_t count = 0;
        browser_failure_t failure = BROWSER_FAILURE_RESPONSE;
        if (body && (!job.generation || found)) {
            for (size_t i = 0; i < mirror_count && i < 3 && !cancelled(job.generation); i++) {
                bool received = request_json(mirrors[i], path, job.generation, body, capacity, &failure);
                /* Discovery can contain only one server. Give a transient
                 * reset one bounded retry instead of requiring another Enter.
                 * Click notifications are best effort and are never retried. */
                if (!received && job.generation && mirror_count == 1 &&
                    failure == BROWSER_FAILURE_CONNECT && !cancelled(job.generation)) {
                    ESP_LOGI(TAG, "Retrying single directory mirror after connection failure");
                    vTaskDelay(pdMS_TO_TICKS(200));
                    if (!cancelled(job.generation)) {
                        received = request_json(mirrors[i], path, job.generation, body, capacity, &failure);
                    }
                }
                if (!received) continue;
                if (!job.generation || browser_parse_stations(body, found, BROWSER_RESULT_LIMIT, &count)) {
                    success = true;
                    break;
                }
                failure = BROWSER_FAILURE_RESPONSE;
                ESP_LOGW(TAG, "Directory %s returned invalid station JSON", mirrors[i]);
            }
        }
        if (job.generation) {
            const char *message = !body || !found ? "Not enough memory. Try again after stopping playback." :
                !success ? (failure == BROWSER_FAILURE_CONNECT ?
                    "Directory connection failed. Press Enter to retry." :
                    failure == BROWSER_FAILURE_SIZE ?
                    "Directory response too large. Refine the search." :
                    "Directory response failed. Press Enter to retry.") :
                !count ? "No compatible stations found. Try another name or country." :
                "MP3 results (up to 20). Type to change your search.";
            finish_search(job.generation, found, success ? count : 0, message);
        }
        if (!success && !cancelled(job.generation)) mirror_count = 0;
        free(found);
        free(body);
    }
    vTaskDelete(NULL);
}

void radio_browser_init(void)
{
    if (jobs) return;
    mutex = xSemaphoreCreateMutex();
    results = heap_caps_calloc(BROWSER_RESULT_LIMIT, sizeof(*results),
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    QueueHandle_t queue = xQueueCreate(8, sizeof(browser_job_t));
    if (!mutex || !results || !queue) {
        if (mutex) vSemaphoreDelete(mutex);
        if (queue) vQueueDelete(queue);
        free(results);
        mutex = NULL;
        results = NULL;
        return;
    }
    jobs = queue;
    if (xTaskCreate(browser_worker, "tanradio_browser", 8192, NULL, 2, NULL) != pdPASS) {
        vQueueDelete(jobs);
        jobs = NULL;
        vSemaphoreDelete(mutex);
        mutex = NULL;
        free(results);
        results = NULL;
        ESP_LOGE(TAG, "Unable to create directory worker");
    }
}

bool radio_browser_search(const char *query)
{
    return radio_browser_search_by(query, BROWSER_SEARCH_NAME);
}

bool radio_browser_search_by(const char *query, radio_browser_search_mode_t mode)
{
    if (mode != BROWSER_SEARCH_NAME && mode != BROWSER_SEARCH_COUNTRY) return false;
    if (!jobs || !query) return false;
    while (*query && isspace((unsigned char)*query)) query++;
    size_t length = strlen(query);
    while (length && isspace((unsigned char)query[length - 1])) length--;
    if (!length || length > BROWSER_QUERY_LENGTH) return false;
    browser_job_t job = {.mode = mode};
    memcpy(job.query, query, length);
    xSemaphoreTake(mutex, portMAX_DELAY);
    job.generation = next_generation();
    status.busy = true;
    status.count = 0;
    status.revision++;
    snprintf(status.message, sizeof(status.message), "Searching Radio Browser...");
    xSemaphoreGive(mutex);
    if (xQueueSendToFront(jobs, &job, 0) == pdTRUE) return true;
    finish_search(job.generation, NULL, 0, "Directory is busy. Press Enter to retry.");
    return false;
}

void radio_browser_cancel(void)
{
    if (!mutex) return;
    xSemaphoreTake(mutex, portMAX_DELAY);
    next_generation();
    status.busy = false;
    status.count = 0;
    status.revision++;
    snprintf(status.message, sizeof(status.message), "Type a name or country; Tab changes mode.");
    xSemaphoreGive(mutex);
}

void radio_browser_status(radio_browser_status_t *out)
{
    if (!mutex) {
        *out = (radio_browser_status_t){0};
        snprintf(out->message, sizeof(out->message), "Station search is unavailable. Restart TanRadio to retry.");
        return;
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    *out = status;
    xSemaphoreGive(mutex);
}

bool radio_browser_result(size_t index, radio_station_t *station)
{
    if (!mutex || !station) return false;
    xSemaphoreTake(mutex, portMAX_DELAY);
    bool valid = index < status.count && !status.busy;
    if (valid) *station = results[index];
    xSemaphoreGive(mutex);
    return valid;
}

void radio_browser_count_play(const char *uuid)
{
    if (!jobs || !stations_uuid_valid(uuid)) return;
    browser_job_t job = {0};
    snprintf(job.query, sizeof(job.query), "%s", uuid);
    xQueueSendToBack(jobs, &job, 0);
}

/* Called only by the artwork worker. Keep its mirror cache separate from search. */
bool radio_browser_lookup_logo(const char *uuid, char *url, size_t capacity)
{
    if (!stations_uuid_valid(uuid) || !network_is_connected()) return false;
    static char hosts[BROWSER_MIRROR_LIMIT][BROWSER_HOST_LENGTH];
    static size_t count;
    static int64_t expires;
    if (!count || esp_timer_get_time() >= expires) {
        count = discover_mirrors(0, hosts);
        expires = esp_timer_get_time() + 30LL * 60 * 1000000;
    }
    if (!count) return false;
    char path[80];
    snprintf(path, sizeof(path), "/json/stations/byuuid/%s", uuid);
    const size_t body_size = 8192;
    char *body = heap_caps_malloc(body_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) return false;
    browser_failure_t failure;
    bool received = request_json(hosts[0], path, 0, body, body_size, &failure);
    bool found = received && browser_parse_logo(body, uuid, url, capacity);
    if (!received) count = 0;
    free(body);
    return found;
}

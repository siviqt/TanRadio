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
#define BROWSER_CACHE_PAGES 3
#define BROWSER_PREFETCH_ROWS 3
static const char *TAG = "tanradio.browser";

typedef struct {
    unsigned generation; /* Zero denotes a best-effort playback click. */
    char query[BROWSER_QUERY_LENGTH + 1];
    radio_browser_search_mode_t mode;
    size_t offset;
    int direction;
    bool retry;
} browser_job_t;

typedef struct {
    size_t offset;
    size_t count;
    radio_station_t rows[BROWSER_PAGE_SIZE];
} browser_page_t;

static QueueHandle_t jobs;
static SemaphoreHandle_t mutex;
static browser_page_t *results;
/* Viewport and cached pages are published together under the browser mutex. */
static browser_page_t *pages[BROWSER_CACHE_PAGES];
static size_t page_count, selection, first_offset, next_offset;
static bool exhausted, step_after_load;
static browser_job_t active_search, pending_page;
static radio_browser_status_t status;
static unsigned generation = 1;
/* Only the browser worker accesses the mirror cache. */
static char mirrors[BROWSER_MIRROR_LIMIT][BROWSER_HOST_LENGTH];
static size_t mirror_count;
static int64_t mirrors_expire;
static char search_mirror[BROWSER_HOST_LENGTH];
static unsigned mirror_generation;

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

static void search_message_locked(const char *message)
{
    status.revision++;
    snprintf(status.message, sizeof(status.message), "%s", message);
}

static void search_failure_locked(const char *message)
{
    status.busy = false;
    status.can_retry = true;
    search_message_locked(message);
}

static void fail_page(unsigned request, const char *message)
{
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (request == generation) search_failure_locked(message);
    xSemaphoreGive(mutex);
}

static void request_new_mirror(unsigned request)
{
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (request == generation) {
        status.busy = status.can_retry = false;
        search_message_locked("Directory mirror unavailable. Press Enter to start a new search.");
    }
    xSemaphoreGive(mutex);
}

static void clear_pages_locked(void)
{
    for (size_t i = 0; i < BROWSER_CACHE_PAGES; i++) pages[i] = results + i;
    page_count = selection = first_offset = next_offset = status.count = 0;
    exhausted = step_after_load = status.busy = status.can_retry = false;
}

static bool queue_page_locked(browser_job_t job, bool priority)
{
    pending_page = job;
    status.busy = true;
    status.can_retry = false;
    search_message_locked(!status.count ? "Searching Radio Browser..." :
                          job.direction < 0 ? "Loading earlier stations..." : "Loading more stations...");
    BaseType_t queued = priority ? xQueueSendToFront(jobs, &job, 0) : xQueueSendToBack(jobs, &job, 0);
    if (queued == pdTRUE) return true;
    search_failure_locked("Directory is busy. Press Enter to retry.");
    return false;
}

static void ready_message_locked(void)
{
    search_message_locked(status.count ? "" : "No compatible stations found.");
}

static bool can_cache_page_locked(int direction)
{
    if (page_count < BROWSER_CACHE_PAGES) return true;
    return direction < 0 ? selection < status.count - pages[page_count - 1]->count :
                           selection >= pages[0]->count;
}

static const radio_station_t *result_locked(size_t index)
{
    for (size_t i = 0; i < page_count; i++) {
        if (index < pages[i]->count) return &pages[i]->rows[index];
        index -= pages[i]->count;
    }
    return NULL;
}

static bool already_cached_locked(const radio_station_t *station)
{
    for (size_t i = 0; i < page_count; i++) {
        for (size_t row = 0; row < pages[i]->count; row++) {
            if (stations_same(station, &pages[i]->rows[row])) return true;
        }
    }
    return false;
}

static void evict_page_locked(bool before)
{
    if (before) {
        browser_page_t *first = pages[0];
        selection -= first->count;
        status.count -= first->count;
        --page_count;
        memmove(pages, pages + 1, page_count * sizeof(*pages));
        pages[page_count] = first;
        first_offset = pages[0]->offset;
    } else {
        browser_page_t *last = pages[--page_count];
        status.count -= last->count;
        next_offset = last->offset;
        exhausted = false;
    }
}

static void publish_page(const browser_job_t *job, radio_station_t *found,
                         size_t count, size_t raw_count)
{
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (job->generation != generation) goto done;
    status.busy = false;
    bool before = job->direction < 0;
    size_t accepted = 0;
    for (size_t i = 0; i < count; i++) {
        if (already_cached_locked(&found[i])) continue;
        if (accepted != i) found[accepted] = found[i];
        accepted++;
    }
    /* The user may have moved into the page that would be evicted while an
     * optional prefetch was in flight. Keep the selection and fetch again later. */
    if (accepted && !can_cache_page_locked(job->direction)) {
        step_after_load = false;
        ready_message_locked();
        goto done;
    }
    if (before) {
        first_offset = job->offset;
    } else {
        next_offset = job->offset + raw_count;
        exhausted = raw_count < BROWSER_PAGE_SIZE;
    }
    if (accepted) {
        if (page_count == BROWSER_CACHE_PAGES) evict_page_locked(!before);
        browser_page_t *page = pages[page_count];
        page->offset = job->offset;
        page->count = accepted;
        memcpy(page->rows, found, accepted * sizeof(*found));
        if (before) {
            memmove(pages + 1, pages, page_count * sizeof(*pages));
            pages[0] = page;
            selection += accepted;
            if (step_after_load && selection == accepted) selection--;
        } else if (step_after_load && status.count && selection + 1 == status.count) {
            selection++;
        }
        page_count++;
        status.count += accepted;
    } else if (before ? first_offset != 0 : !exhausted) {
        browser_job_t next = *job;
        next.direction = before ? -1 : 1;
        next.offset = before ? first_offset - BROWSER_PAGE_SIZE : next_offset;
        next.retry = false;
        queue_page_locked(next, false);
        goto done;
    }
    step_after_load = false;
    ready_message_locked();
done:
    xSemaphoreGive(mutex);
}

static void browser_worker(void *arg)
{
    (void)arg;
    browser_job_t job;
    while (xQueueReceive(jobs, &job, portMAX_DELAY) == pdTRUE) {
        if (cancelled(job.generation)) continue;
        if (!network_is_connected()) {
            if (job.generation) fail_page(job.generation, "Connect to Wi-Fi, then press Enter to retry.");
            continue;
        }
        bool pinned = job.generation && job.generation == mirror_generation;
        if (!pinned && (!mirror_count || esp_timer_get_time() >= mirrors_expire)) {
            mirror_count = discover_mirrors(job.generation, mirrors);
            mirrors_expire = esp_timer_get_time() + 30LL * 60 * 1000000;
        }
        if (!pinned && !mirror_count) {
            if (job.generation) fail_page(job.generation, "Directory discovery failed. Press Enter to retry.");
            continue;
        }
        char encoded[BROWSER_QUERY_LENGTH * 3 + 1];
        char path[480];
        if (job.generation) {
            if (!browser_encode_query(job.query, encoded, sizeof(encoded))) {
                fail_page(job.generation, "Search term is too long.");
                continue;
            }
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
            snprintf(path, sizeof(path), "/json/stations/search?%s=%s&hidebroken=true"
                     "&limit=%d&offset=%zu&order=name", field, encoded, BROWSER_PAGE_SIZE, job.offset);
        } else {
            snprintf(path, sizeof(path), "/json/url/%s", job.query);
        }
        size_t capacity = job.generation ? SEARCH_BODY_LIMIT + 1 : 2048;
        char *body = heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        radio_station_t *found = job.generation ? heap_caps_calloc(
            BROWSER_PAGE_SIZE, sizeof(*found), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
        bool success = false;
        size_t count = 0, raw_count = 0;
        browser_failure_t failure = BROWSER_FAILURE_RESPONSE;
        if (body && (!job.generation || found)) {
            size_t attempts = pinned ? 1 : mirror_count;
            for (size_t i = 0; i < attempts && i < 3 && !cancelled(job.generation); i++) {
                const char *host = pinned ? search_mirror : mirrors[i];
                bool received = request_json(host, path, job.generation, body, capacity, &failure);
                /* Discovery can contain only one server. Give a transient
                 * reset one bounded retry instead of requiring another Enter.
                 * Click notifications are best effort and are never retried. */
                if (!received && job.generation && attempts == 1 &&
                    failure == BROWSER_FAILURE_CONNECT && !cancelled(job.generation)) {
                    ESP_LOGI(TAG, "Retrying single directory mirror after connection failure");
                    vTaskDelay(pdMS_TO_TICKS(200));
                    if (!cancelled(job.generation)) {
                        received = request_json(host, path, job.generation, body, capacity, &failure);
                    }
                }
                if (!received) continue;
                if (!job.generation ||
                    (browser_parse_stations(body, found, BROWSER_PAGE_SIZE, &count, &raw_count) &&
                     raw_count <= BROWSER_PAGE_SIZE && job.offset <= SIZE_MAX - raw_count)) {
                    if (job.generation && !pinned) {
                        snprintf(search_mirror, sizeof(search_mirror), "%s", host);
                        mirror_generation = job.generation;
                    }
                    success = true;
                    break;
                }
                failure = BROWSER_FAILURE_RESPONSE;
                ESP_LOGW(TAG, "Directory %s returned an invalid station page", host);
            }
        }
        if (job.generation && success) {
            publish_page(&job, found, count, raw_count);
        } else if (pinned && job.retry && failure == BROWSER_FAILURE_CONNECT) {
            request_new_mirror(job.generation);
        } else if (job.generation) {
            const char *message = !body || !found ? "Not enough memory. Try again after stopping playback." :
                failure == BROWSER_FAILURE_CONNECT ? "Directory connection failed. Press Enter to retry." :
                failure == BROWSER_FAILURE_SIZE ? "Directory response too large. Refine the search." :
                "Directory response failed. Press Enter to retry.";
            fail_page(job.generation, message);
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
    results = heap_caps_calloc(BROWSER_CACHE_PAGES, sizeof(*results),
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
    clear_pages_locked();
    active_search = job;
    bool queued = queue_page_locked(job, true);
    xSemaphoreGive(mutex);
    return queued;
}

void radio_browser_cancel(void)
{
    if (!mutex) return;
    xSemaphoreTake(mutex, portMAX_DELAY);
    next_generation();
    clear_pages_locked();
    search_message_locked("Type a name or country; Tab changes mode.");
    xSemaphoreGive(mutex);
}

static void snapshot_status_locked(radio_browser_status_t *out)
{
    *out = status;
    out->at_start = status.count > 0 && first_offset == 0 && selection == 0;
    out->at_end = status.count > 0 && exhausted && selection == status.count - 1;
}

void radio_browser_status(radio_browser_status_t *out)
{
    if (!mutex) {
        *out = (radio_browser_status_t){0};
        snprintf(out->message, sizeof(out->message), "Station search is unavailable. Restart TanRadio to retry.");
        return;
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    snapshot_status_locked(out);
    xSemaphoreGive(mutex);
}

bool radio_browser_result(size_t index, radio_station_t *station)
{
    if (!mutex || !station) return false;
    xSemaphoreTake(mutex, portMAX_DELAY);
    const radio_station_t *result = result_locked(index);
    if (result) *station = *result;
    xSemaphoreGive(mutex);
    return result != NULL;
}

bool radio_browser_selected(radio_station_t *station)
{
    if (!mutex || !station) return false;
    xSemaphoreTake(mutex, portMAX_DELAY);
    const radio_station_t *result = result_locked(selection);
    if (result) *station = *result;
    xSemaphoreGive(mutex);
    return result != NULL;
}

size_t radio_browser_window(radio_browser_status_t *out, radio_station_t *rows,
                            size_t capacity, size_t *selected_row)
{
    *selected_row = SIZE_MAX;
    if (!mutex) {
        radio_browser_status(out);
        return 0;
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    snapshot_status_locked(out);
    size_t count = status.count < capacity ? status.count : capacity;
    if (count) {
        size_t first = selection > capacity / 2 ? selection - capacity / 2 : 0;
        if (first > status.count - count) first = status.count - count;
        *selected_row = selection - first;
        for (size_t i = 0; i < count; i++) rows[i] = *result_locked(first + i);
    }
    xSemaphoreGive(mutex);
    return count;
}

void radio_browser_move(int direction)
{
    if (!mutex || !direction) return;
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (!status.count) goto done;
    bool before = direction < 0;
    bool at_edge = before ? selection == 0 : selection + 1 == status.count;
    if (!at_edge) selection = before ? selection - 1 : selection + 1;
    if (status.busy) {
        if (at_edge && pending_page.direction == direction) step_after_load = true;
    } else if (!status.can_retry) {
        size_t distance = before ? selection : status.count - 1 - selection;
        if (distance < BROWSER_PREFETCH_ROWS && (before ? first_offset != 0 : !exhausted) &&
            can_cache_page_locked(direction)) {
            browser_job_t job = active_search;
            job.direction = direction;
            job.offset = before ? first_offset - BROWSER_PAGE_SIZE : next_offset;
            step_after_load = at_edge;
            queue_page_locked(job, true);
        } else {
            ready_message_locked();
        }
    }
done:
    xSemaphoreGive(mutex);
}

bool radio_browser_retry(void)
{
    if (!mutex) return false;
    xSemaphoreTake(mutex, portMAX_DELAY);
    bool retry = status.can_retry && !status.busy;
    if (retry) {
        browser_job_t job = pending_page;
        job.retry = true;
        queue_page_locked(job, true);
    }
    xSemaphoreGive(mutex);
    return retry;
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

#!/usr/bin/env python3
"""Run production directory parsers and SD catalog code on the host.

Requires cc and the cJSON source fetched by `make build`. Uses temporary files;
rename/fsync fault injection checks that failed saves preserve the old catalog.
"""
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
CJSON = ROOT / "managed_components/espressif__cjson/cJSON"

STUBS = r'''
#pragma once
#include <stdlib.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_INVALID_ARG 2
#define ESP_ERR_INVALID_STATE 3
#define ESP_ERR_NOT_FOUND 4
#define ESP_ERR_INVALID_SIZE 5
#define ESP_LOGE(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGI(tag, ...) ((void)(tag))
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define heap_caps_calloc(n, s, caps) calloc(n, s)
'''

HARNESS = r'''
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "radio/stations.h"
#include "radio/browser_parse.h"

static int fail_rename, renames, fail_sync;
int __real_rename(const char *, const char *);
int __wrap_rename(const char *from, const char *to) {
    if (++renames == fail_rename) { errno = EIO; return -1; }
    return __real_rename(from, to);
}
int __real_fsync(int);
int __wrap_fsync(int fd) {
    if (fail_sync) { errno = EIO; return -1; }
    return __real_fsync(fd);
}
static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb"); assert(f);
    assert(fseek(f, 0, SEEK_END) == 0);
    long length = ftell(f); assert(length >= 0);
    rewind(f);
    char *data = calloc(length + 1, 1); assert(data);
    assert(fread(data, 1, length, f) == (size_t)length);
    assert(fclose(f) == 0);
    return data;
}
static void write_file(const char *path, const char *data) {
    FILE *f = fopen(path, "wb"); assert(f);
    assert(fwrite(data, 1, strlen(data), f) == strlen(data));
    assert(fclose(f) == 0);
}
static void check_unchanged(const char *before) {
    char *after = read_file("stations.txt");
    assert(!strcmp(before, after)); free(after);
}
static void put16(unsigned char *p, unsigned n) { p[0] = n >> 8; p[1] = n; }

int main(void) {
    radio_station_t records[20]; size_t count;
    char *data = read_file("mixed.json");
    assert(browser_parse_stations(data, records, 20, &count) && count == 2);
    assert(!strcmp(records[0].name, "Good Station"));
    assert(!strcmp(records[0].url, "https://example.org/live.mp3?x=1&y=2"));
    assert(records[0].bitrate_kbps == 128 && !strcmp(records[1].country, ""));
    free(data);
    char logo[64];
    const char *uuid = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
    const char *logos = "[{\"stationuuid\":\"bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb\","
        "\"favicon\":\"https://example.org/wrong.png\"},"
        "{\"stationuuid\":\"AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA\","
        "\"favicon\":\"https://example.org/right.png\"}]";
    assert(browser_parse_logo(logos, uuid, logo, sizeof(logo)));
    assert(!strcmp(logo, "https://example.org/right.png"));
    assert(!browser_parse_logo(logos, uuid, logo, 8));
    assert(!browser_parse_logo(logos, "cccccccc-cccc-cccc-cccc-cccccccccccc", logo, sizeof(logo)));
    assert(!browser_parse_logo("[{\"stationuuid\":\"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa\","
        "\"favicon\":\"http://example.org/insecure.png\"}]", uuid, logo, sizeof(logo)));
    assert(!browser_parse_stations("[{}", records, 20, &count));
    assert(!browser_parse_stations("{}", records, 20, &count));
    assert(!browser_parse_stations("[]garbage", records, 20, &count));
    assert(browser_parse_stations("[]", records, 20, &count) && count == 0);
    char deep[80]; memset(deep, '[', 32); memset(deep + 32, ']', 32); deep[64] = 0;
    assert(!browser_parse_stations(deep, records, 20, &count));
    char encoded[64];
    assert(browser_encode_query("A&B + Радио", encoded, sizeof(encoded)));
    assert(!strcmp(encoded, "A%26B%20%2B%20%D0%A0%D0%B0%D0%B4%D0%B8%D0%BE"));
    assert(!browser_encode_query("&&", encoded, 6));

    /* A real compressed SRV answer plus malformed IDs, lengths and loops. */
    unsigned char packet[128] = {0};
    put16(packet, 0x1234); put16(packet + 2, 0x8180);
    put16(packet + 4, 1); put16(packet + 6, 1);
    unsigned char question[] = {4,'_','a','p','i',4,'_','t','c','p',13,'r','a','d','i','o','-','b','r','o','w','s','e','r',4,'i','n','f','o',0,0,33,0,1};
    memcpy(packet + 12, question, sizeof(question));
    size_t at = 12 + sizeof(question), answer = at;
    packet[at++] = 0xc0; packet[at++] = 12;
    put16(packet + at, 33); at += 2; put16(packet + at, 1); at += 2;
    at += 4; put16(packet + at, 16); at += 2;
    at += 4; put16(packet + at, 80); at += 2;
    unsigned char host[] = {3,'d','e','1',3,'a','p','i',0xc0,22};
    memcpy(packet + at, host, sizeof(host)); at += sizeof(host);
    char hosts[BROWSER_MIRROR_LIMIT][BROWSER_HOST_LENGTH];
    assert(browser_parse_srv(packet, at, 0x1234, hosts) == 1);
    assert(!strcmp(hosts[0], "de1.api.radio-browser.info"));
    assert(browser_parse_srv(packet, at, 0x4321, hosts) == 0);
    for (size_t n = 0; n < at; n++) assert(browser_parse_srv(packet, n, 0x1234, hosts) == 0);
    packet[answer] = 0xc0; packet[answer + 1] = answer;
    assert(browser_parse_srv(packet, at, 0x1234, hosts) == 0);
    /* Random/malformed packets must never escape bounds or loop forever. */
    unsigned seed = 1234;
    for (unsigned i = 0; i < 3000; i++) {
        for (size_t n = 0; n < sizeof(packet); n++) {
            seed = seed * 1664525 + 1013904223; packet[n] = seed >> 24;
        }
        browser_parse_srv(packet, i % sizeof(packet), (packet[0] << 8) | packet[1], hosts);
    }

    /* Fresh installs contain no presets. Rechecking an existing file must
     * preserve user data, and the first save must work on the empty catalog. */
    assert(stations_ensure_file("fresh/stations.txt") == ESP_OK);
    data = read_file("fresh/stations.txt");
    assert(data[0] == '\0'); free(data);
    assert(stations_load_file("fresh/stations.txt") == ESP_ERR_NOT_FOUND);
    assert(stations_count() == 0);
    data = read_file("mixed.json");
    assert(browser_parse_stations(data, records, 20, &count)); free(data);
    size_t first_index; bool first_duplicate;
    assert(stations_save("fresh/stations.txt", &records[0], &first_index,
                         &first_duplicate) == ESP_OK);
    assert(first_index == 0 && !first_duplicate);
    data = read_file("fresh/stations.txt");
    assert(stations_ensure_file("fresh/stations.txt") == ESP_OK);
    char *preserved = read_file("fresh/stations.txt");
    assert(!strcmp(data, preserved)); free(data); free(preserved);
    assert(stations_load_file("fresh/stations.txt") == ESP_OK && stations_count() == 1);

    const char *original = "# user comment\nExisting:::http://example.org/old:::BG:::MP3\nmalformed user line";
    write_file("stations.txt", original);
    assert(stations_load_file("stations.txt") == ESP_OK && stations_count() == 1);
    const radio_station_t *stable = stations_get(0);
    data = read_file("mixed.json");
    assert(browser_parse_stations(data, records, 20, &count)); free(data);
    radio_station_t station = records[0];
    size_t index; bool duplicate;
    renames = 0; fail_rename = 2;
    assert(stations_save("stations.txt", &station, &index, &duplicate) != ESP_OK);
    check_unchanged(original); assert(stations_count() == 1);
    renames = 0; fail_rename = 1;
    assert(stations_save("stations.txt", &station, &index, &duplicate) != ESP_OK);
    check_unchanged(original);
    fail_rename = 0; fail_sync = 1;
    assert(stations_save("stations.txt", &station, &index, &duplicate) != ESP_OK);
    check_unchanged(original); fail_sync = 0;
    assert(stations_save("stations.txt", &station, &index, &duplicate) == ESP_OK);
    assert(!duplicate && index == 1 && stations_count() == 2);
    assert(stations_get(0) == stable && !strcmp(stable->name, "Existing"));
    data = read_file("stations.txt");
    assert(!strncmp(data, original, strlen(original)));
    assert(strstr(data, "#RADIOBROWSERUUID:01234567-89ab-cdef-0123-456789abcdef\nGood Station:::"));
    assert(stations_save("stations.txt", &station, &index, &duplicate) == ESP_OK && duplicate && index == 1);
    check_unchanged(data); free(data);
    assert(stations_load_file("stations.txt") == ESP_OK && stations_count() == 2);
    assert(!strcmp(stations_get(1)->stationuuid, station.stationuuid));
    strcpy(station.name, "Bad:::name");
    assert(stations_save("stations.txt", &station, &index, &duplicate) == ESP_ERR_INVALID_ARG);
    strcpy(station.name, "Bad\nname"); assert(!stations_valid(&station));
    strcpy(station.name, "#not a record"); assert(!stations_valid(&station));

    /* Boot recovery selects the old catalog if a FAT rename was interrupted. */
    assert(rename("stations.txt", "stations.txt.bak") == 0);
    assert(stations_ensure_file("stations.txt") == ESP_OK);
    assert(stations_load_file("stations.txt") == ESP_OK && stations_count() == 2);
    write_file("empty.txt", "# no stations yet\n");
    assert(stations_load_file("empty.txt") == ESP_ERR_NOT_FOUND);
    strcpy(station.name, "New station");
    assert(stations_save("empty.txt", &station, &index, &duplicate) == ESP_OK && index == 0);
    assert(stations_load_file("empty.txt") == ESP_OK && stations_count() == 1);

    FILE *f = fopen("full.txt", "wb"); assert(f);
    for (unsigned i = 0; i < 128; i++) fprintf(f, "Station %u:::http://example.org/%u:::BG:::MP3:::128\n", i, i);
    assert(fclose(f) == 0 && stations_load_file("full.txt") == ESP_OK);
    assert(stations_save("full.txt", &station, &index, &duplicate) == ESP_ERR_NO_MEM);
    f = fopen("large.txt", "wb"); assert(f);
    for (unsigned i = 0; i < 65536; i++) fputc('\n', f);
    assert(fclose(f) == 0 && stations_load_file("large.txt") == ESP_ERR_NOT_FOUND);
    assert(stations_save("large.txt", &station, &index, &duplicate) == ESP_ERR_INVALID_SIZE);
    /* Remove exactly one saved row and its UUID metadata; retain all other
     * bytes, including CRLF, malformed lines and comments between UUIDs. */
    const char *removal = "# header\r\n#RADIOBROWSERUUID:aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa\n"
        "# user note\r\n#RADIOBROWSERUUID:bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb\n\n"
        "Remove me:::http://example.org/remove:::BG:::MP3:::128\r\n"
        "# next note\nmalformed user line\nKeep me:::http://example.org/keep:::DE:::MP3";
    const char *kept = "# header\r\n# user note\r\n\n# next note\nmalformed user line\n"
        "Keep me:::http://example.org/keep:::DE:::MP3";
    write_file("stations.txt", removal);
    assert(stations_load_file("stations.txt") == ESP_OK && stations_count() == 2);
    radio_station_t owned = *stations_get(0);
    assert(stations_remove("stations.txt", 2) == ESP_ERR_INVALID_ARG);
    for (unsigned failure = 0; failure < 3; failure++) {
        renames = 0; fail_rename = failure < 2 ? failure + 1 : 0;
        fail_sync = failure == 2;
        assert(stations_remove("stations.txt", 0) != ESP_OK);
        check_unchanged(removal);
        assert(stations_count() == 2 && stations_same(stations_get(0), &owned));
    }
    fail_sync = fail_rename = 0;
    assert(stations_remove("stations.txt", 0) == ESP_OK);
    check_unchanged(kept);
    assert(stations_count() == 1 && !strcmp(stations_get(0)->name, "Keep me"));
    assert(!strcmp(owned.name, "Remove me") && !strcmp(owned.url, "http://example.org/remove"));
    assert(stations_load_file("stations.txt") == ESP_OK && stations_count() == 1);
    assert(!stations_get(0)->stationuuid[0]); /* Removed metadata cannot leak. */
    assert(stations_remove("stations.txt", 0) == ESP_OK && stations_count() == 0);
    check_unchanged("# header\r\n# user note\r\n\n# next note\nmalformed user line\n");
    assert(stations_load_file("stations.txt") == ESP_ERR_NOT_FOUND);
    for (unsigned cycle = 0; cycle < 150; cycle++) {
        assert(stations_save("stations.txt", &owned, &index, &duplicate) == ESP_OK);
        assert(index == 0 && !duplicate && stations_count() == 1);
        assert(stations_remove("stations.txt", 0) == ESP_OK && stations_count() == 0);
    }
    /* Identical legacy rows are separate entries; only the selected one goes. */
    write_file("stations.txt", "Same:::http://example.org/same:::BG:::MP3\n"
               "# keep this comment\nSame:::http://example.org/same:::BG:::MP3\n");
    assert(stations_load_file("stations.txt") == ESP_OK && stations_count() == 2);
    assert(stations_remove("stations.txt", 1) == ESP_OK && stations_count() == 1);
    check_unchanged("Same:::http://example.org/same:::BG:::MP3\n# keep this comment\n");
    /* Refuse external catalog edits rather than deleting a different station. */
    write_file("stations.txt", "Other:::http://example.org/other:::BG:::MP3\n");
    assert(stations_remove("stations.txt", 0) != ESP_OK && stations_count() == 1);
    assert(!strcmp(stations_get(0)->name, "Same"));
    check_unchanged("Other:::http://example.org/other:::BG:::MP3\n");
    /* Overlong user lines are preserved and do not bind metadata to a row. */
    f = fopen("stations.txt", "wb"); assert(f);
    for (unsigned i = 0; i < 3000; i++) fputc('x', f);
    fputs("\nRemove:::http://example.org/remove:::BG:::MP3\n", f); fclose(f);
    assert(stations_load_file("stations.txt") == ESP_OK && stations_count() == 1);
    assert(stations_remove("stations.txt", 0) == ESP_OK);
    data = read_file("stations.txt"); assert(strlen(data) == 3001 && data[3000] == '\n'); free(data);
    puts("PASS: bounded parsing, catalog compatibility, duplicate saves, empty catalogs, limits, recovery, removal preservation and rollback");
}
'''

# A deterministic scheduler and HTTP transport exercise the actual worker,
# including cancellation during a read, without contacting public services.
RUNTIME_STUBS = r'''
#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#define portMAX_DELAY UINT32_MAX
#define pdTRUE 1
#define pdPASS 1
#define pdMS_TO_TICKS(ms) (ms)
void vTaskDelay(unsigned);
typedef void *QueueHandle_t;
typedef void *SemaphoreHandle_t;
QueueHandle_t xQueueCreate(unsigned, unsigned);
int xQueueSendToFront(QueueHandle_t, const void *, unsigned);
int xQueueSendToBack(QueueHandle_t, const void *, unsigned);
int xQueueReceive(QueueHandle_t, void *, unsigned);
void vQueueDelete(QueueHandle_t);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t, unsigned);
int xSemaphoreGive(SemaphoreHandle_t);
void vSemaphoreDelete(SemaphoreHandle_t);
int xTaskCreate(void (*)(void *), const char *, unsigned, void *, unsigned, void *);
void vTaskDelete(void *);
int64_t esp_timer_get_time(void);
uint32_t esp_random(void);
#define heap_caps_malloc(n, caps) malloc(n)
const char *esp_err_to_name(int);
void esp_crt_bundle_attach(void);
typedef struct { const char *url; int timeout_ms; void (*crt_bundle_attach)(void);
    const char *user_agent; bool disable_auto_redirect; int buffer_size; } esp_http_client_config_t;
typedef struct fake_http *esp_http_client_handle_t;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *);
int esp_http_client_open(esp_http_client_handle_t, int);
int esp_http_client_get_and_clear_last_tls_error(esp_http_client_handle_t, int *, int *);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t);
int esp_http_client_get_status_code(esp_http_client_handle_t);
int esp_http_client_read(esp_http_client_handle_t, char *, int);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t);
void esp_http_client_close(esp_http_client_handle_t);
void esp_http_client_cleanup(esp_http_client_handle_t);
typedef void esp_netif_t;
typedef struct { struct { int type; union { struct { uint32_t addr; } ip4; } u_addr; } ip; } esp_netif_dns_info_t;
#define ESP_NETIF_DNS_MAIN 0
#define ESP_NETIF_DNS_BACKUP 1
#define ESP_IPADDR_TYPE_V4 0
esp_netif_t *esp_netif_get_default_netif(void);
int esp_netif_get_dns_info(esp_netif_t *, int, esp_netif_dns_info_t *);
'''

WORKER_HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "radio/browser.c"
struct fake_queue { unsigned size, count; unsigned char data[8][256]; } queue;
QueueHandle_t xQueueCreate(unsigned capacity, unsigned size) {
    assert(capacity == 8 && size <= 256); queue.size = size; return &queue;
}
int xQueueSendToBack(QueueHandle_t q, const void *value, unsigned wait) {
    (void)wait; assert(q == &queue);
    if (queue.count == 8) return 0;
    memcpy(queue.data[queue.count++], value, queue.size); return 1;
}
int xQueueSendToFront(QueueHandle_t q, const void *value, unsigned wait) {
    (void)wait; assert(q == &queue);
    if (queue.count == 8) return 0;
    memmove(queue.data[1], queue.data[0], queue.count++ * sizeof(queue.data[0]));
    memcpy(queue.data[0], value, queue.size); return 1;
}
int xQueueReceive(QueueHandle_t q, void *value, unsigned wait) {
    (void)wait; assert(q == &queue);
    if (!queue.count) return 0;
    memcpy(value, queue.data[0], queue.size);
    memmove(queue.data[0], queue.data[1], --queue.count * sizeof(queue.data[0])); return 1;
}
void vQueueDelete(QueueHandle_t q) { (void)q; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
int xSemaphoreTake(SemaphoreHandle_t m, unsigned wait) { (void)m; (void)wait; return 1; }
int xSemaphoreGive(SemaphoreHandle_t m) { (void)m; return 1; }
void vSemaphoreDelete(SemaphoreHandle_t m) { (void)m; }
int xTaskCreate(void (*f)(void *), const char *name, unsigned stack, void *arg, unsigned priority, void *handle) {
    (void)f; (void)name; (void)stack; (void)arg; (void)priority; (void)handle; return 1;
}
void vTaskDelete(void *task) { (void)task; }
int64_t esp_timer_get_time(void) { return 1; }
uint32_t esp_random(void) { return 1234; }
void esp_crt_bundle_attach(void) {}
esp_netif_t *esp_netif_get_default_netif(void) { return NULL; }
int esp_netif_get_dns_info(esp_netif_t *n, int type, esp_netif_dns_info_t *dns) {
    (void)n; (void)type; (void)dns; return ESP_FAIL;
}
static bool connected = true, cancel_read, fail_first, oversized, truncated, fail_all;
static unsigned requests, fail_count, retry_delays;
static bool cancel_retry;
void vTaskDelay(unsigned ticks) {
    assert(ticks == 200); retry_delays++;
    if (cancel_retry) radio_browser_cancel();
}
static char last_url[640];
static const char *payload = "[{\"name\":\"Test Station\",\"url_resolved\":\"https://example.org/live\","
    "\"codec\":\"MP3\",\"countrycode\":\"BG\",\"hls\":0,\"lastcheckok\":1,"
    "\"stationuuid\":\"01234567-89ab-cdef-0123-456789abcdef\",\"bitrate\":128}]";
bool network_is_connected(void) { return connected; }
struct fake_http { size_t position; char url[640]; };
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *c) {
    assert(c->crt_bundle_attach && c->disable_auto_redirect && c->timeout_ms == 5000);
    assert(!strncmp(c->url, "https://", 8) && strstr(c->user_agent, "TanRadio/"));
    struct fake_http *h = calloc(1, sizeof(*h)); assert(h);
    snprintf(h->url, sizeof(h->url), "%s", c->url);
    snprintf(last_url, sizeof(last_url), "%s", c->url); return h;
}
int esp_http_client_open(esp_http_client_handle_t h, int length) {
    (void)length; requests++;
    if (fail_count) { fail_count--; return ESP_FAIL; }
    return fail_all || (fail_first && strstr(h->url, "first.api")) ? ESP_FAIL : ESP_OK;
}
int esp_http_client_get_and_clear_last_tls_error(esp_http_client_handle_t h, int *error, int *flags) {
    (void)h; *error = -0x50; *flags = 0; return ESP_OK;
}
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t h) { (void)h; return oversized ? SEARCH_BODY_LIMIT + 10 : strlen(payload); }
int esp_http_client_get_status_code(esp_http_client_handle_t h) { (void)h; return 200; }
int esp_http_client_read(esp_http_client_handle_t h, char *buffer, int capacity) {
    if (cancel_read) { cancel_read = false; radio_browser_cancel(); }
    size_t size = strlen(payload) - h->position;
    if (truncated && h->position) return 0;
    if (size > 31) size = 31;
    assert((int)size <= capacity);
    memcpy(buffer, payload + h->position, size); h->position += size; return size;
}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t h) { return h->position == strlen(payload); }
void esp_http_client_close(esp_http_client_handle_t h) { (void)h; }
void esp_http_client_cleanup(esp_http_client_handle_t h) { free(h); }
static void cache_mirrors(void) {
    mirror_count = 2; mirrors_expire = INT64_MAX;
    strcpy(mirrors[0], "first.api.radio-browser.info");
    strcpy(mirrors[1], "second.api.radio-browser.info");
}
int main(void) {
    radio_browser_init(); assert(jobs && mutex && results); cache_mirrors();
    assert(radio_browser_search("obsolete"));
    assert(radio_browser_search("A&B + Радио"));
    browser_worker(NULL);
    radio_browser_status_t state; radio_browser_status(&state);
    assert(!state.busy && state.count == 1 && requests == 1);
    assert(strstr(last_url, "name=A%26B%20%2B%20%D0%A0"));
    radio_station_t snapshot;
    assert(radio_browser_result(0, &snapshot));
    assert(!radio_browser_result(1, &snapshot));
    assert(!radio_browser_search_by("BG", (radio_browser_search_mode_t)99));
    assert(radio_browser_search_by("  bg  ", BROWSER_SEARCH_COUNTRY));
    browser_worker(NULL);
    assert(strstr(last_url, "countrycode=BG&codec=MP3"));
    assert(!strstr(last_url, "?name="));
    assert(radio_browser_search_by("United Kingdom", BROWSER_SEARCH_COUNTRY));
    browser_worker(NULL);
    assert(strstr(last_url, "country=United%20Kingdom&codec=MP3"));
    assert(radio_browser_search_by("A&B", BROWSER_SEARCH_COUNTRY));
    assert(radio_browser_search("name takes priority"));
    browser_worker(NULL);
    assert(strstr(last_url, "?name=name%20takes%20priority"));
    assert(radio_browser_search_by("BG", BROWSER_SEARCH_COUNTRY));
    radio_browser_cancel(); browser_worker(NULL);
    radio_browser_status(&state); assert(!state.busy && state.count == 0);
    cancel_read = true;
    assert(radio_browser_search("cancel during HTTP read")); browser_worker(NULL);
    radio_browser_status(&state); assert(!state.busy && state.count == 0);
    assert(!strcmp(snapshot.name, "Test Station"));
    requests = 0; fail_first = true;
    assert(radio_browser_search("failover")); browser_worker(NULL);
    radio_browser_status(&state); assert(state.count == 1 && requests == 2);
    fail_first = false;
    /* A lone discovered server gets one automatic retry for a transient reset. */
    requests = retry_delays = 0; mirror_count = 1; fail_count = 1;
    assert(radio_browser_search("transient reset")); browser_worker(NULL);
    radio_browser_status(&state);
    assert(state.count == 1 && requests == 2 && retry_delays == 1);
    requests = retry_delays = 0; fail_count = 1; cancel_retry = true;
    assert(radio_browser_search("cancel retry")); browser_worker(NULL);
    radio_browser_status(&state);
    assert(!state.busy && requests == 1 && retry_delays == 1);
    cancel_retry = false;
    requests = retry_delays = 0; mirror_count = 1; fail_all = true;
    assert(radio_browser_search("persistent reset")); browser_worker(NULL);
    radio_browser_status(&state);
    assert(!state.busy && requests == 2 && retry_delays == 1);
    fail_all = false; cache_mirrors();
    oversized = true;
    assert(radio_browser_search("oversized")); browser_worker(NULL);
    radio_browser_status(&state); assert(!state.busy && state.count == 0);
    assert(strstr(state.message, "too large"));
    oversized = false; truncated = true; cache_mirrors();
    assert(radio_browser_search("partial JSON")); browser_worker(NULL);
    radio_browser_status(&state); assert(!state.busy && state.count == 0);
    assert(strstr(state.message, "response failed"));
    truncated = false; connected = false;
    assert(radio_browser_search("offline")); browser_worker(NULL);
    radio_browser_status(&state); assert(!state.busy && strstr(state.message, "Wi-Fi"));
    connected = true; fail_all = true; cache_mirrors();
    assert(radio_browser_search("both mirrors fail")); browser_worker(NULL);
    radio_browser_status(&state); assert(!state.busy && state.count == 0);
    assert(strstr(state.message, "connection failed"));
    fail_all = false; cache_mirrors();
    unsigned revision = state.revision;
    radio_browser_count_play(snapshot.stationuuid); browser_worker(NULL);
    radio_browser_status(&state); assert(state.revision == revision);
    assert(strstr(last_url, "/json/url/01234567-89ab-cdef-0123-456789abcdef"));
    mirror_count = 1; requests = retry_delays = 0; fail_all = true;
    radio_browser_count_play(snapshot.stationuuid); browser_worker(NULL);
    assert(requests == 1 && retry_delays == 0);
    puts("PASS: search supersession, cancellation during HTTP read, mirror failover, response bounds, offline errors and queued playback clicks");
    free(results);
}
'''


def main():
    if not (CJSON / "cJSON.c").is_file():
        raise SystemExit("Run make build first to fetch the declared cJSON dependency.")
    base = dict(name=" Good Station ", url="https://example.org/list.m3u",
                url_resolved="https://example.org/live.mp3?x=1&y=2", countrycode="BG",
                codec="MP3", stationuuid="01234567-89ab-cdef-0123-456789abcdef",
                hls=0, lastcheckok=1, bitrate=128)
    fixtures = [base, base.copy()]
    for changes in [dict(hls=1), dict(codec="AAC"), dict(lastcheckok=0),
                    dict(stationuuid="invalid"), dict(url_resolved="ftp://example.org/x"),
                    dict(name="x" * 97), dict(name="Injected\nrecord"),
                    dict(name="Bad:::delimiter"), dict(url_resolved="https://example.org/" + "x" * 512),
                    dict(countrycode="BGR"), dict(url_resolved="")]:
        fixtures.append(dict(base, **changes))
    fixtures.append(dict(base, name="Unknown country", countrycode="", bitrate=None,
                         stationuuid="11234567-89ab-cdef-0123-456789abcdef",
                         url_resolved="http://example.org/second"))
    with tempfile.TemporaryDirectory(prefix="tanradio-browser-") as directory:
        work = Path(directory)
        for name in ["esp_err.h", "esp_log.h", "esp_heap_caps.h"]:
            (work / name).write_text(STUBS)
        (work / "harness.c").write_text(HARNESS)
        (work / "mixed.json").write_text(json.dumps(fixtures))
        command = ["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                   "-Wno-sign-compare", "-g", "-fsanitize=address,undefined",
                   "-DCJSON_NESTING_LIMIT=16", "-I", str(work), "-I", str(ROOT / "main"),
                   "-I", str(CJSON), str(work / "harness.c"),
                   str(ROOT / "main/radio/stations.c"), str(ROOT / "main/radio/browser_parse.c"),
                   str(CJSON / "cJSON.c"), "-Wl,--wrap=rename", "-Wl,--wrap=fsync",
                   "-o", str(work / "check")]
        subprocess.run(command, check=True)
        subprocess.run([str(work / "check")], check=True, cwd=work,
                       env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))
        (work / "runtime.h").write_text(RUNTIME_STUBS)
        for name in ["lwip/sockets.h", "esp_crt_bundle.h", "esp_http_client.h",
                     "esp_netif.h", "esp_random.h", "esp_timer.h", "freertos/FreeRTOS.h",
                     "freertos/queue.h", "freertos/semphr.h", "freertos/task.h"]:
            target = work / name
            target.parent.mkdir(exist_ok=True)
            target.write_text('#include "runtime.h"\n')
        (work / "worker.c").write_text(WORKER_HARNESS)
        worker_command = command.copy()
        worker_command[worker_command.index(str(work / "harness.c"))] = str(work / "worker.c")
        worker_command.remove("-Wl,--wrap=rename")
        worker_command.remove("-Wl,--wrap=fsync")
        worker_command[-1] = str(work / "worker")
        subprocess.run(worker_command, check=True)
        subprocess.run([str(work / "worker")], check=True, cwd=work,
                       env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))


if __name__ == "__main__":
    main()

#include "radio/browser_parse.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include "cJSON.h"

static const char *string_field(const cJSON *object, const char *key)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(value) ? value->valuestring : "";
}

static bool number_is(const cJSON *object, const char *key, double number)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsNumber(value) && value->valuedouble == number;
}

bool browser_parse_stations(const char *json, radio_station_t *results,
                             size_t capacity, size_t *count)
{
    *count = 0;
    cJSON *root = cJSON_ParseWithOpts(json, NULL, true);
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return false;
    }
    const cJSON *item;
    cJSON_ArrayForEach(item, root) {
        if (*count >= capacity) break;
        const char *name = string_field(item, "name");
        const char *url = string_field(item, "url_resolved");
        const char *country = string_field(item, "countrycode");
        const char *codec = string_field(item, "codec");
        const char *uuid = string_field(item, "stationuuid");
        /* Do not guess at raw playlist URLs or missing compatibility data. */
        if (strcmp(codec, "MP3") != 0 || !number_is(item, "hls", 0) ||
            !number_is(item, "lastcheckok", 1) || !stations_uuid_valid(uuid) ||
            strlen(name) > STATIONS_MAX_NAME_LENGTH ||
            strlen(url) > STATIONS_MAX_URL_LENGTH || strlen(country) > 2) {
            continue;
        }
        radio_station_t station = {0};
        snprintf(station.name, sizeof(station.name), "%s", name);
        /* Trim directory display names while preserving URLs byte for byte. */
        size_t start = 0;
        while (isspace((unsigned char)station.name[start])) start++;
        memmove(station.name, station.name + start, strlen(station.name + start) + 1);
        size_t end = strlen(station.name);
        while (end && isspace((unsigned char)station.name[end - 1])) station.name[--end] = 0;
        snprintf(station.url, sizeof(station.url), "%s", url);
        snprintf(station.country, sizeof(station.country), "%s", country);
        snprintf(station.codec, sizeof(station.codec), "%s", codec);
        snprintf(station.stationuuid, sizeof(station.stationuuid), "%s", uuid);
        const cJSON *bitrate = cJSON_GetObjectItemCaseSensitive(item, "bitrate");
        if (cJSON_IsNumber(bitrate) && bitrate->valuedouble >= 0 &&
            bitrate->valuedouble <= 10000) {
            station.bitrate_kbps = (uint16_t)bitrate->valuedouble;
        }
        if (!stations_valid(&station)) continue;
        bool duplicate = false;
        for (size_t i = 0; i < *count; i++) {
            if (strcmp(results[i].stationuuid, uuid) == 0 ||
                strcmp(results[i].url, url) == 0) duplicate = true;
        }
        if (!duplicate) results[(*count)++] = station;
    }
    cJSON_Delete(root);
    return true;
}

bool browser_encode_query(const char *query, char *encoded, size_t capacity)
{
    const char hex[] = "0123456789ABCDEF";
    size_t used = 0;
    for (const unsigned char *p = (const unsigned char *)query; *p; p++) {
        bool plain = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                     (*p >= '0' && *p <= '9') || strchr("-._~", *p) != NULL;
        size_t needed = plain ? 1 : 3;
        if (used + needed >= capacity) return false;
        if (plain) encoded[used++] = *p;
        else {
            encoded[used++] = '%';
            encoded[used++] = hex[*p >> 4];
            encoded[used++] = hex[*p & 15];
        }
    }
    if (used >= capacity) return false;
    encoded[used] = 0;
    return true;
}

static uint16_t read16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

/* Decode DNS compression with bounded jumps, including hostile pointer loops. */
static bool dns_name(const uint8_t *packet, size_t length, size_t *position,
                     char *name, size_t capacity)
{
    size_t at = *position, used = 0;
    bool jumped = false;
    for (size_t steps = 0; steps < 128 && at < length; steps++) {
        uint8_t size = packet[at++];
        if (!size) {
            if (!jumped) *position = at;
            if (used >= capacity) return false;
            name[used] = 0;
            return true;
        }
        if ((size & 0xc0) == 0xc0) {
            if (at >= length) return false;
            size_t next = ((size & 0x3f) << 8) | packet[at++];
            if (!jumped) *position = at;
            jumped = true;
            at = next;
            continue;
        }
        if (size > 63 || at + size > length || used + size + 1 >= capacity) return false;
        if (used) name[used++] = '.';
        for (size_t i = 0; i < size; i++) {
            unsigned char c = packet[at++];
            if (!(isalnum(c) || c == '-' || c == '_')) return false;
            name[used++] = (char)tolower(c);
        }
    }
    return false;
}

size_t browser_parse_srv(const uint8_t *packet, size_t length, uint16_t query_id,
                         char hosts[BROWSER_MIRROR_LIMIT][BROWSER_HOST_LENGTH])
{
    if (length < 12 || read16(packet) != query_id ||
        (read16(packet + 2) & 0xfa0f) != 0x8000 || read16(packet + 4) != 1) return 0;
    size_t at = 12, count = 0;
    char name[256];
    if (!dns_name(packet, length, &at, name, sizeof(name)) ||
        strcmp(name, "_api._tcp.radio-browser.info") || at + 4 > length ||
        read16(packet + at) != 33 || read16(packet + at + 2) != 1) return 0;
    at += 4;
    uint16_t answers = read16(packet + 6);
    for (uint16_t i = 0; i < answers; i++) {
        if (!dns_name(packet, length, &at, name, sizeof(name)) || at + 10 > length) return 0;
        uint16_t type = read16(packet + at), klass = read16(packet + at + 2);
        size_t size = read16(packet + at + 8);
        at += 10;
        if (at + size > length) return 0;
        if (type == 33 && klass == 1 && size >= 7 && count < BROWSER_MIRROR_LIMIT &&
            strcmp(name, "_api._tcp.radio-browser.info") == 0) {
            size_t target = at + 6;
            if (!dns_name(packet, length, &target, name, sizeof(name)) || target > at + size) return 0;
            const char suffix[] = ".api.radio-browser.info";
            size_t n = strlen(name);
            if (n > sizeof(suffix) - 1 && n < BROWSER_HOST_LENGTH &&
                strcmp(name + n - (sizeof(suffix) - 1), suffix) == 0) {
                bool duplicate = false;
                for (size_t h = 0; h < count; h++) if (!strcmp(hosts[h], name)) duplicate = true;
                if (!duplicate) memcpy(hosts[count++], name, n + 1);
            }
        }
        at += size;
    }
    return count;
}

bool browser_parse_logo(const char *json, const char *uuid, char *url, size_t capacity)
{
    cJSON *root = cJSON_ParseWithOpts(json, NULL, true);
    bool found = false;
    if (cJSON_IsArray(root)) {
        const cJSON *item;
        cJSON_ArrayForEach(item, root) {
            if (strcasecmp(string_field(item, "stationuuid"), uuid) != 0) continue;
            const char *favicon = string_field(item, "favicon");
            size_t length = strlen(favicon);
            if (length > 8 && length < capacity && !strncmp(favicon, "https://", 8)) {
                memcpy(url, favicon, length + 1);
                found = true;
                break;
            }
        }
    }
    cJSON_Delete(root);
    return found;
}

/*
 * TanRadio external station catalog.
 */
#include "radio/stations.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <stdatomic.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "tanradio.stations";

#define STATIONS_LINE_SIZE 1024
#define STATIONS_MAX_BITRATE_KBPS 10000

static radio_station_t *catalog = NULL;
/* Allocated once before playback; runtime additions never move the array. Removal compacts it under the UI
 * mutex; readers must not retain catalog pointers across removals.
 * Publishing count last makes appended records visible to readers. */
static atomic_size_t catalog_count = 0;

esp_err_t stations_ensure_file(const char *path)
{
    struct stat info;
    if (stat(path, &info) == 0) {
        return ESP_OK;
    }

    if (errno != ENOENT) {
        ESP_LOGE(TAG, "Unable to check %s: errno %d", path, errno);
        return ESP_FAIL;
    }

    /* FAT cannot replace an existing name atomically. Recover a save that
     * lost power between the two renames before creating an empty catalog. */
    char backup[280];
    if (snprintf(backup, sizeof(backup), "%s.bak", path) >= sizeof(backup)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (stat(backup, &info) == 0) {
        return rename(backup, path) == 0 ? ESP_OK : ESP_FAIL;
    }

    char directory[256];
    const char *separator = strrchr(path, '/');
    const size_t directory_length =
        separator == NULL ? 0 : (size_t)(separator - path);

    if (directory_length == 0 || directory_length >= sizeof(directory)) {
        ESP_LOGE(TAG, "Invalid station file path: %s", path);
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(directory, path, directory_length);
    directory[directory_length] = '\0';

    if (mkdir(directory, 0775) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "Unable to create %s: errno %d", directory, errno);
        return ESP_FAIL;
    }

    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        ESP_LOGE(TAG, "Unable to create %s: errno %d", path, errno);
        return ESP_FAIL;
    }

    const bool closed = fclose(file) == 0;

    if (!closed) {
        ESP_LOGE(TAG, "Unable to create empty station catalog at %s", path);
        remove(path);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Created empty station catalog at %s", path);
    return ESP_OK;
}

static void stations_clear(void)
{
    free(catalog);
    catalog = NULL;
    catalog_count = 0;
}

static char *trim(char *value)
{
    while (isspace((unsigned char)*value)) {
        value++;
    }

    char *end = value + strlen(value);
    while (end > value && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }

    return value;
}

static bool valid_country(const char *value)
{
    return strlen(value) == 2 &&
           isupper((unsigned char)value[0]) &&
           isupper((unsigned char)value[1]);
}

static bool valid_url(const char *value)
{
    return strncmp(value, "http://", 7) == 0 ||
           strncmp(value, "https://", 8) == 0;
}

static bool parse_bitrate(const char *value, uint16_t *bitrate_kbps)
{
    *bitrate_kbps = 0;

    if (*value == '\0') {
        return true;
    }

    errno = 0;
    char *end = NULL;
    const unsigned long parsed = strtoul(value, &end, 10);

    if (errno != 0 || end == value || *end != '\0' ||
        parsed > STATIONS_MAX_BITRATE_KBPS || parsed > UINT16_MAX) {
        return false;
    }

    *bitrate_kbps = (uint16_t)parsed;
    return true;
}

bool stations_uuid_valid(const char *uuid)
{
    if (uuid == NULL || strlen(uuid) != 36) {
        return false;
    }
    for (size_t i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (uuid[i] != '-') return false;
        } else if (!isxdigit((unsigned char)uuid[i])) {
            return false;
        }
    }
    return true;
}

static bool safe_field(const char *value, size_t capacity)
{
    if (strnlen(value, capacity) == capacity || strstr(value, ":::") != NULL) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (*p < 32 || *p == 127) return false;
    }
    return true;
}

bool stations_valid(const radio_station_t *station)
{
    return station != NULL &&
           safe_field(station->name, sizeof(station->name)) &&
           station->name[0] != '\0' && station->name[0] != '#' &&
           !isspace((unsigned char)station->name[0]) &&
           !isspace((unsigned char)station->name[strlen(station->name) - 1]) &&
           safe_field(station->url, sizeof(station->url)) &&
           valid_url(station->url) && strpbrk(station->url, " \t") == NULL &&
           safe_field(station->country, sizeof(station->country)) &&
           (station->country[0] == '\0' || valid_country(station->country)) &&
           safe_field(station->codec, sizeof(station->codec)) &&
           station->codec[0] != '\0' &&
           safe_field(station->stationuuid, sizeof(station->stationuuid)) &&
           (station->stationuuid[0] == '\0' || stations_uuid_valid(station->stationuuid)) &&
           station->bitrate_kbps <= STATIONS_MAX_BITRATE_KBPS;
}

static bool parse_fields(char *fields[5], radio_station_t *station)
{
    const char *name = fields[0];
    const char *url = fields[1];
    const char *country = fields[2];
    const char *codec = fields[3];
    uint16_t bitrate_kbps;

    if (*name == '\0' || strlen(name) > STATIONS_MAX_NAME_LENGTH ||
        strlen(url) > STATIONS_MAX_URL_LENGTH || !valid_url(url) ||
        (*country != '\0' && !valid_country(country)) || *codec == '\0' ||
        strlen(codec) > STATIONS_MAX_CODEC_LENGTH ||
        !parse_bitrate(fields[4], &bitrate_kbps)) {
        return false;
    }

    *station = (radio_station_t){.bitrate_kbps = bitrate_kbps};
    snprintf(station->name, sizeof(station->name), "%s", name);
    snprintf(station->url, sizeof(station->url), "%s", url);
    snprintf(station->codec, sizeof(station->codec), "%s", codec);
    snprintf(station->country, sizeof(station->country), "%s", country);
    return stations_valid(station);
}

static bool parse_line(char *line, radio_station_t *station)
{
    char *fields[5] = {0};
    char *cursor = line;

    for (size_t field = 0; field < 3; field++) {
        char *separator = strstr(cursor, ":::");
        if (separator == NULL) {
            return false;
        }

        *separator = '\0';
        fields[field] = trim(cursor);
        cursor = separator + 3;
    }

    char *separator = strstr(cursor, ":::");
    if (separator == NULL) {
        fields[3] = trim(cursor);
        fields[4] = "";
        return parse_fields(fields, station);
    }

    *separator = '\0';
    fields[3] = trim(cursor);
    fields[4] = trim(separator + 3);

    if (strstr(fields[4], ":::") != NULL) {
        return false;
    }

    return parse_fields(fields, station);
}

esp_err_t stations_load_file(const char *path)
{
    stations_clear();
    catalog = heap_caps_calloc(STATIONS_MAX_COUNT, sizeof(*catalog),
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (catalog == NULL) return ESP_ERR_NO_MEM;

    FILE *file = fopen(path, "r");
    if (file == NULL) {
        ESP_LOGE(TAG, "Unable to open %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return ESP_FAIL;
    }

    const long file_size = ftell(file);
    if (file_size < 0 || file_size > STATIONS_MAX_FILE_SIZE ||
        fseek(file, 0, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "Station file is too large or cannot be read");
        fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }

    char line[STATIONS_LINE_SIZE];
    size_t line_number = 0;
    char pending_uuid[37] = {0};

    while (fgets(line, sizeof(line), file) != NULL) {
        line_number++;
        const size_t line_length = strlen(line);

        if (line_length == sizeof(line) - 1 && line[line_length - 1] != '\n') {
            int character;
            while ((character = fgetc(file)) != '\n' && character != EOF) {
            }
            ESP_LOGW(TAG, "Ignoring overlong station line %u",
                     (unsigned int)line_number);
            pending_uuid[0] = '\0';
            continue;
        }

        char *record = trim(line);
        if (strncmp(record, "#RADIOBROWSERUUID:", 18) == 0) {
            const char *uuid = record + 18;
            snprintf(pending_uuid, sizeof(pending_uuid), "%s",
                     stations_uuid_valid(uuid) ? uuid : "");
            continue;
        }
        if (*record == '\0' || *record == '#') {
            continue;
        }

        if (catalog_count >= STATIONS_MAX_COUNT ||
            !parse_line(record, &catalog[catalog_count])) {
            ESP_LOGW(TAG, "Ignoring invalid station line %u",
                     (unsigned int)line_number);
        } else {
            snprintf(catalog[catalog_count].stationuuid, 37, "%s", pending_uuid);
            catalog_count++;
        }
        pending_uuid[0] = '\0';
    }

    bool read_ok = !ferror(file);
    if (fclose(file) != 0) read_ok = false;
    if (!read_ok) {
        stations_clear();
        return ESP_FAIL;
    }

    if (catalog_count == 0) {
        ESP_LOGW(TAG, "No valid stations in %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Loaded %u stations from %s",
             (unsigned int)catalog_count, path);
    return ESP_OK;
}

size_t stations_count(void)
{
    return catalog_count;
}

const radio_station_t *stations_get(size_t index)
{
    return index < catalog_count ? &catalog[index] : NULL;
}

bool stations_same(const radio_station_t *left, const radio_station_t *right)
{
    return left && right &&
           ((left->stationuuid[0] && right->stationuuid[0] &&
             strcasecmp(left->stationuuid, right->stationuuid) == 0) ||
            (left->url[0] && right->url[0] && strcmp(left->url, right->url) == 0));
}

bool stations_find(const radio_station_t *station, size_t *index)
{
    size_t count = catalog_count;
    for (size_t i = 0; i < count; i++) {
        if (stations_same(&catalog[i], station)) {
            if (index) *index = i;
            return true;
        }
    }
    return false;
}

static esp_err_t stations_replace_file(const char *path, const char *temporary,
                                       const char *backup)
{
    if (remove(backup) != 0 && errno != ENOENT) {
        remove(temporary);
        return ESP_FAIL;
    }
    if (rename(path, backup) != 0) {
        remove(temporary);
        return ESP_FAIL;
    }
    if (rename(temporary, path) != 0) {
        /* If rollback itself fails, ensure_file recovers .bak on next boot. */
        rename(backup, path);
        remove(temporary);
        return ESP_FAIL;
    }
    remove(backup);
    return ESP_OK;
}

esp_err_t stations_save(const char *path, const radio_station_t *station,
                        size_t *index, bool *already_saved)
{
    if (!path || !index || !already_saved || !stations_valid(station)) {
        return ESP_ERR_INVALID_ARG;
    }
    *already_saved = false;
    size_t count = catalog_count;
    if (stations_find(station, index)) {
        *already_saved = true;
        return ESP_OK;
    }
    if (catalog == NULL) return ESP_ERR_INVALID_STATE;
    if (count >= STATIONS_MAX_COUNT) return ESP_ERR_NO_MEM;

    char temporary[280], backup[280];
    if (snprintf(temporary, sizeof(temporary), "%s.tmp", path) >= sizeof(temporary) ||
        snprintf(backup, sizeof(backup), "%s.bak", path) >= sizeof(backup)) {
        return ESP_ERR_INVALID_ARG;
    }
    FILE *source = fopen(path, "rb");
    if (source == NULL) return ESP_ERR_NOT_FOUND;
    FILE *target = fopen(temporary, "wb");
    if (target == NULL) {
        fclose(source);
        return ESP_FAIL;
    }

    /* Preserve user records/comments exactly. Never rewrite from the parsed
     * catalog, which may have skipped malformed or unsupported records. */
    char chunk[512];
    size_t total = 0, bytes;
    int last = '\n';
    bool ok = true;
    while ((bytes = fread(chunk, 1, sizeof(chunk), source)) > 0) {
        total += bytes;
        if (total > STATIONS_MAX_FILE_SIZE || fwrite(chunk, 1, bytes, target) != bytes) {
            ok = false;
            break;
        }
        last = (unsigned char)chunk[bytes - 1];
    }
    if (ferror(source)) ok = false;
    if (fclose(source) != 0) ok = false;

    char record[800];
    int length = snprintf(record, sizeof(record), "%s%s%s%s%s:::%s:::%s:::%s:::%u\n",
                          last == '\n' ? "" : "\n",
                          station->stationuuid[0] ? "#RADIOBROWSERUUID:" : "",
                          station->stationuuid,
                          station->stationuuid[0] ? "\n" : "",
                          station->name, station->url, station->country,
                          station->codec, station->bitrate_kbps);
    if (length < 0 || length >= sizeof(record) || total + length > STATIONS_MAX_FILE_SIZE) {
        ok = false;
    }
    if (ok && fwrite(record, 1, length, target) != length) ok = false;
    if (fflush(target) != 0 || fsync(fileno(target)) != 0) ok = false;
    if (fclose(target) != 0) ok = false;
    if (!ok) {
        remove(temporary);
        return total + (length > 0 ? length : 0) > STATIONS_MAX_FILE_SIZE
                   ? ESP_ERR_INVALID_SIZE : ESP_FAIL;
    }

    esp_err_t result = stations_replace_file(path, temporary, backup);
    if (result != ESP_OK) return result;
    catalog[count] = *station;
    catalog_count = count + 1;
    *index = count;
    return ESP_OK;
}

esp_err_t stations_remove(const char *path, size_t index)
{
    size_t count = catalog_count;
    if (!path || index >= count) return ESP_ERR_INVALID_ARG;
    char temporary[280], backup[280];
    if (snprintf(temporary, sizeof(temporary), "%s.tmp", path) >= sizeof(temporary) ||
        snprintf(backup, sizeof(backup), "%s.bak", path) >= sizeof(backup)) {
        return ESP_ERR_INVALID_ARG;
    }
    FILE *source = fopen(path, "rb");
    if (!source) return ESP_ERR_NOT_FOUND;
    if (fseek(source, 0, SEEK_END) != 0) { fclose(source); return ESP_FAIL; }
    long length = ftell(source);
    if (length < 0 || length > STATIONS_MAX_FILE_SIZE ||
        fseek(source, 0, SEEK_SET) != 0) {
        fclose(source);
        return ESP_ERR_INVALID_SIZE;
    }
    /* Keep parsing scratch off the 8 KiB input stack. The bounded source copy
     * also lets us preserve comments, malformed lines and line endings exactly. */
    struct removal_workspace {
        radio_station_t parsed;
        char line[STATIONS_LINE_SIZE];
        char uuid[37];
        char data[];
    } *work = heap_caps_calloc(1, sizeof(*work) + (size_t)length + 1,
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!work) { fclose(source); return ESP_ERR_NO_MEM; }
    bool ok = fread(work->data, 1, length, source) == (size_t)length && !ferror(source);
    if (fclose(source) != 0) ok = false;
    size_t ordinal = 0, block = 0, remove_block = 0, remove_line = 0, remove_end = 0;
    for (size_t at = 0; ok && at < (size_t)length;) {
        const char *newline = memchr(work->data + at, '\n', length - at);
        size_t end = newline ? (size_t)(newline - work->data) + 1 : (size_t)length;
        size_t bytes = end - at;
        if (bytes >= sizeof(work->line) ||
            (bytes == sizeof(work->line) - 1 && !newline)) {
            work->uuid[0] = '\0';
            block = end;
            at = end;
            continue;
        }
        memcpy(work->line, work->data + at, bytes);
        work->line[bytes] = '\0';
        char *record = trim(work->line);
        if (strncmp(record, "#RADIOBROWSERUUID:", 18) == 0) {
            snprintf(work->uuid, sizeof(work->uuid), "%s",
                     stations_uuid_valid(record + 18) ? record + 18 : "");
        } else if (*record && *record != '#') {
            if (parse_line(record, &work->parsed) && ordinal < count) {
                snprintf(work->parsed.stationuuid, sizeof(work->parsed.stationuuid), "%s", work->uuid);
                const radio_station_t *saved = &catalog[ordinal];
                /* Refuse stale on-disk edits rather than removing a different
                 * record or publishing a catalog inconsistent with the file. */
                ok = strcmp(saved->name, work->parsed.name) == 0 &&
                     strcmp(saved->url, work->parsed.url) == 0 &&
                     strcmp(saved->country, work->parsed.country) == 0 &&
                     strcmp(saved->codec, work->parsed.codec) == 0 &&
                     strcmp(saved->stationuuid, work->parsed.stationuuid) == 0 &&
                     saved->bitrate_kbps == work->parsed.bitrate_kbps;
                if (ordinal == index) {
                    remove_block = block; remove_line = at; remove_end = end;
                }
                ordinal++;
            }
            work->uuid[0] = '\0';
            block = end;
        }
        at = end;
    }
    if (!ok || ordinal != count || !remove_end) { free(work); return ESP_FAIL; }

    FILE *target = fopen(temporary, "wb");
    if (!target) { free(work); return ESP_FAIL; }
    ok = fwrite(work->data, 1, remove_block, target) == remove_block;
    /* Remove associated UUID declarations, including superseded declarations,
     * without deleting user comments interspersed before this station. */
    for (size_t at = remove_block; ok && at < remove_line;) {
        const char *newline = memchr(work->data + at, '\n', remove_line - at);
        size_t end = newline ? (size_t)(newline - work->data) + 1 : remove_line;
        size_t bytes = end - at;
        memcpy(work->line, work->data + at, bytes);
        work->line[bytes] = '\0';
        if (strncmp(trim(work->line), "#RADIOBROWSERUUID:", 18) != 0) {
            ok = fwrite(work->data + at, 1, bytes, target) == bytes;
        }
        at = end;
    }
    size_t remaining = (size_t)length - remove_end;
    if (ok) ok = fwrite(work->data + remove_end, 1, remaining, target) == remaining;
    free(work);
    if (fflush(target) != 0 || fsync(fileno(target)) != 0) ok = false;
    if (fclose(target) != 0) ok = false;
    if (!ok) { remove(temporary); return ESP_FAIL; }
    esp_err_t result = stations_replace_file(path, temporary, backup);
    if (result != ESP_OK) return result;
    memmove(&catalog[index], &catalog[index + 1], (count - index - 1) * sizeof(*catalog));
    catalog_count = count - 1;
    return ESP_OK;
}

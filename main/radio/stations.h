/*
 * TanRadio station catalog loaded from external storage.
 */
#ifndef TANRADIO_MAIN_RADIO_STATIONS_H
#define TANRADIO_MAIN_RADIO_STATIONS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define STATIONS_MAX_COUNT 128
#define STATIONS_MAX_FILE_SIZE (64 * 1024)
#define STATIONS_MAX_NAME_LENGTH 96
#define STATIONS_MAX_URL_LENGTH 512
#define STATIONS_MAX_CODEC_LENGTH 32
#define STATIONS_FILE_PATH "/sd/tanradio/stations.txt"

/* Value records also serve as owned playback and search-result snapshots. */
typedef struct {
    char name[STATIONS_MAX_NAME_LENGTH + 1];
    char url[STATIONS_MAX_URL_LENGTH + 1];
    char country[3];
    char codec[STATIONS_MAX_CODEC_LENGTH + 1];
    char stationuuid[37];
    uint16_t bitrate_kbps;
} radio_station_t;

/** Create the parent directory and empty station file when path is absent. */
esp_err_t stations_ensure_file(const char *path);

/** Replace the catalog at startup, before readers/playback tasks are started. */
esp_err_t stations_load_file(const char *path);

bool stations_valid(const radio_station_t *station);
bool stations_uuid_valid(const char *uuid);
/** UUID or exact stream URL identifies a station; display names do not. */
bool stations_same(const radio_station_t *left, const radio_station_t *right);
bool stations_find(const radio_station_t *station, size_t *index);

/* Called by the input task. Append without replacing any published records.
 * Duplicate UUIDs/URLs return their existing index without touching the file. */
esp_err_t stations_save(const char *path, const radio_station_t *station,
                        size_t *index, bool *already_saved);

/* Input task only, under ui_mutex to exclude catalog readers. Preserve all
 * other file bytes and compact the catalog only after the SD update succeeds.
 * Catalog pointers/indices can change; playback must own its station record. */
esp_err_t stations_remove(const char *path, size_t index);

size_t stations_count(void);
const radio_station_t *stations_get(size_t index);

#endif /* TANRADIO_MAIN_RADIO_STATIONS_H */

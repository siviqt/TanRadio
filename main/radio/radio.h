/******************************************************************************
 *
 * Project : TanRadio
 * Module  : radio
 * File    : radio.h
 *
 * Purpose :
 *     Public interface for the radio module.
 *
 ******************************************************************************/

#ifndef TANRADIO_MAIN_RADIO_RADIO_H
#define TANRADIO_MAIN_RADIO_RADIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "radio/stations.h"
#include "radio/now_playing.h"

/**
 * @brief Initialize the radio subsystem.
 */
void radio_init(void);

/** Silence playback immediately after exit is confirmed. */
void radio_stop_for_exit(void);

/**
 * @brief Request the radio task to stop playback and exit to the launcher.
 */
void radio_request_exit(void);

/**
 * @brief Return the number of configured stations.
 */
size_t radio_station_count(void);

/**
 * @brief Return the name of a station.
 *
 * @param index Station index.
 * @return Station name, or NULL if index is invalid.
 */
const char *radio_station_get_name(size_t index);

/**
 * @brief Return the URL of a station.
 *
 * @param index Station index.
 * @return Station URL, or NULL if index is invalid.
 */
const char *radio_station_get_url(size_t index);

/** Return the configured two-character country code, or NULL. */
const char *radio_station_get_country(size_t index);

/** Return the configured station bit-rate in kbps, or zero when absent. */
uint16_t radio_station_get_bitrate_kbps(size_t index);

/**
 * @brief Return the currently selected station index.
 */
size_t radio_station_get_selected(void);

/** Input task: rebase catalog indices after removal, preserving owned streams. */
void radio_catalog_changed(void);

/**
 * @brief Select a station.
 *
 * @param index Station index.
 * @return true if selected, false if index is invalid.
 */
bool radio_station_select(size_t index);

/** Play a directory result without adding it to the saved station catalog. */
bool radio_play_preview(const radio_station_t *station);

/** Play an owned record, reusing the active connection when identity matches. */
bool radio_play_record(const radio_station_t *station);

typedef struct {
    radio_station_t station; /* Requested target, including while switching. */
    bool stopped;
    bool muted;
    bool buffering;
} radio_playback_status_t;
void radio_get_playback_status(radio_playback_status_t *status);

typedef struct {
    radio_playback_status_t playback;
    uint32_t connection;
    uint32_t metadata_revision;
    radio_metadata_t metadata;
} radio_now_playing_t;
/* Copy playback and metadata together, including while a station is switching. */
void radio_get_now_playing(radio_now_playing_t *snapshot);

/** Copy the current playback record; safe across searches and catalog saves. */
bool radio_get_playing_station(radio_station_t *station);

/**
 * @brief Start or restart radio playback.
 *
 * @return true if playback is running or was successfully started.
 */
bool radio_play(void);

/**
 * @brief Stop radio playback without exiting the application.
 *
 * @return true if the stop request was accepted.
 */
bool radio_stop(void);

/**
 * @brief Toggle audio mute.
 *
 * @return true if audio is now muted, false if audio is now unmuted.
 */
bool radio_toggle_mute(void);

/**
 * @brief Return whether radio playback is currently stopped.
 */
bool radio_is_stopped(void);

/**
 * @brief Return whether radio audio is currently muted.
 */
bool radio_is_muted(void);

/**
 * @brief Return whether playback is silently filling its safety buffers.
 */
bool radio_is_buffering(void);

/** Current audio failure message, or NULL; cleared by a new play request. */
const char *radio_get_audio_error(void);

/**
 * @brief Return the currently playing station index.
 */
size_t radio_station_get_playing(void);

#endif /* TANRADIO_MAIN_RADIO_RADIO_H */

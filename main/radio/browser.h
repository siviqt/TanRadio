/* Asynchronous, bounded Radio Browser search. No UI or audio ownership. */
#ifndef TANRADIO_BROWSER_H
#define TANRADIO_BROWSER_H

#include "radio/stations.h"

#define BROWSER_QUERY_LENGTH 96
#define BROWSER_RESULT_LIMIT 20

typedef struct {
    bool busy;
    size_t count;
    unsigned revision;
    char message[96];
} radio_browser_status_t;

typedef enum {
    BROWSER_SEARCH_NAME,
    BROWSER_SEARCH_COUNTRY,
} radio_browser_search_mode_t;

void radio_browser_init(void);
bool radio_browser_search(const char *query);
bool radio_browser_search_by(const char *query, radio_browser_search_mode_t mode);
void radio_browser_cancel(void);
void radio_browser_status(radio_browser_status_t *status);
bool radio_browser_result(size_t index, radio_station_t *station);
/* Best effort; never waits for a directory request on the playback caller. */
void radio_browser_count_play(const char *stationuuid);

/* Blocking lookup for the artwork worker only. */
bool radio_browser_lookup_logo(const char *uuid, char *url, size_t capacity);

#endif

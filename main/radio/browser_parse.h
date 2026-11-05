#ifndef TANRADIO_BROWSER_PARSE_H
#define TANRADIO_BROWSER_PARSE_H

#include "radio/stations.h"

/* Pure parsers shared by the device client and host regression tests. */
bool browser_parse_stations(const char *json, radio_station_t *results,
                             size_t capacity, size_t *count);
#define BROWSER_MIRROR_LIMIT 6
#define BROWSER_HOST_LENGTH 96
size_t browser_parse_srv(const uint8_t *packet, size_t length, uint16_t query_id,
                         char hosts[BROWSER_MIRROR_LIMIT][BROWSER_HOST_LENGTH]);
bool browser_encode_query(const char *query, char *encoded, size_t capacity);

bool browser_parse_logo(const char *json, const char *uuid, char *url, size_t capacity);

#endif

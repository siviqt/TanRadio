#ifndef TANRADIO_ARTWORK_H
#define TANRADIO_ARTWORK_H

#include "external_display/external_display.h"
#include "radio/stations.h"

#define STATION_LOGO_SIZE 48

typedef struct {
    uint16_t color[STATION_LOGO_SIZE * STATION_LOGO_SIZE];
    uint8_t mono[EXTERNAL_LOGO_BYTES];
} station_logo_t;

/* Submit only from the UI task. A NULL station cancels the current job. */
unsigned artwork_submit(const radio_station_t *station, bool online);
/* Copy a finished thumbnail for this request, if any. */
bool artwork_copy(unsigned request, station_logo_t *logo);

#endif

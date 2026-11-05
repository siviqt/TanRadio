#ifndef TANRADIO_NOW_PLAYING_H
#define TANRADIO_NOW_PLAYING_H

#include <stdint.h>
#include "radio/icy.h"

typedef struct {
    char title[RADIO_TITLE_SIZE];
    char name[97];
    char genre[64];
    char description[160];
    uint16_t bitrate_kbps;
} radio_metadata_t;

#endif

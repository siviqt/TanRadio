#ifndef TANRADIO_EXTERNAL_DRIVER_H
#define TANRADIO_EXTERNAL_DRIVER_H

#include "external_display/external_display.h"

typedef struct {
    bool (*open)(uint8_t contrast);
    /* Request a timed redraw of the current frame, or leave refresh_ms zero. */
    bool (*present)(const external_frame_t *frame, uint32_t now_ms, uint32_t *refresh_ms);
    void (*close)(void);
} external_driver_t;

extern const external_driver_t external_nokia5110;

#endif

#ifndef TANRADIO_EXTERNAL_DISPLAY_H
#define TANRADIO_EXTERNAL_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#define EXTERNAL_TITLE_SIZE 240
#define EXTERNAL_LOGO_SIZE 16
#define EXTERNAL_LOGO_BYTES (EXTERNAL_LOGO_SIZE * EXTERNAL_LOGO_SIZE / 8)

typedef enum {
    EXTERNAL_DISPLAY_OFF = 0,
    EXTERNAL_DISPLAY_NOKIA5110 = 1,
} external_display_id_t;

typedef enum {
    EXTERNAL_DISABLED,
    EXTERNAL_STARTING,
    EXTERNAL_INITIALIZED,
    EXTERNAL_FAILED,
} external_display_status_t;

typedef struct {
    char station[97];
    char title[EXTERNAL_TITLE_SIZE];
    char state[24];
    uint8_t battery_percent;
    uint8_t volume_percent;
    bool battery_available;
    bool battery_charging;
    bool muted;
    bool have_logo;
    uint8_t logo[EXTERNAL_LOGO_BYTES]; /* Row-major, MSB-first, 1 = dark. */
} external_frame_t;

/* UI task only. The display worker owns all driver calls. */
void external_display_submit(external_display_id_t id, uint8_t contrast,
                             const external_frame_t *frame, bool retry);
external_display_status_t external_display_status(void);
void external_display_stop(void);

#endif

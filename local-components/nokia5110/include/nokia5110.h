// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define NOKIA5110_WIDTH 84
#define NOKIA5110_HEIGHT 48
#define NOKIA5110_FRAME_BYTES 504
#define NOKIA5110_DEFAULT_CONTRAST 60

typedef enum {
    NOKIA5110_OK = 0,
    NOKIA5110_INVALID = -1,
    NOKIA5110_STATE = -2,
    NOKIA5110_IO = -3,
    NOKIA5110_BUSY = -4
} nokia5110_result_t;

// Native PCD8544 layout: pixels[x + (y / 8) * 84], bit (y % 8).
typedef struct { uint8_t pixels[NOKIA5110_FRAME_BYTES]; } nokia5110_canvas_t;
void nokia5110_clear(nokia5110_canvas_t *canvas, bool black);
void nokia5110_pixel(nokia5110_canvas_t *canvas, int x, int y, bool black);
void nokia5110_rect(nokia5110_canvas_t *canvas, int x, int y, int width, int height, bool black, bool filled);
// 5x7 glyphs in 6x8 cells; newline supported. Lowercase renders uppercase.
// Supports A-Z, 0-9, space and common punctuation; other bytes render '?'.
// Opaque drawing: the complete cell is replaced, including background.
void nokia5110_text(nokia5110_canvas_t *canvas, int x, int y, const char *text, bool black);

// begin configures pins and pulses reset. end releases pins. write sends an
// entire command/data block synchronously. Return 0 on success, nonzero on error.
typedef struct {
    void *context;
    int (*begin)(void *context);
    int (*write)(void *context, bool data, const uint8_t *bytes, size_t length);
    int (*end)(void *context);
} nokia5110_transport_t;

typedef struct {
    nokia5110_transport_t transport;
    bool initialized;
    bool sleeping;
    bool inverted;
    uint8_t contrast;
} nokia5110_t;

// Zero-initialize device before first use. Caller serializes all access.
// init and wake clear display RAM; flush a canvas afterward.
nokia5110_result_t nokia5110_init(nokia5110_t *device, const nokia5110_transport_t *transport, unsigned contrast);
nokia5110_result_t nokia5110_flush(nokia5110_t *device, const nokia5110_canvas_t *canvas);
nokia5110_result_t nokia5110_set_contrast(nokia5110_t *device, unsigned contrast);
nokia5110_result_t nokia5110_set_inverted(nokia5110_t *device, bool inverted);
nokia5110_result_t nokia5110_set_sleep(nokia5110_t *device, bool sleeping);
// Idempotent. Blanks the LCD, powers down the controller, releases GPIOs.
nokia5110_result_t nokia5110_deinit(nokia5110_t *device);
#ifdef __cplusplus
}
#endif

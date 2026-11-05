// SPDX-License-Identifier: MIT
#pragma once
#include "nokia5110.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { int clk, din, ce, dc, rst; } nokia5110_pins_t;
#define NOKIA5110_TANMATSU_PINS ((nokia5110_pins_t){.clk=2, .din=3, .ce=15, .dc=4, .rst=5})
typedef struct {
    nokia5110_t display;
    nokia5110_pins_t pins;
    unsigned configured_pins;
} nokia5110_espidf_t;
// Zero-initialize, then open after BSP initialization. One open device per app.
// GPIO assignments are customizable. LIGHT is never configured.
// Serialize calls on a device externally; do not call from interrupt handlers.
nokia5110_result_t nokia5110_espidf_open(nokia5110_espidf_t *device, nokia5110_pins_t pins, unsigned contrast);
nokia5110_result_t nokia5110_espidf_close(nokia5110_espidf_t *device);
#ifdef __cplusplus
}
#endif

// SPDX-License-Identifier: MIT
#include "nokia5110.h"
#include <string.h>

static nokia5110_result_t send(nokia5110_t *d, bool data, const uint8_t *bytes, size_t n) {
    return d->transport.write(d->transport.context, data, bytes, n) == 0 ? NOKIA5110_OK : NOKIA5110_IO;
}
static nokia5110_result_t clear_ram(nokia5110_t *d) {
    const uint8_t address[] = {0x80, 0x40};
    const uint8_t zeros[84] = {0};
    if (send(d, false, address, sizeof(address))) return NOKIA5110_IO;
    for (int bank = 0; bank < 6; bank++) if (send(d, true, zeros, sizeof(zeros))) return NOKIA5110_IO;
    return NOKIA5110_OK;
}
static nokia5110_result_t configure(nokia5110_t *d) {
    // Horizontal addressing, TC=0, bias=4, contrast as in the hardware test.
    const uint8_t commands[] = {0x21, 0x04, 0x14, (uint8_t)(0x80 | d->contrast), 0x20, 0x08};
    if (send(d, false, commands, sizeof(commands)) || clear_ram(d)) return NOKIA5110_IO;
    const uint8_t mode = d->inverted ? 0x0d : 0x0c;
    return send(d, false, &mode, 1);
}
nokia5110_result_t nokia5110_init(nokia5110_t *d, const nokia5110_transport_t *t, unsigned contrast) {
    if (!d || !t || !t->begin || !t->write || !t->end || contrast > 127) return NOKIA5110_INVALID;
    if (d->initialized) return NOKIA5110_STATE;
    d->transport = *t;
    d->contrast = contrast;
    d->inverted = d->sleeping = false;
    // begin must unwind its own partial failures (including ownership conflicts).
    if (t->begin(t->context)) return NOKIA5110_IO;
    if (configure(d)) {
        t->end(t->context);
        return NOKIA5110_IO;
    }
    d->initialized = true;
    return NOKIA5110_OK;
}
nokia5110_result_t nokia5110_flush(nokia5110_t *d, const nokia5110_canvas_t *c) {
    if (!d || !c) return NOKIA5110_INVALID;
    if (!d->initialized || d->sleeping) return NOKIA5110_STATE;
    const uint8_t address[] = {0x80, 0x40};
    if (send(d, false, address, sizeof(address))) return NOKIA5110_IO;
    return send(d, true, c->pixels, sizeof(c->pixels));
}
nokia5110_result_t nokia5110_set_contrast(nokia5110_t *d, unsigned contrast) {
    if (!d || contrast > 127) return NOKIA5110_INVALID;
    if (!d->initialized || d->sleeping) return NOKIA5110_STATE;
    const uint8_t commands[] = {0x21, (uint8_t)(0x80 | contrast), 0x20};
    if (send(d, false, commands, sizeof(commands))) return NOKIA5110_IO;
    d->contrast = contrast;
    return NOKIA5110_OK;
}
nokia5110_result_t nokia5110_set_inverted(nokia5110_t *d, bool inverted) {
    if (!d) return NOKIA5110_INVALID;
    if (!d->initialized || d->sleeping) return NOKIA5110_STATE;
    uint8_t mode = inverted ? 0x0d : 0x0c;
    if (send(d, false, &mode, 1)) return NOKIA5110_IO;
    d->inverted = inverted;
    return NOKIA5110_OK;
}
nokia5110_result_t nokia5110_set_sleep(nokia5110_t *d, bool sleeping) {
    if (!d) return NOKIA5110_INVALID;
    if (!d->initialized) return NOKIA5110_STATE;
    if (d->sleeping == sleeping) return NOKIA5110_OK;
    if (sleeping) {
        if (clear_ram(d)) return NOKIA5110_IO;
        const uint8_t commands[] = {0x08, 0x24};
        if (send(d, false, commands, sizeof(commands))) return NOKIA5110_IO;
    } else if (configure(d)) return NOKIA5110_IO;
    d->sleeping = sleeping;
    return NOKIA5110_OK;
}
nokia5110_result_t nokia5110_deinit(nokia5110_t *d) {
    if (!d) return NOKIA5110_INVALID;
    if (!d->initialized) return NOKIA5110_OK;
    nokia5110_result_t result = nokia5110_set_sleep(d, true);
    if (d->transport.end(d->transport.context)) result = NOKIA5110_IO;
    memset(d, 0, sizeof(*d));
    return result;
}

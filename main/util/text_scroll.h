#ifndef TANRADIO_TEXT_SCROLL_H
#define TANRADIO_TEXT_SCROLL_H

#include <stdint.h>

typedef struct {
    unsigned offset;
    uint32_t until_next;
} text_scroll_t;

/* Walk from the first position to the last, pause at both ends, then repeat.
 * All time arguments use the caller's clock units. Zero delay means static. */
static inline text_scroll_t text_scroll_at(uint32_t elapsed, unsigned last,
                                          uint32_t step, uint32_t pause)
{
    if (!last) return (text_scroll_t){0};
    uint32_t travel = (last - 1) * step;
    uint32_t phase = elapsed % (2 * pause + travel);
    if (phase < pause) return (text_scroll_t){0, pause - phase};
    phase -= pause;
    if (phase < travel) return (text_scroll_t){1 + phase / step, step - phase % step};
    return (text_scroll_t){last, pause + travel - phase};
}

#endif

#ifndef TANRADIO_WELCOME_H
#define TANRADIO_WELCOME_H

#include "pax_gfx.h"

/* Native 800 x 480 Tanmatsu welcome layout. */
void welcome_draw(pax_buf_t *buffer, const pax_buf_t *logo);
void welcome_draw_exit(pax_buf_t *buffer, const pax_buf_t *logo);

#endif

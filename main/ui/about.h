#ifndef TANRADIO_UI_ABOUT_H
#define TANRADIO_UI_ABOUT_H

#include "bsp/input.h"

/* A distinct Hyper/Super tap toggles About. Return true for consumed keys. */
bool ui_about_navigation(bool *open, bsp_input_navigation_key_t key);

#endif

#ifndef TANRADIO_UI_EXIT_H
#define TANRADIO_UI_EXIT_H

#include "bsp/input.h"

typedef enum {
    UI_EXIT_IDLE,
    UI_EXIT_CONFIRM,
    UI_EXIT_SETTINGS,
    UI_EXIT_COMMITTED,
} ui_exit_state_t;

/* Called only for distinct key presses, after input_keys_event filtering.
 * True consumes the key before the underlying screen can act on it. */
bool ui_exit_navigation(ui_exit_state_t *state, bsp_input_navigation_key_t key);

#endif

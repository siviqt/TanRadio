#include "ui/exit.h"

bool ui_exit_navigation(ui_exit_state_t *state, bsp_input_navigation_key_t key)
{
    if (*state == UI_EXIT_COMMITTED) return true;
    if (key == BSP_INPUT_NAVIGATION_KEY_F1 && *state != UI_EXIT_SETTINGS) {
        *state = *state == UI_EXIT_IDLE ? UI_EXIT_CONFIRM : UI_EXIT_COMMITTED;
        return true;
    }
    if (*state == UI_EXIT_IDLE) return false;
    if (key == BSP_INPUT_NAVIGATION_KEY_ESC || key == BSP_INPUT_NAVIGATION_KEY_F2) {
        *state = *state == UI_EXIT_SETTINGS ? UI_EXIT_CONFIRM : UI_EXIT_IDLE;
        return true;
    }
    if (key == BSP_INPUT_NAVIGATION_KEY_F3 && *state == UI_EXIT_CONFIRM) {
        *state = UI_EXIT_SETTINGS;
        return true;
    }
    /* Volume remains available without changing the screen or playback. */
    return key != BSP_INPUT_NAVIGATION_KEY_VOLUME_UP &&
           key != BSP_INPUT_NAVIGATION_KEY_VOLUME_DOWN;
}

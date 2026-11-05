#include "ui/about.h"

bool ui_about_navigation(bool *open, bsp_input_navigation_key_t key)
{
    if (key == BSP_INPUT_NAVIGATION_KEY_SUPER) {
        *open = !*open;
        return true;
    }
    if (!*open) return false;
    if (key == BSP_INPUT_NAVIGATION_KEY_ESC || key == BSP_INPUT_NAVIGATION_KEY_F2) {
        *open = false;
        return true;
    }
    /* Exit confirmation, illumination and volume keep their global bindings. */
    return key != BSP_INPUT_NAVIGATION_KEY_F1 &&
           key != BSP_INPUT_NAVIGATION_KEY_F6 &&
           key != BSP_INPUT_NAVIGATION_KEY_VOLUME_UP &&
           key != BSP_INPUT_NAVIGATION_KEY_VOLUME_DOWN;
}

#ifndef TANRADIO_UI_SHORTCUTS_H
#define TANRADIO_UI_SHORTCUTS_H

#include "pax_gfx.h"

typedef enum {
    UI_KEY_F1, UI_KEY_F2, UI_KEY_F3, UI_KEY_F4, UI_KEY_F5, UI_KEY_F6,
    UI_KEY_VOLUME_UP, UI_KEY_VOLUME_DOWN,
    UI_KEY_UP, UI_KEY_DOWN, UI_KEY_LEFT, UI_KEY_RIGHT,
    UI_KEY_TAB, UI_KEY_ENTER, UI_KEY_BACKSPACE, UI_KEY_SPACE, UI_KEY_HYPER,
    UI_KEY_NONE,
} ui_shortcut_key_t;

#define UI_KEYS(...) (const ui_shortcut_key_t[]){__VA_ARGS__, UI_KEY_NONE}

void ui_shortcuts_load(void);

/* Draw key icons and an action at a common vertical center. Return the right edge. */
float ui_draw_shortcut(pax_buf_t *buffer, const ui_shortcut_key_t *keys, const char *action,
                       float x, float center_y, pax_col_t color, float text_size);

#endif

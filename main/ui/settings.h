#ifndef TANRADIO_UI_SETTINGS_H
#define TANRADIO_UI_SETTINGS_H

#include "bsp/input.h"
#include "external_display/external_display.h"
#include "pax_gfx.h"

enum {
    UI_LED_BRIGHTNESS_DEFAULT = 50,
    UI_LED_BRIGHTNESS_STEP = 5,
    UI_LED_BRIGHTNESS_MAX = 100,
};

typedef struct {
    external_display_id_t display;
    uint8_t contrast;
    bool online_logos;
    uint8_t led_brightness;
} ui_preferences_t;

enum {
    UI_SETTING_DISPLAY,
    UI_SETTING_CONTRAST,
    UI_SETTING_LOGOS,
    UI_SETTING_LED_BRIGHTNESS,
    UI_SETTING_COUNT,
};

typedef struct {
    ui_preferences_t draft;
    unsigned row;
    const char *notice;
} ui_settings_t;

void ui_preferences_load(ui_preferences_t *preferences);
bool ui_preferences_save(const ui_preferences_t *preferences);
bool ui_settings_edit(ui_settings_t *settings, bsp_input_navigation_key_t key);
void ui_settings_draw(pax_buf_t *buffer, const ui_settings_t *settings, bool services_started);

#endif

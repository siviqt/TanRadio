#include "ui/settings.h"
#include "ui/shortcuts.h"
#include "ui/theme.h"

#include <stdio.h>
#include "nvs.h"

static const char *const labels[UI_SETTING_COUNT] = {
    "External display", "Contrast", "Download station logos", "LED brightness", "Theme",
};

void ui_preferences_load(ui_preferences_t *preferences)
{
    *preferences = (ui_preferences_t){.contrast = 60, .led_brightness = UI_LED_BRIGHTNESS_DEFAULT};
    nvs_handle_t handle;
    if (nvs_open("tanradio_ui", NVS_READONLY, &handle) != ESP_OK) return;
    /* Leave legacy options intact so older firmware can restore its preferences.
     * Byte 3 is reserved. LED brightness and theme are bytes 4 and 5. */
    uint8_t values[6];
    size_t size = sizeof(values);
    bool loaded = nvs_get_blob(handle, "options_v2", values, &size) == ESP_OK &&
                  size == sizeof(values);
    if (!loaded) {
        size = 5;
        loaded = nvs_get_blob(handle, "options", values, &size) == ESP_OK && size >= 3;
    }
    if (loaded) {
        if (values[0] == EXTERNAL_DISPLAY_NOKIA5110)
            preferences->display = EXTERNAL_DISPLAY_NOKIA5110;
        if (values[1] <= 127) preferences->contrast = values[1];
        preferences->online_logos = values[2] == 1;
        if (size >= 5 && values[4] <= UI_LED_BRIGHTNESS_MAX && values[4] % UI_LED_BRIGHTNESS_STEP == 0)
            preferences->led_brightness = values[4];
        if (size == sizeof(values) && values[5] == UI_THEME_VIVID)
            preferences->theme = UI_THEME_VIVID;
    }
    nvs_close(handle);
}

bool ui_preferences_save(const ui_preferences_t *preferences)
{
    nvs_handle_t handle;
    if (nvs_open("tanradio_ui", NVS_READWRITE, &handle) != ESP_OK) return false;
    const uint8_t values[] = {
        preferences->display, preferences->contrast, preferences->online_logos, 0,
        preferences->led_brightness, preferences->theme,
    };
    esp_err_t result = nvs_set_blob(handle, "options_v2", values, sizeof(values));
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result == ESP_OK;
}

bool ui_settings_edit(ui_settings_t *settings, bsp_input_navigation_key_t key)
{
    bool have_display = settings->draft.display != EXTERNAL_DISPLAY_OFF;
    if (key == BSP_INPUT_NAVIGATION_KEY_UP || key == BSP_INPUT_NAVIGATION_KEY_DOWN) {
        unsigned step = key == BSP_INPUT_NAVIGATION_KEY_UP ? UI_SETTING_COUNT - 1 : 1;
        do {
            settings->row = (settings->row + step) % UI_SETTING_COUNT;
        } while (settings->row == UI_SETTING_CONTRAST && !have_display);
        return true;
    }
    if (key != BSP_INPUT_NAVIGATION_KEY_LEFT && key != BSP_INPUT_NAVIGATION_KEY_RIGHT &&
        key != BSP_INPUT_NAVIGATION_KEY_RETURN) return false;
    if (settings->row == UI_SETTING_DISPLAY) {
        settings->draft.display = have_display ? EXTERNAL_DISPLAY_OFF : EXTERNAL_DISPLAY_NOKIA5110;
    } else if (settings->row == UI_SETTING_CONTRAST) {
        int contrast = settings->draft.contrast;
        if (key == BSP_INPUT_NAVIGATION_KEY_RETURN) return false;
        contrast += key == BSP_INPUT_NAVIGATION_KEY_LEFT ? -1 : 1;
        if (contrast < 0 || contrast > 127) return false;
        settings->draft.contrast = contrast;
    } else if (settings->row == UI_SETTING_LOGOS) {
        settings->draft.online_logos = !settings->draft.online_logos;
    } else if (settings->row == UI_SETTING_THEME) {
        settings->draft.theme = settings->draft.theme == UI_THEME_VIVID ? UI_THEME_MIDNIGHT : UI_THEME_VIVID;
    } else if (settings->row == UI_SETTING_LED_BRIGHTNESS) {
        if (key == BSP_INPUT_NAVIGATION_KEY_RETURN) return false;
        int brightness = settings->draft.led_brightness;
        brightness += key == BSP_INPUT_NAVIGATION_KEY_LEFT ? -UI_LED_BRIGHTNESS_STEP : UI_LED_BRIGHTNESS_STEP;
        if (brightness < 0 || brightness > UI_LED_BRIGHTNESS_MAX) return false;
        settings->draft.led_brightness = brightness;
    }
    settings->notice = NULL;
    settings->retry = false;
    return true;
}

void ui_settings_draw(pax_buf_t *buffer, const ui_settings_t *settings, bool services_started)
{
    const float text_x = 136;
    bool nokia = settings->draft.display == EXTERNAL_DISPLAY_NOKIA5110;
    char contrast[8], brightness[8];
    snprintf(contrast, sizeof(contrast), "%u", settings->draft.contrast);
    snprintf(brightness, sizeof(brightness), "%u%%", settings->draft.led_brightness);
    const char *values[] = {nokia ? "Nokia 5110" : "Off", contrast,
                            settings->draft.online_logos ? "On" : "Off", brightness,
                            settings->draft.theme == UI_THEME_VIVID ? "Vivid" : "Midnight"};
    pax_draw_round_rect(buffer, UI_COLOR_BORDER, 96, 72, 608, 335, 14);
    pax_draw_round_rect(buffer, UI_COLOR_PANEL, 98, 74, 604, 331, 12);
    pax_draw_text(buffer, UI_COLOR_TEXT, UI_FONT_HEADING, UI_TEXT_TITLE, text_x, 90, "TanRadio Settings");
    for (unsigned row = 0; row < sizeof(labels) / sizeof(labels[0]); row++) {
        if (row == UI_SETTING_CONTRAST && !nokia) continue;
        float y = 138 + row * 29;
        if (settings->row == row)
            pax_draw_round_rect(buffer, UI_COLOR_SELECTION, 124, y - 2, 548, 27, 5);
        pax_draw_text(buffer, UI_COLOR_TEXT, UI_FONT_BODY, UI_TEXT_BODY, text_x, y, labels[row]);
        pax_draw_text(buffer, UI_COLOR_ACCENT, UI_FONT_BODY, UI_TEXT_BODY, 468, y, values[row]);
    }

    const char *display_status = NULL;
    bool retry = settings->retry;
    if (!services_started) {
        if (nokia) {
            ui_draw_shortcut(buffer, UI_KEYS(UI_KEY_SPACE), "Starts the external display from Welcome",
                             text_x, 302, UI_COLOR_SECONDARY, UI_TEXT_SMALL);
        }
    } else {
        switch (external_display_status()) {
        case EXTERNAL_DISABLED:    display_status = "External display off"; break;
        case EXTERNAL_STARTING:    display_status = "Initializing external display"; break;
        case EXTERNAL_INITIALIZED: display_status = "Nokia initialized · connection unverified"; break;
        case EXTERNAL_FAILED:
            display_status = "External display error";
            retry |= nokia;
            break;
        }
    }
    if (display_status)
        pax_draw_text(buffer, UI_COLOR_SECONDARY, UI_FONT_BODY, UI_TEXT_SMALL, text_x, 293, display_status);
    if (settings->notice)
        pax_draw_text(buffer, UI_COLOR_SECONDARY, UI_FONT_BODY, UI_TEXT_SMALL, text_x, 315, settings->notice);

    const float footer_y = 366;
    float next_x = ui_draw_shortcut(buffer, UI_KEYS(UI_KEY_UP, UI_KEY_DOWN), "Select",
                                    text_x, footer_y, UI_COLOR_TEXT, UI_TEXT_SMALL);
    next_x = ui_draw_shortcut(buffer, UI_KEYS(UI_KEY_LEFT, UI_KEY_RIGHT), "Edit",
                              next_x + 22, footer_y, UI_COLOR_TEXT, UI_TEXT_SMALL);
    next_x = ui_draw_shortcut(buffer, UI_KEYS(UI_KEY_F4), retry ? "Retry" : "Apply",
                              next_x + 22, footer_y, UI_COLOR_TEXT, UI_TEXT_SMALL);
    ui_draw_shortcut(buffer, UI_KEYS(UI_KEY_F2), "Back",
                     next_x + 22, footer_y, UI_COLOR_TEXT, UI_TEXT_SMALL);
}

#include "ui/settings.h"
#include "ui/theme.h"

#include <stdio.h>
#include "nvs.h"

static const char *const labels[UI_SETTING_COUNT] = {
    "External display", "Contrast", "Download station logos", "LED brightness",
};

void ui_preferences_load(ui_preferences_t *preferences)
{
    *preferences = (ui_preferences_t){.contrast = 60, .led_brightness = UI_LED_BRIGHTNESS_DEFAULT};
    nvs_handle_t handle;
    if (nvs_open("tanradio_ui", NVS_READONLY, &handle) != ESP_OK) return;
    /* Byte 3 belongs to the withdrawn revision 14. LED brightness is byte 4. */
    uint8_t values[5];
    size_t size = sizeof(values);
    if (nvs_get_blob(handle, "options", values, &size) == ESP_OK && size >= 3) {
        if (values[0] == EXTERNAL_DISPLAY_NOKIA5110)
            preferences->display = EXTERNAL_DISPLAY_NOKIA5110;
        if (values[1] <= 127) preferences->contrast = values[1];
        preferences->online_logos = values[2] == 1;
        if (size == sizeof(values) && values[4] <= UI_LED_BRIGHTNESS_MAX && values[4] % UI_LED_BRIGHTNESS_STEP == 0)
            preferences->led_brightness = values[4];
    }
    nvs_close(handle);
}

bool ui_preferences_save(const ui_preferences_t *preferences)
{
    nvs_handle_t handle;
    if (nvs_open("tanradio_ui", NVS_READWRITE, &handle) != ESP_OK) return false;
    const uint8_t values[] = {
        preferences->display, preferences->contrast, preferences->online_logos, 0, preferences->led_brightness,
    };
    esp_err_t result = nvs_set_blob(handle, "options", values, sizeof(values));
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
    } else {
        if (key == BSP_INPUT_NAVIGATION_KEY_RETURN) return false;
        int brightness = settings->draft.led_brightness;
        brightness += key == BSP_INPUT_NAVIGATION_KEY_LEFT ? -UI_LED_BRIGHTNESS_STEP : UI_LED_BRIGHTNESS_STEP;
        if (brightness < 0 || brightness > UI_LED_BRIGHTNESS_MAX) return false;
        settings->draft.led_brightness = brightness;
    }
    settings->notice = NULL;
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
                            settings->draft.online_logos ? "On" : "Off", brightness};
    pax_draw_round_rect(buffer, UI_COLOR_BORDER, 96, 72, 608, 335, 14);
    pax_draw_round_rect(buffer, UI_COLOR_PANEL, 98, 74, 604, 331, 12);
    pax_draw_text(buffer, UI_COLOR_TEXT, UI_FONT_HEADING, UI_TEXT_TITLE, text_x, 90, "TanRadio Settings");
    for (unsigned row = 0; row < sizeof(labels) / sizeof(labels[0]); row++) {
        if (row == UI_SETTING_CONTRAST && !nokia) continue;
        float y = 139 + row * 35;
        if (settings->row == row)
            pax_draw_round_rect(buffer, UI_COLOR_SELECTION, 124, y - 3, 548, 31, 5);
        pax_draw_text(buffer, UI_COLOR_TEXT, UI_FONT_BODY, UI_TEXT_BODY, text_x, y, labels[row]);
        pax_draw_text(buffer, UI_COLOR_ACCENT, UI_FONT_BODY, UI_TEXT_BODY, 468, y, values[row]);
    }

    const char *display_status = NULL;
    if (!services_started) {
        if (nokia) display_status = "The display starts when you press Space.";
    } else {
        switch (external_display_status()) {
        case EXTERNAL_DISABLED:    display_status = "External display off"; break;
        case EXTERNAL_STARTING:    display_status = "Initializing external display"; break;
        case EXTERNAL_INITIALIZED: display_status = "Nokia initialized · connection unverified"; break;
        case EXTERNAL_FAILED:      display_status = "Display error · press F4 to retry"; break;
        }
    }
    if (display_status)
        pax_draw_text(buffer, UI_COLOR_SECONDARY, UI_FONT_BODY, UI_TEXT_SMALL, text_x, 287, display_status);
    if (settings->notice)
        pax_draw_text(buffer, UI_COLOR_SECONDARY, UI_FONT_BODY, UI_TEXT_SMALL, text_x, 315, settings->notice);
    pax_draw_text(buffer, UI_COLOR_TEXT, UI_FONT_BODY, UI_TEXT_SMALL, text_x, 356,
                  "Arrows to edit    F4 Apply    F2 / Esc Back");
}

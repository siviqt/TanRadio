#include "ui/about_view.h"
#include "ui/theme.h"
#include "tanradio_build_info.h"
#include "esp_app_desc.h"
#include "pax_text.h"
#include "sdkconfig.h"

#if defined(CONFIG_BSP_TARGET_TANMATSU)
#include "dsi_panel_nicolaielectronics_st7701.h"
#endif

#include <stdio.h>
#include <stdint.h>

static void about_display_value(char *value, size_t length)
{
#if defined(CONFIG_BSP_TARGET_TANMATSU)
    uint8_t id[3];
    if (st7701_get_id(id) == ESP_OK) {
        snprintf(value, length, "v%u (ID %02X %02X %02X)",
                 st7701_is_display_variant_2() ? 2u : 1u,
                 (unsigned)id[0], (unsigned)id[1], (unsigned)id[2]);
        return;
    }
#endif
    snprintf(value, length, "Unknown");
}

static void about_row(pax_buf_t *buffer, float y, const char *label, const char *value)
{
    pax_draw_text(buffer, UI_COLOR_SECONDARY, UI_FONT_BODY, UI_TEXT_SMALL, 100, y, label);
    pax_draw_text(buffer, UI_COLOR_TEXT, UI_FONT_BODY, UI_TEXT_SMALL, 260, y, value);
}

void ui_about_draw(pax_buf_t *buffer)
{
    const esp_app_desc_t *app = esp_app_get_description();
    char version[64], built[48], build_id[17], display[40];
    snprintf(version, sizeof(version), "Version %s   Revision %u",
             TANRADIO_APP_VERSION, TANRADIO_APP_REVISION);
    snprintf(built, sizeof(built), "%s  %s", app->date, app->time);
    about_display_value(display, sizeof(display));
    for (unsigned i = 0; i < 8; i++) {
        snprintf(build_id + i * 2, 3, "%02x", app->app_elf_sha256[i]);
    }

    pax_draw_round_rect(buffer, UI_COLOR_BORDER, 72, 64, 656, 376, 14);
    pax_draw_round_rect(buffer, UI_COLOR_PANEL, 74, 66, 652, 372, 12);
    pax_draw_text(buffer, UI_COLOR_TEXT, UI_FONT_HEADING, 36, 100, 80, "About TanRadio");
    pax_draw_text(buffer, UI_COLOR_ACCENT, UI_FONT_BODY, 24, 100, 130, version);
    about_row(buffer, 177, "Build", app->version);
    about_row(buffer, 207, "ESP-IDF", app->idf_ver);
    about_row(buffer, 237, "BSP / LCD / DAC",
              TANRADIO_BSP_VERSION " / " TANRADIO_LCD_VERSION " / " TANRADIO_CODEC_VERSION);
    about_row(buffer, 267, "Display", display);
    about_row(buffer, 297, "Built", built);
    about_row(buffer, 327, "Build ID", build_id);
    pax_draw_line(buffer, UI_COLOR_BORDER, 100, 366, 700, 366);
    pax_draw_text(buffer, UI_COLOR_SECONDARY, UI_FONT_BODY, UI_TEXT_SMALL,
                  100, 392, "Hyper / Esc / F2: Close");
}

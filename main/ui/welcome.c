#include "welcome.h"
#include "pax_fonts.h"
#include "pax_text.h"
#include "ui/shortcuts.h"
#include "theme.h"

static void session_draw(pax_buf_t *buffer, const pax_buf_t *logo)
{
    pax_background(buffer, UI_COLOR_BACKGROUND);
    if (logo != NULL) {
        /* Asset is prepared at its exact on-screen size (250 x 480). */
        pax_shade_rect(buffer, 0xFFFFFFFF, &PAX_SHADER_TEXTURE_OP(logo),
            NULL, 0, 0, 250, 480);
    }
    pax_draw_line(buffer, UI_COLOR_BORDER, 250, 36, 250, 444);
    pax_draw_text(buffer, UI_COLOR_TEXT, UI_FONT_HEADING,
        42, 278, 30, "TanRadio");
}

void welcome_draw(pax_buf_t *buffer, const pax_buf_t *logo)
{
    session_draw(buffer, logo);
    pax_draw_text(buffer, UI_COLOR_ACCENT, UI_FONT_BODY,
        UI_TEXT_BODY, 280, 87, "Internet radio for your Tanmatsu.");

    pax_draw_text(buffer, UI_COLOR_SECONDARY, UI_FONT_BODY,
        UI_TEXT_BODY, 280, 143,
        "Configure Wi-Fi in the Tanmatsu launcher\n"
        "before starting. TanRadio connects using\n"
        "your saved networks.");

    ui_draw_shortcut(buffer, UI_KEYS(UI_KEY_F2), "Find and add radio stations",
                     274, 249, UI_COLOR_SECONDARY, UI_TEXT_BODY);
    ui_draw_shortcut(buffer, UI_KEYS(UI_KEY_F4), "Play, mute or unmute your selection",
                     274, 280, UI_COLOR_SECONDARY, UI_TEXT_BODY);
    pax_draw_text(buffer, UI_COLOR_SECONDARY, UI_FONT_BODY,
        UI_TEXT_BODY, 280, 302,
        "You can also edit /tanradio/stations.txt\n"
        "on the SD card while TanRadio is closed.\n"
        "An SD card is required.");

    pax_draw_round_rect(buffer, UI_COLOR_ACCENT, 280, 391, 492, 53, 10);
    pax_draw_round_rect(buffer, UI_COLOR_SELECTION, 282, 393, 488, 49, 8);
    ui_draw_shortcut(buffer, UI_KEYS(UI_KEY_SPACE), "Start TanRadio",
                     398, 417, UI_COLOR_TEXT, 26);
    float next_x = ui_draw_shortcut(buffer, UI_KEYS(UI_KEY_F1), "Options/Exit",
                                    274, 461, UI_COLOR_SECONDARY, UI_TEXT_SMALL);
    ui_draw_shortcut(buffer, UI_KEYS(UI_KEY_HYPER), "About",
                     next_x + 22, 461, UI_COLOR_SECONDARY, UI_TEXT_SMALL);
}

void welcome_draw_exit(pax_buf_t *buffer, const pax_buf_t *logo)
{
    session_draw(buffer, logo);
    pax_draw_text(buffer, UI_COLOR_ACCENT, UI_FONT_HEADING,
        38, 280, 172, "Thanks for listening.");
    pax_draw_text(buffer, UI_COLOR_TEXT, UI_FONT_BODY,
        26, 280, 236, "Returning to launcher...");
}

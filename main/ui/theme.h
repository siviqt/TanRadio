#ifndef TANRADIO_UI_THEME_H
#define TANRADIO_UI_THEME_H

#include <stdbool.h>
#include <stdint.h>
#include "pax_fonts.h"

typedef struct {
    uint32_t background, panel, field, border;
    uint32_t text, secondary, disabled;
    uint32_t accent, selection, success, warning, error;
} ui_palette_t;

/* The UI mutex serializes theme changes and rendering after welcome. */
extern const ui_palette_t *ui_palette;
void ui_theme_select(bool vivid);

#define UI_COLOR_BACKGROUND (ui_palette->background)
#define UI_COLOR_PANEL      (ui_palette->panel)
#define UI_COLOR_FIELD      (ui_palette->field)
#define UI_COLOR_BORDER     (ui_palette->border)
#define UI_COLOR_TEXT       (ui_palette->text)
#define UI_COLOR_SECONDARY  (ui_palette->secondary)
#define UI_COLOR_DISABLED   (ui_palette->disabled)
#define UI_COLOR_ACCENT     (ui_palette->accent)
#define UI_COLOR_SELECTION  (ui_palette->selection)
#define UI_COLOR_SUCCESS    (ui_palette->success)
#define UI_COLOR_WARNING    (ui_palette->warning)
#define UI_COLOR_ERROR      (ui_palette->error)

#define UI_FONT_BODY    pax_font_saira_regular
#define UI_FONT_HEADING pax_font_saira_condensed
#define UI_TEXT_SMALL   18
#define UI_TEXT_BODY    22
#define UI_TEXT_TITLE   32
#define UI_TEXT_STATION 36

#endif

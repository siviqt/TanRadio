#include "ui/theme.h"

static const ui_palette_t midnight = {
    .background = 0xFF101820,
    .panel = 0xFF1C2A36,
    .field = 0xFF263D4C,
    .border = 0xFF405666,
    .text = 0xFFF1F5F9,
    .secondary = 0xFFB8C8D6,
    .disabled = 0xFF93A8B8,
    .accent = 0xFF62DFFF,
    .selection = 0xFF173F50,
    .success = 0xFF83DBAC,
    .warning = 0xFFFFD28A,
    .error = 0xFFFFA3AB,
};

static const ui_palette_t vivid = {
    .background = 0xFF0C1426,
    .panel = 0xFF192747,
    .field = 0xFF233963,
    .border = 0xFF627CAD,
    .text = 0xFFF1F5F9,
    .secondary = 0xFFC5D6F2,
    .disabled = 0xFF96AAC7,
    .accent = 0xFF00E5FF,
    .selection = 0xFF153E85,
    .success = 0xFF43EC9C,
    .warning = 0xFFFFCC46,
    .error = 0xFFFF8DA0,
};

const ui_palette_t *ui_palette = &midnight;

void ui_theme_select(bool enabled)
{
    ui_palette = enabled ? &vivid : &midnight;
}

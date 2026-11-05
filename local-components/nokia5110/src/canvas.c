// SPDX-License-Identifier: MIT
#include "nokia5110.h"
#include <string.h>
#include <limits.h>
#include "font.inc"
void nokia5110_clear(nokia5110_canvas_t *c, bool black) {
    if (c) memset(c->pixels, black ? 0xff : 0, sizeof(c->pixels));
}
void nokia5110_pixel(nokia5110_canvas_t *c, int x, int y, bool black) {
    if (!c || x < 0 || x >= 84 || y < 0 || y >= 48) return;
    uint8_t *p = &c->pixels[x + (y / 8) * 84];
    if (black) *p |= 1u << (y % 8);
    else *p &= ~(1u << (y % 8));
}
void nokia5110_rect(nokia5110_canvas_t *c, int x, int y, int w, int h, bool black, bool filled) {
    if (!c || w <= 0 || h <= 0) return;
    // Wide arithmetic prevents overflow even for extreme off-screen rectangles.
    int64_t right = (int64_t)x + w - 1, bottom = (int64_t)y + h - 1;
    for (int py = 0; py < 48; py++) for (int px = 0; px < 84; px++) {
        if (px < x || px > right || py < y || py > bottom) continue;
        if (filled || px == x || px == right || py == y || py == bottom)
            nokia5110_pixel(c, px, py, black);
    }
}
void nokia5110_text(nokia5110_canvas_t *c, int x, int y, const char *text, bool black) {
    if (!c || !text) return;
    int64_t cx = x, cy = y;
    for (; *text; text++) {
        if (*text == '\n') { cx = x; cy += 8; if (cy >= 48) break; continue; }
        unsigned char ch = (unsigned char)*text;
        if (ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
        const uint8_t *glyph = NULL, *fallback = NULL;
        for (size_t i = 0; i < sizeof(font)/sizeof(font[0]); i++) {
            if (font[i].character == '?') fallback = font[i].columns;
            if (font[i].character == ch) glyph = font[i].columns;
        }
        if (!glyph) glyph = fallback;
        for (int dy = 0; dy < 8; dy++) for (int dx = 0; dx < 6; dx++) {
            int64_t px = cx + dx, py = cy + dy;
            if (px < 0 || px >= 84 || py < 0 || py >= 48) continue;
            bool ink = dx < 5 && dy < 7 && ((glyph[dx] >> dy) & 1);
            nokia5110_pixel(c, (int)px, (int)py, ink ? black : !black);
        }
        // No automatic wrapping. Saturate far off-screen positions.
        if (cx < INT_MAX) cx += 6;
    }
}

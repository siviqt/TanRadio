#include "external_display/driver.h"
#include "bsp/catt.h"
#include "nokia5110_espidf.h"
#include "util/text_scroll.h"

#include <string.h>

#define NOKIA_SCROLL_STEP_MS 1300
#define NOKIA_SCROLL_PAUSE_MS 2200

typedef struct {
    char source[EXTERNAL_TITLE_SIZE];
    uint32_t started_ms;
} nokia_scroll_t;

static nokia5110_espidf_t lcd;
static nokia5110_canvas_t last;
static bool presented;
static nokia_scroll_t station_scroll, title_scroll;

static bool nokia_open(uint8_t contrast)
{
    nokia5110_pins_t pins = {
        .rst = bsp_catt_get_gpio(CATT_PIN_D2),
        .ce  = bsp_catt_get_gpio(CATT_PIN_D5),
        .dc  = bsp_catt_get_gpio(CATT_PIN_D6),
        .din = bsp_catt_get_gpio(CATT_PIN_D3),
        .clk = bsp_catt_get_gpio(CATT_PIN_D7),
    };
    presented = false;
    station_scroll = (nokia_scroll_t){0};
    title_scroll = (nokia_scroll_t){0};
    return nokia5110_espidf_open(&lcd, pins, contrast) == NOKIA5110_OK;
}

static unsigned nokia_codepoint(const unsigned char **cursor)
{
    const unsigned char *text = *cursor;
    unsigned code = *text++;
    if (code >= 0x80) {
        unsigned length = code < 0xc2 || code > 0xf4 ? 0 :
                          code < 0xe0 ? 2 : code < 0xf0 ? 3 : 4;
        code = length ? code & (0x7f >> length) : '?';
        for (unsigned i = 1; i < length; i++) {
            if ((*text & 0xc0) != 0x80) {
                code = '?';
                break;
            }
            code = (code << 6) | (*text++ & 0x3f);
        }
    }
    *cursor = text;
    return code;
}

static const char *nokia_transliterate(unsigned code)
{
    /* Bulgarian station names and punctuation commonly used in ICY titles. */
    static const char *const cyrillic[] = {
        "A", "B", "V", "G", "D", "E", "ZH", "Z", "I", "Y", "K", "L", "M", "N",
        "O", "P", "R", "S", "T", "U", "F", "H", "TS", "CH", "SH", "SHT", "A",
        "Y", "Y", "E", "YU", "YA",
    };
    if (code >= 0x410 && code <= 0x44f) return cyrillic[(code - 0x410) % 32];
    switch (code) {
        case 0x401: case 0x451: return "YO";
        case 0xa0: case 0x2009: case 0x202f: return " ";
        case 0x2010: case 0x2011: case 0x2012: case 0x2013:
        case 0x2014: case 0x2015: case 0x2212: return "-";
        case 0x2018: case 0x2019: case 0x201a: case 0x201b: case 0x2032: return "'";
        case 0xab: case 0xbb: case 0x201c: case 0x201d:
        case 0x201e: case 0x201f: case 0x2033: return "\"";
        case 0xb7: case 0x2022: return ".";
        case 0x2026: return "...";
        case 0xc6: case 0xe6: return "AE";
        case 0xde: case 0xfe: return "TH";
        case 0xa9: return "(C)";
        case 0xae: return "(R)";
        case 0x2116: return "No.";
        case 0x2122: return "TM";
        default: return "?";
    }
}

static void nokia_ascii(char *out, size_t capacity, const char *input)
{
    /* Basic Latin-1 accents use the corresponding letter in the 5x7 font. */
    static const char latin1[] =
        "AAAAAA CEEEEIIII"
        "DNOOOOOxOUUUUY S"
        "AAAAAA CEEEEIIII"
        "DNOOOOO/OUUUUY Y";
    const unsigned char *text = (const unsigned char *)input;
    while (*text && capacity > 1) {
        unsigned code = nokia_codepoint(&text);
        if (code < 0x80) {
            *out++ = code >= 0x20 && code <= 0x7e ? (char)code : ' ';
            capacity--;
        } else if (code >= 0xc0 && code <= 0xff && latin1[code - 0xc0] != ' ') {
            *out++ = latin1[code - 0xc0];
            capacity--;
        } else {
            const char *glyph = nokia_transliterate(code);
            while (*glyph && capacity > 1) {
                *out++ = *glyph++;
                capacity--;
            }
        }
    }
    *out = '\0';
}

static bool nokia_next_row(const char **cursor, unsigned columns, char *line)
{
    const char *text = *cursor;
    while (*text == ' ') text++;
    size_t length = strlen(text);
    if (!length) return false;
    if (length > columns) {
        length = columns;
        while (length && text[length] != ' ') length--;
        if (!length) length = columns;
    }
    if (line) {
        memcpy(line, text, length);
        line[length] = '\0';
    }
    *cursor = text + length;
    return true;
}

static void nokia_draw_scrolled(nokia5110_canvas_t *canvas, nokia_scroll_t *label,
                                const char *source, const char *text, uint32_t now_ms,
                                uint32_t *refresh_ms, int row, unsigned rows, unsigned columns)
{
    if (strcmp(source, label->source)) {
        memcpy(label->source, source, strlen(source) + 1);
        label->started_ms = now_ms;
    }

    const char *cursor = text;
    unsigned lines = 0;
    while (nokia_next_row(&cursor, columns, NULL)) lines++;
    text_scroll_t scroll = text_scroll_at(now_ms - label->started_ms,
                                          lines > rows ? lines - rows : 0,
                                          NOKIA_SCROLL_STEP_MS, NOKIA_SCROLL_PAUSE_MS);
    if (scroll.until_next && (!*refresh_ms || scroll.until_next < *refresh_ms))
        *refresh_ms = scroll.until_next;

    for (unsigned i = 0; i < scroll.offset; i++) nokia_next_row(&text, columns, NULL);
    char line[15];
    while (rows-- && nokia_next_row(&text, columns, line))
        nokia5110_text(canvas, 0, row++ * 8, line, true);
}

static void nokia_draw_volume(nokia5110_canvas_t *canvas, uint8_t volume, bool muted)
{
    const int x = 56, y = 40;
    nokia5110_rect(canvas, x, y + 2, 2, 3, true, true);
    nokia5110_rect(canvas, x + 2, y + 1, 1, 5, true, true);
    nokia5110_rect(canvas, x + 3, y, 2, 7, true, true);
    if (muted || volume == 0) {
        for (int i = 0; i < 5; i++) {
            nokia5110_pixel(canvas, x + 7 + i, y + 1 + i, true);
            nokia5110_pixel(canvas, x + 7 + i, y + 5 - i, true);
        }
    } else {
        for (int i = 0; i < 3 && volume * 3 > i * 100; i++) {
            nokia5110_rect(canvas, x + 7 + i * 2, y + 2 - i, 1, 3 + i * 2, true, true);
        }
    }
}

static void nokia_draw_battery(nokia5110_canvas_t *canvas, const external_frame_t *frame)
{
    const int x = 70, y = 40;
    nokia5110_rect(canvas, x, y, 12, 7, true, false);
    nokia5110_rect(canvas, x + 12, y + 2, 2, 3, true, true);
    if (!frame->battery_available) {
        nokia5110_rect(canvas, x + 4, y + 3, 4, 1, true, true);
        return;
    }

    int fill = (frame->battery_percent + 5) / 10;
    if (fill > 10) fill = 10;
    nokia5110_rect(canvas, x + 1, y + 1, fill, 5, true, true);
    if (frame->battery_charging) {
        /* Invert the bolt against the gauge so it is visible at any level. */
        const uint8_t bolt[] = {0x4, 0x6, 0xf, 0x6, 0x2};
        for (int row = 0; row < 5; row++) {
            for (int col = 0; col < 4; col++) {
                if (bolt[row] & (1u << col))
                    nokia5110_pixel(canvas, x + 4 + col, y + 1 + row, 3 + col >= fill);
            }
        }
    }
}

static bool nokia_present(const external_frame_t *frame, uint32_t now_ms, uint32_t *refresh_ms)
{
    nokia5110_canvas_t canvas;
    char title[2 * sizeof(frame->title)], station[2 * sizeof(frame->station)];
    nokia_ascii(title, sizeof(title), frame->title);
    nokia_ascii(station, sizeof(station), frame->station);
    nokia5110_clear(&canvas, false);
    nokia_draw_scrolled(&canvas, &station_scroll, frame->station, station, now_ms, refresh_ms, 0, 2, 11);
    if (frame->have_logo) {
        for (int y = 0; y < EXTERNAL_LOGO_SIZE; y++) {
            for (int x = 0; x < EXTERNAL_LOGO_SIZE; x++) {
                if (frame->logo[y * 2 + x / 8] & (0x80 >> (x % 8)))
                    nokia5110_pixel(&canvas, NOKIA5110_WIDTH - EXTERNAL_LOGO_SIZE + x, y, true);
            }
        }
    } else if (*station) {
        char initials[3] = {station[0], 0, 0};
        const char *space = strchr(station, ' ');
        if (space && space[1]) initials[1] = space[1];
        nokia5110_rect(&canvas, 68, 0, 16, 16, true, false);
        nokia5110_text(&canvas, 70, 4, initials, true);
    }
    nokia_draw_scrolled(&canvas, &title_scroll, frame->title, title, now_ms, refresh_ms, 2, 3, 14);
    char status[10] = {0}; /* Nine 6-pixel cells before the icons. */
    memcpy(status, frame->state, sizeof(status) - 1);
    nokia5110_text(&canvas, 0, 40, status, true);
    nokia_draw_volume(&canvas, frame->volume_percent, frame->muted);
    nokia_draw_battery(&canvas, frame);
    if (presented && !memcmp(&canvas, &last, sizeof(canvas))) return true;
    if (nokia5110_flush(&lcd.display, &canvas) != NOKIA5110_OK) return false;
    last = canvas;
    presented = true;
    return true;
}

static void nokia_close(void)
{
    (void)nokia5110_espidf_close(&lcd);
    presented = false;
}

const external_driver_t external_nokia5110 = {nokia_open, nokia_present, nokia_close};

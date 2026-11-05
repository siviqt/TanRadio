#!/usr/bin/env python3
"""Exercise the scroll cycle, Nokia viewport, glyphs and status icons."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "external_display/driver.h"
#include "nokia5110_espidf.h"
#include "util/text_scroll.h"

static nokia5110_canvas_t screen;
static unsigned frames;
int bsp_catt_get_gpio(int pin) { return pin + 1; }
nokia5110_result_t nokia5110_espidf_open(nokia5110_espidf_t *lcd, nokia5110_pins_t pins, unsigned contrast) {
    assert(lcd && pins.rst == 3 && pins.ce == 6 && pins.dc == 7 && pins.din == 4 && pins.clk == 8);
    assert(contrast == 60);
    return NOKIA5110_OK;
}
nokia5110_result_t nokia5110_espidf_close(nokia5110_espidf_t *lcd) {
    assert(lcd);
    return NOKIA5110_OK;
}
nokia5110_result_t nokia5110_flush(nokia5110_t *lcd, const nokia5110_canvas_t *canvas) {
    assert(lcd);
    screen = *canvas;
    frames++;
    return NOKIA5110_OK;
}

static void check_row(int row, const char *text, int columns) {
    nokia5110_canvas_t expected = {0};
    nokia5110_text(&expected, 0, row * 8, text, true);
    assert(!memcmp(screen.pixels + row * 84, expected.pixels + row * 84, columns * 6));
}

static bool pixel(int x, int y) {
    return (screen.pixels[x + (y / 8) * 84] >> (y % 8)) & 1;
}

static void check_glyphs(void) {
    nokia5110_canvas_t question = {0};
    nokia5110_text(&question, 0, 0, "?", true);
    for (char ch = '!'; ch <= '~'; ch++) {
        if (ch == '?') continue;
        nokia5110_canvas_t rendered = {0};
        const char text[] = {ch, 0};
        nokia5110_text(&rendered, 0, 0, text, true);
        assert(memcmp(rendered.pixels, question.pixels, 5));
        assert(memcmp(rendered.pixels, (uint8_t[5]){0}, 5));
    }
}

int main(void) {
    check_glyphs();
    assert(!text_scroll_at(100, 0, 250, 1500).until_next);
    assert(text_scroll_at(1499, 3, 250, 1500).offset == 0);
    assert(text_scroll_at(1500, 3, 250, 1500).offset == 1);
    assert(text_scroll_at(1750, 3, 250, 1500).offset == 2);
    assert(text_scroll_at(2000, 3, 250, 1500).offset == 3);
    assert(text_scroll_at(3500, 3, 250, 1500).offset == 0);

    external_frame_t frame = {
        .station = "XXXXXXXXXXX YYYYYYYYYYY ZZZZZZZZZZZ",
        .title = "AAAAAAAAAAAAAA BBBBBBBBBBBBBB CCCCCCCCCCCCCC DDDDDDDDDDDDDD",
        .state = "Playing",
        .have_logo = true,
        .logo = {0x80},
    };
    const external_driver_t *driver = &external_nokia5110;
    assert(driver->open(60));
    uint32_t refresh = 0;
    uint32_t start = UINT32_MAX - 1000; /* The first scroll spans clock wrap. */
    assert(driver->present(&frame, start, &refresh) && refresh == 2200 && frames == 1);
    check_row(0, "XXXXXXXXXXX", 11);
    check_row(2, "AAAAAAAAAAAAAA", 14);
    check_row(4, "CCCCCCCCCCCCCC", 14);
    assert(screen.pixels[68] == 1);
    refresh = 0;
    assert(driver->present(&frame, start + 2200, &refresh) && refresh == 2200 && frames == 2);
    check_row(0, "YYYYYYYYYYY", 11);
    check_row(2, "BBBBBBBBBBBBBB", 14);
    check_row(4, "DDDDDDDDDDDDDD", 14);
    check_row(5, "Playing", 9);
    assert(screen.pixels[68] == 1);
    refresh = 0;
    assert(driver->present(&frame, start + 4400, &refresh) && refresh == 2200 && frames == 3);
    check_row(0, "XXXXXXXXXXX", 11);
    check_row(2, "AAAAAAAAAAAAAA", 14);

    strcpy(frame.station, "Café FM");
    strcpy(frame.title, "DJ #1 — “Rock”");
    refresh = 0;
    assert(driver->present(&frame, start + 4500, &refresh) && !refresh && frames == 4);
    check_row(0, "CAFE FM", 11);
    check_row(2, "DJ #1 - \"Rock\"", 14);
    check_row(3, "", 14);
    check_row(5, "Playing", 9);
    assert(driver->present(&frame, start + 6000, &refresh) && !refresh && frames == 4);
    frame.battery_available = true;
    frame.battery_percent = 50;
    frame.volume_percent = 65;
    assert(driver->present(&frame, start + 6100, &refresh) && frames == 5);
    check_row(5, "Playing", 9);
    assert(pixel(70, 40) && pixel(83, 43)); /* Battery outline and terminal. */
    assert(pixel(75, 43) && !pixel(76, 43)); /* Half filled. */
    assert(pixel(56, 43) && pixel(63, 43) && pixel(65, 43) && !pixel(67, 43));
    assert(driver->present(&frame, start + 6200, &refresh) && frames == 5);

    frame.muted = frame.battery_charging = true;
    assert(driver->present(&frame, start + 6300, &refresh) && frames == 6);
    assert(pixel(63, 41) && pixel(65, 43) && pixel(67, 45)); /* Mute cross. */
    assert(!pixel(75, 42) && pixel(76, 42)); /* Charging bolt on both sides of the fill. */

    frame.muted = frame.battery_charging = false;
    frame.volume_percent = frame.battery_percent = 100;
    strcpy(frame.state, "Audio error");
    assert(driver->present(&frame, start + 6400, &refresh) && frames == 7);
    check_row(5, "Audio err", 9);
    assert(pixel(67, 40) && pixel(80, 45));
    assert(!pixel(54, 43) && !pixel(55, 43) && !pixel(68, 43) && !pixel(69, 43));

    frame.battery_available = false;
    frame.volume_percent = 0;
    assert(driver->present(&frame, start + 6500, &refresh) && frames == 8);
    assert(!pixel(71, 41) && pixel(74, 43) && pixel(77, 43) && !pixel(80, 45));
    assert(pixel(63, 41) && pixel(67, 45));
    driver->close();
    puts("PASS: Nokia scrolling, glyphs, battery level/charging, volume/mute and status layout");
}
'''

with tempfile.TemporaryDirectory(prefix="tanradio-now-playing-scroll-") as directory:
    work = Path(directory)
    (work / "bsp").mkdir()
    (work / "bsp/catt.h").write_text(
        "enum {CATT_PIN_D0, CATT_PIN_D1, CATT_PIN_D2, CATT_PIN_D3, CATT_PIN_D4, "
        "CATT_PIN_D5, CATT_PIN_D6, CATT_PIN_D7};\nint bsp_catt_get_gpio(int pin);\n"
    )
    (work / "check.c").write_text(HARNESS)
    nokia = ROOT / "local-components/nokia5110"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "main"),
                    "-I", str(work), "-I", str(nokia / "include"), str(work / "check.c"),
                    str(ROOT / "main/external_display/nokia.c"), str(nokia / "src/canvas.c"),
                    "-o", str(work / "check")], check=True)
    subprocess.run([str(work / "check")], check=True)

#!/usr/bin/env python3
"""Check shortcut glyph positions and directions with the production PAX renderer."""
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
PAX = ROOT / "managed_components/robotman2412__pax-gfx"
CODECS = ROOT / "managed_components/robotman2412__pax-codecs/include"

CMAKE = r'''
cmake_minimum_required(VERSION 3.21)
project(shortcut_test LANGUAGES C CXX)
set(CMAKE_C_STANDARD 23)
set(CMAKE_CXX_STANDARD 23)
set(PAX_STATIC ON)
add_subdirectory("${PAX_SOURCE}" pax)
add_executable(shortcut_test check.c "${APP_SOURCE}/ui/shortcuts.c" "${APP_SOURCE}/ui/theme.c")
target_include_directories(shortcut_test PRIVATE . "${APP_SOURCE}" "${CODECS_INCLUDE}")
target_compile_options(shortcut_test PRIVATE -ffunction-sections)
# The launcher artwork loader uses ESP-IDF. Test the built-in symbols without it.
target_link_options(shortcut_test PRIVATE -Wl,--gc-sections)
target_link_libraries(shortcut_test PRIVATE pax_gfx)
'''

FAT_HEADER = r'''
#include <stdbool.h>
typedef int wl_handle_t;
typedef int esp_err_t;
#define WL_INVALID_HANDLE (-1)
#define ESP_OK 0
#define CONFIG_WL_SECTOR_SIZE 4096
typedef struct {
    bool format_if_mount_failed;
    int max_files, allocation_unit_size;
} esp_vfs_fat_mount_config_t;
esp_err_t esp_vfs_fat_spiflash_mount_rw_wl(const char *, const char *,
    const esp_vfs_fat_mount_config_t *, wl_handle_t *);
esp_err_t esp_vfs_fat_spiflash_unmount_rw_wl(const char *, wl_handle_t);
'''

HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "ui/shortcuts.h"
#include "ui/theme.h"

static bool white(const pax_buf_t *buffer, int x, int y) {
    return (pax_get_pixel(buffer, x, y) & 0x00e0e0e0) == 0x00e0e0e0;
}

static void check_keycap(const pax_buf_t *buffer, int left, int width, int center_y) {
    unsigned ink = 0;
    for (int y = 0; y < 480; y++) {
        for (int x = 0; x < 800; x++) {
            if (!white(buffer, x, y)) continue;
            assert(x >= left && x < left + width && abs(y - center_y) < 12);
            ink++;
        }
    }
    assert(ink >= 20);
}

static void check_arrow(pax_buf_t *buffer, ui_shortcut_key_t key,
                        int left, int center_y, int dx, int dy) {
    pax_background(buffer, UI_COLOR_BACKGROUND);
    ui_draw_shortcut(buffer, UI_KEYS(key), "", left, center_y, UI_COLOR_TEXT, UI_TEXT_SMALL);
    check_keycap(buffer, left, 24, center_y);

    unsigned head = 0, tail = 0, shaft = 0;
    for (int y = -8; y <= 8; y++) {
        for (int x = -8; x <= 8; x++) {
            if (!white(buffer, left + 12 + x, center_y + y)) continue;
            int along = x * dx + y * dy;
            int across = abs(y * dx - x * dy);
            if (across >= 2 && along >= 0) head++;
            if (across >= 2 && along < 0) tail++;
            if (across <= 1 && along <= -3) shaft++;
        }
    }
    assert(head >= 6 && head > tail + 4 && shaft >= 3);
}

static void check_backspace(pax_buf_t *buffer) {
    const int left = 514, y = 395;
    pax_background(buffer, UI_COLOR_BACKGROUND);
    ui_draw_shortcut(buffer, UI_KEYS(UI_KEY_BACKSPACE), "", left, y, UI_COLOR_TEXT, UI_TEXT_SMALL);
    check_keycap(buffer, left, 32, y);
    assert(white(buffer, left + 7, y));            /* Left-facing tip. */
    assert(!white(buffer, left + 7, y - 5));
    assert(white(buffer, left + 18, y));           /* Cross inside the outline. */
    assert(white(buffer, left + 25, y));           /* Straight right edge. */
}

static void check_function_symbol(pax_buf_t *buffer, ui_shortcut_key_t key) {
    const int left = 520, center_y = 449;
    pax_background(buffer, UI_COLOR_BACKGROUND);
    const pax_col_t background = pax_get_pixel(buffer, 0, 0);
    ui_draw_shortcut(buffer, UI_KEYS(key), "", left, center_y, UI_COLOR_TEXT, UI_TEXT_SMALL);
    bool top = false, bottom = false;
    for (int y = 0; y < 480; y++) {
        for (int x = 0; x < 800; x++) {
            if (pax_get_pixel(buffer, x, y) == background) continue;
            assert(x >= left && x < left + 32 && abs(y - center_y) < 16);
            top |= y <= center_y - 6;
            bottom |= y >= center_y + 5;
        }
    }
    assert(top && bottom);
}

static void check_orientation(bool rotated) {
    pax_buf_t buffer;
    assert(pax_buf_init(&buffer, NULL, rotated ? 480 : 800, rotated ? 800 : 480, PAX_BUF_16_565RGB));
    if (rotated) pax_buf_set_orientation(&buffer, PAX_O_ROT_CW);
    assert(pax_buf_get_width(&buffer) == 800 && pax_buf_get_height(&buffer) == 480);
    check_arrow(&buffer, UI_KEY_UP, 136, 366, 0, -1);
    check_arrow(&buffer, UI_KEY_DOWN, 164, 366, 0, 1);
    check_arrow(&buffer, UI_KEY_LEFT, 270, 366, -1, 0);
    check_arrow(&buffer, UI_KEY_RIGHT, 298, 366, 1, 0);
    check_arrow(&buffer, UI_KEY_UP, 658, 94, 0, -1);
    check_arrow(&buffer, UI_KEY_DOWN, 686, 94, 0, 1);
    check_backspace(&buffer);
    for (ui_shortcut_key_t key = UI_KEY_F1; key <= UI_KEY_F6; key++) {
        check_function_symbol(&buffer, key);
    }
    pax_buf_destroy(&buffer);
}

int main(void) {
    check_orientation(false);
    check_orientation(true);
    puts("PASS: arrows point correctly and stay inside their keycaps; Backspace and F-key symbols render in native Tanmatsu rotation");
}
'''


def cmake(*args):
    result = subprocess.run(["cmake", *args], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if result.returncode:
        raise SystemExit(result.stdout)


def main():
    if not (PAX / "CMakeLists.txt").is_file() or not (CODECS / "pax_codecs.h").is_file():
        raise SystemExit("Run make build first to fetch the declared PAX dependencies.")
    with tempfile.TemporaryDirectory(prefix="tanradio-shortcuts-") as directory:
        work = Path(directory)
        (work / "CMakeLists.txt").write_text(CMAKE)
        (work / "check.c").write_text(HARNESS)
        (work / "esp_heap_caps.h").write_text("#define heap_caps_calloc(n, size, caps) calloc(n, size)\n")
        (work / "esp_log.h").write_text("#define ESP_LOGW(tag, ...) ((void)(tag))\n")
        (work / "esp_vfs_fat.h").write_text(FAT_HEADER)
        build = work / "build"
        cmake("-S", str(work), "-B", str(build), f"-DPAX_SOURCE={PAX}",
              f"-DAPP_SOURCE={ROOT / 'main'}", f"-DCODECS_INCLUDE={CODECS}")
        cmake("--build", str(build), "--target", "shortcut_test", "--parallel", "4")
        subprocess.run([str(build / "shortcut_test")], check=True)


if __name__ == "__main__":
    main()

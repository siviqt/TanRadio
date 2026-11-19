#include "ui/shortcuts.h"
#include "ui/theme.h"

#include <stdlib.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "pax_codecs.h"
#include "pax_text.h"

#define ICON_SIZE 32
#define KEYCAP_HEIGHT 24
#define KEYCAP_TEXT_SIZE 15

static const char *TAG = "tanradio.shortcuts";

static const struct {
    const char *path;
    pax_col_t color;
} artwork[] = {
    [UI_KEY_F1] = {"/int/icons/f1.png", 0xFFFF6666},
    [UI_KEY_F2] = {"/int/icons/f2.png", 0xFFFFA054},
    [UI_KEY_F3] = {"/int/icons/f3.png", 0xFFCED344},
    [UI_KEY_F4] = {"/int/icons/f4.png", 0xFF91D653},
    [UI_KEY_F5] = {"/int/icons/f5.png", 0xFF50D7D7},
    [UI_KEY_F6] = {"/int/icons/f6.png", 0xFFDB72E7},
    [UI_KEY_VOLUME_UP] = {"/int/icons/volume_up.png", 0xFFF1F5F9},
    [UI_KEY_VOLUME_DOWN] = {"/int/icons/volume_down.png", 0xFFF1F5F9},
};

#define ARTWORK_COUNT (sizeof(artwork) / sizeof(artwork[0]))
static pax_buf_t icons[ARTWORK_COUNT];

static bool load_icon(size_t index)
{
    FILE *file = fopen(artwork[index].path, "rb");
    if (!file) return false;

    pax_png_info_t info;
    pax_col_t *pixels = NULL;
    if (pax_info_png_fd(&info, file) && info.width == ICON_SIZE && info.height == ICON_SIZE &&
        fseek(file, 0, SEEK_SET) == 0) {
        pixels = heap_caps_calloc(ICON_SIZE * ICON_SIZE, sizeof(*pixels), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    bool loaded = false;
    if (pixels) {
        pax_buf_init(&icons[index], pixels, ICON_SIZE, ICON_SIZE, PAX_BUF_32_8888ARGB);
        loaded = pax_insert_png_fd(&icons[index], file, 0, 0, 0);
        if (loaded) {
            /* Keep the launcher silhouettes and make them legible on the dark themes. */
            for (size_t i = 0; i < ICON_SIZE * ICON_SIZE; i++) {
                pixels[i] = (pixels[i] & 0xFF000000) | (artwork[index].color & 0x00FFFFFF);
            }
        } else {
            pax_buf_destroy(&icons[index]);
            free(pixels);
            icons[index] = (pax_buf_t){0};
        }
    }
    fclose(file);
    return loaded;
}

void ui_shortcuts_load(void)
{
    const esp_vfs_fat_mount_config_t config = {
        .format_if_mount_failed = false,
        .max_files = 1,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
    };
    wl_handle_t handle = WL_INVALID_HANDLE;
    esp_err_t result = esp_vfs_fat_spiflash_mount_rw_wl("/int", "locfd", &config, &handle);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Using built-in key symbols: %s", esp_err_to_name(result));
        return;
    }

    for (size_t i = 0; i < ARTWORK_COUNT; i++) {
        if (!load_icon(i)) ESP_LOGW(TAG, "Key artwork unavailable: %s", artwork[i].path);
    }
    result = esp_vfs_fat_spiflash_unmount_rw_wl("/int", handle);
    if (result != ESP_OK) ESP_LOGW(TAG, "Unable to release key artwork filesystem: %s", esp_err_to_name(result));
}

/* PAX's thick-line primitive bypasses framebuffer orientation. The triangle
 * primitive rotates both halves of the stroke with the rest of the keycap. */
static void draw_stroke(pax_buf_t *buffer, pax_col_t color,
                        float x0, float y0, float x1, float y1, float width)
{
    const pax_vec2f normal = pax_vec2f_unify((pax_vec2f){y0 - y1, x1 - x0});
    const float dx = normal.x * width / 2;
    const float dy = normal.y * width / 2;
    pax_draw_tri(buffer, color, x0 + dx, y0 + dy, x1 + dx, y1 + dy, x1 - dx, y1 - dy);
    pax_draw_tri(buffer, color, x0 + dx, y0 + dy, x1 - dx, y1 - dy, x0 - dx, y0 - dy);
}

static void draw_function_symbol(pax_buf_t *buffer, ui_shortcut_key_t key, float x, float y)
{
    const pax_col_t color = artwork[key].color;
    const float stroke = 2.8f;
    switch (key) {
        case UI_KEY_F1: /* Cross */
            draw_stroke(buffer, color, x - 7, y - 7, x + 7, y + 7, stroke);
            draw_stroke(buffer, color, x + 7, y - 7, x - 7, y + 7, stroke);
            break;
        case UI_KEY_F2: /* Triangle */
            draw_stroke(buffer, color, x, y - 8, x + 9, y + 7, stroke);
            draw_stroke(buffer, color, x - 9, y + 7, x, y - 8, stroke);
            pax_draw_rect(buffer, color, x - 10, y + 6, 20, stroke);
            break;
        case UI_KEY_F3: /* Square */
            pax_draw_rect(buffer, color, x - 9, y - 9, 18, stroke);
            pax_draw_rect(buffer, color, x - 9, y + 9 - stroke, 18, stroke);
            pax_draw_rect(buffer, color, x - 9, y - 9, stroke, 18);
            pax_draw_rect(buffer, color, x + 9 - stroke, y - 9, stroke, 18);
            break;
        case UI_KEY_F4: /* Circle */
            pax_draw_hollow_circle(buffer, color, x, y, 6, 8.8f);
            break;
        case UI_KEY_F5: /* Three rounded lobes */
            pax_draw_round_hollow_arc(buffer, color, x, y - 4, 3.1f, 5.6f, -0.46f, 3.59f);
            pax_draw_round_hollow_arc(buffer, color, x - 5, y + 4, 3.1f, 5.6f, 1.56f, 5.85f);
            pax_draw_round_hollow_arc(buffer, color, x + 5, y + 4, 3.1f, 5.6f, 3.58f, 7.87f);
            break;
        case UI_KEY_F6: /* Diamond */
            draw_stroke(buffer, color, x, y - 9, x + 9, y, stroke);
            draw_stroke(buffer, color, x + 9, y, x, y + 9, stroke);
            draw_stroke(buffer, color, x, y + 9, x - 9, y, stroke);
            draw_stroke(buffer, color, x - 9, y, x, y - 9, stroke);
            break;
        default:
            break;
    }
}

static void draw_arrow(pax_buf_t *buffer, ui_shortcut_key_t key, float x, float y)
{
    static const pax_vec2f direction[] = {
        [UI_KEY_UP - UI_KEY_UP] = {0, -1},
        [UI_KEY_DOWN - UI_KEY_UP] = {0, 1},
        [UI_KEY_LEFT - UI_KEY_UP] = {-1, 0},
        [UI_KEY_RIGHT - UI_KEY_UP] = {1, 0},
    };
    const pax_vec2f d = direction[key - UI_KEY_UP];
    const float tip_x = x + 5 * d.x, tip_y = y + 5 * d.y;
    const pax_col_t color = UI_COLOR_TEXT;
    draw_stroke(buffer, color, x - 5 * d.x, y - 5 * d.y, tip_x, tip_y, 2);
    draw_stroke(buffer, color, x - 4 * d.y, y + 4 * d.x, tip_x, tip_y, 2);
    draw_stroke(buffer, color, x + 4 * d.y, y - 4 * d.x, tip_x, tip_y, 2);
}

static void draw_backspace(pax_buf_t *buffer, float x, float y)
{
    const pax_col_t color = UI_COLOR_TEXT;
    const float edge = 1.8f;
    draw_stroke(buffer, color, x - 10, y, x - 4, y - 6, edge);
    draw_stroke(buffer, color, x - 4, y - 6, x + 9, y - 6, edge);
    draw_stroke(buffer, color, x + 9, y - 6, x + 9, y + 6, edge);
    draw_stroke(buffer, color, x + 9, y + 6, x - 4, y + 6, edge);
    draw_stroke(buffer, color, x - 4, y + 6, x - 10, y, edge);
    draw_stroke(buffer, color, x, y - 2.5f, x + 5, y + 2.5f, edge);
    draw_stroke(buffer, color, x + 5, y - 2.5f, x, y + 2.5f, edge);
}

static float draw_key(pax_buf_t *buffer, ui_shortcut_key_t key, float x, float y)
{
    if (key <= UI_KEY_F6) {
        if (icons[key].buf) pax_draw_image(buffer, &icons[key], x, y - ICON_SIZE / 2);
        else draw_function_symbol(buffer, key, x + ICON_SIZE / 2, y);
        return ICON_SIZE;
    }

    if (key >= UI_KEY_UP && key <= UI_KEY_RIGHT) {
        pax_draw_round_rect(buffer, UI_COLOR_FIELD, x, y - KEYCAP_HEIGHT / 2, 24, KEYCAP_HEIGHT, 5);
        draw_arrow(buffer, key, x + 12, y);
        return 24;
    }

    if (key == UI_KEY_BACKSPACE) {
        pax_draw_round_rect(buffer, UI_COLOR_FIELD, x, y - KEYCAP_HEIGHT / 2, 32, KEYCAP_HEIGHT, 5);
        draw_backspace(buffer, x + 16, y);
        return 32;
    }

    if ((size_t)key < ARTWORK_COUNT && icons[key].buf) {
        pax_draw_round_rect(buffer, UI_COLOR_FIELD, x, y - KEYCAP_HEIGHT / 2, 32, KEYCAP_HEIGHT, 5);
        pax_draw_image_sized(buffer, &icons[key], x + 3, y - 13, 26, 26);
        return 32;
    }

    static const char *const labels[] = {
        [UI_KEY_TAB] = "Tab", [UI_KEY_ENTER] = "Enter",
        [UI_KEY_SPACE] = "Space", [UI_KEY_HYPER] = "Hyper",
        [UI_KEY_VOLUME_UP] = "Vol+", [UI_KEY_VOLUME_DOWN] = "Vol−",
    };
    const char *label = labels[key];
    const float width = pax_text_size(UI_FONT_BODY, KEYCAP_TEXT_SIZE, label).x + 14;
    pax_draw_round_rect(buffer, UI_COLOR_FIELD, x, y - KEYCAP_HEIGHT / 2, width, KEYCAP_HEIGHT, 5);
    pax_draw_text(buffer, UI_COLOR_TEXT, UI_FONT_BODY, KEYCAP_TEXT_SIZE, x + 7, y - 8, label);
    return width;
}

float ui_draw_shortcut(pax_buf_t *buffer, const ui_shortcut_key_t *keys, const char *action,
                       float x, float center_y, pax_col_t color, float text_size)
{
    for (const ui_shortcut_key_t *key = keys; *key != UI_KEY_NONE; key++) {
        if (key != keys) x += 4;
        x += draw_key(buffer, *key, x, center_y);
    }
    x += 6;
    return x + pax_draw_text(buffer, color, UI_FONT_BODY, text_size, x, center_y - text_size / 2, action).x;
}

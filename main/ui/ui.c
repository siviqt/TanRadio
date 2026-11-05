/*
 * Project : TanRadio
 * Module  : ui
 * File    : ui.c
 *
 * Purpose :
 *   TanRadio user interface.
 */

/* BSP display */
#include "bsp/display.h"

/* BSP input */
#include "bsp/input.h"

#include "bsp/led.h"

/* BSP power */
#include "bsp/power.h"

/* PAX Graphics */
#include "pax_fonts.h"
#include "pax_codecs.h"
#include "pax_gfx.h"
#include "pax_text.h"
#include "pax_types.h"

/* ESP-IDF */
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"

/* FreeRTOS */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

/* Standard C library */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

/* Local headers */
#include "ui.h"
#include "welcome.h"
#include "audio/audio.h"
#include "network/network.h"
#include "radio/radio.h"
#include "radio/browser.h"
#include "ui/controls.h"
#include "ui/theme.h"
#include "ui/exit.h"
#include "ui/settings.h"
#include "radio/artwork.h"
#include "ui/about.h"
#include "ui/about_view.h"
#include "input/keys.h"
#include "util/text_scroll.h"
#include "bsp/device.h"

static const char *TAG = "tanradio.ui";

static size_t display_h_res = 0;
static size_t display_v_res = 0;
static bsp_display_color_format_t display_color_format = 0;
static bsp_display_endianness_t display_data_endian = 0;

static pax_buf_t framebuffer = {0};

#define UI_FRAME_INTERVAL_MS 100
#define UI_NOW_PLAYING_INTERVAL_MS 250
#define UI_SCROLL_STEP_PX 10
#define UI_SCROLL_PAUSE_MS 1500

#define UI_HEADER_HEIGHT 64
#define UI_CONTENT_TOP 78
#define UI_CONTENT_BOTTOM 404
#define UI_NOW_PLAYING_WIDTH 484
#define UI_STATION_LIST_X 514
#define UI_STATION_LIST_WIDTH 270
#define UI_STATION_FIRST_Y 122
#define UI_STATION_ROW_HEIGHT 55
#define UI_STATION_VISIBLE_COUNT 5
#define UI_STATION_SELECTED_ROW 2

#define UI_LEGEND_HEIGHT 62
#define UI_LEGEND_FUNCTION_WIDTH 124

#define UI_STATUS_Y 21
#define UI_STATUS_FONT_SIZE UI_TEXT_SMALL
#define UI_STATUS_SSID_LENGTH 20

#define UI_KEYBOARD_BACKLIGHT_RESTORE_PERCENT 50
#define UI_FUNCTION_ICON_COUNT 6
#define UI_FUNCTION_ICON_SIZE 32
#define UI_FUNCTION_ICON_BYTES \
    (UI_FUNCTION_ICON_SIZE * UI_FUNCTION_ICON_SIZE * sizeof(pax_col_t))

static ui_station_list_t ui_station_list;
/* Search UI state is protected by ui_mutex; directory data has its own lock. */
static bool ui_search_open;
static bool ui_about_open;
static ui_exit_state_t ui_exit_state;
static ui_preferences_t ui_preferences;
static ui_settings_t ui_settings;
static unsigned ui_settings_revision;
/* Presentation and completion are owned by the UI task (or welcome startup). */
static bool ui_exit_presented;
static bool ui_exit_started;
static bool ui_services_started;
/* Release/acquire publication keeps catalog/UI reads out of initialization. */
static atomic_int ui_services_state = UI_SERVICES_STARTING;
#define UI_EXIT_SPLASH_MS 1000
static char ui_search_query[BROWSER_QUERY_LENGTH + 1];
static radio_browser_search_mode_t ui_search_mode = BROWSER_SEARCH_NAME;
static size_t ui_search_selection;
static char ui_search_notice[96];
static unsigned ui_search_revision;
static struct {
    bool open;
    bool failed;
    size_t index;
    radio_station_t station;
} ui_remove_state;

static ui_playback_state_t ui_playback_state =
    UI_PLAYBACK_STOPPED;

static SemaphoreHandle_t ui_mutex = NULL;
static bool ui_display_enabled = true;
static uint8_t ui_saved_backlight_brightness = 100;

static bool ui_status_cache_valid = false;
static uint8_t ui_cached_volume = 0;
static audio_output_t ui_cached_output = AUDIO_OUTPUT_SPEAKER;
static unsigned int ui_cached_battery_percent = 0;
static bool ui_cached_battery_available = false;
static bool ui_cached_battery_charging = false;
static bool ui_cached_wifi_connected = false;
static char ui_cached_ssid[UI_STATUS_SSID_LENGTH + 1] = {0};
static ui_playback_state_t ui_cached_playback_state =
    UI_PLAYBACK_STOPPED;

/* Sampling runs only on the status task (or startup before it exists).
 * Cached display values and wake requests are protected by ui_mutex. */
static TaskHandle_t ui_status_task_handle = NULL;
/* Only the rendering task (or startup before it exists) touches this scratch
 * space, under ui_mutex. Large station snapshots must not stack across nested
 * drawing calls on the input task. */
static struct {
    radio_now_playing_t now_playing;
    radio_station_t selected;
    radio_station_t row;
} ui_render_state;
/* The status/render task owns these copies, including the thumbnail's pixels. */
static radio_now_playing_t ui_now_playing;
static station_logo_t ui_station_logo;
static unsigned ui_artwork_request;
static bool ui_artwork_online, ui_artwork_ready;
static const char *ui_external_state;
typedef struct {
    char text[RADIO_TITLE_SIZE];
    TickType_t started;
    unsigned distance, offset;
} ui_scroll_label_t;
enum { UI_SCROLL_NAME, UI_SCROLL_DETAILS, UI_SCROLL_TRACK, UI_SCROLL_COUNT };
static ui_scroll_label_t ui_now_playing_scroll[UI_SCROLL_COUNT];
static uint32_t ui_scroll_connection, ui_scroll_metadata_revision;

static bool ui_battery_refresh_requested = false;
#define UI_BATTERY_SAMPLE_INTERVAL pdMS_TO_TICKS(60000)
#define UI_BATTERY_STALE_INTERVAL pdMS_TO_TICKS(180000)
#define UI_BATTERY_HYSTERESIS_PERCENT 2
#define UI_BATTERY_CONFIRM_SAMPLES 2

static bool ui_battery_sampled = false;
static TickType_t ui_battery_last_sample = 0;
static bool ui_battery_filter_valid = false;
static float ui_battery_filtered_mv = 0;
static int ui_battery_pending_direction = 0;
static unsigned int ui_battery_pending_samples = 0;

static unsigned int ui_battery_voltage_percent(float voltage)
{
    /* Preserve the existing linear estimate: 3.05 V = 0%, 4.15 V = 100%.
     * Smoothing improves display stability, not capacity-estimate accuracy. */
    if (voltage <= 3050.0f) {
        return 0;
    }
    if (voltage >= 4150.0f) {
        return 100;
    }
    return (unsigned int)((voltage - 3050.0f) * 100.0f / 1100.0f);
}

static bool ui_update_battery(void)
{
    xSemaphoreTake(ui_mutex, portMAX_DELAY);
    if (!ui_display_enabled && external_display_status() != EXTERNAL_INITIALIZED) {
        xSemaphoreGive(ui_mutex);
        return false;
    }
    bool force = ui_battery_refresh_requested;
    ui_battery_refresh_requested = false;

    TickType_t now = xTaskGetTickCount();
    TickType_t elapsed = now - ui_battery_last_sample;
    if (!force && ui_battery_sampled && elapsed < UI_BATTERY_SAMPLE_INTERVAL) {
        xSemaphoreGive(ui_mutex);
        return false;
    }

    bool stale = ui_battery_sampled && elapsed >= UI_BATTERY_STALE_INTERVAL;
    ui_battery_sampled = true;
    ui_battery_last_sample = now;
    xSemaphoreGive(ui_mutex);

    /* Never hold the drawing/input mutex while waiting for the coprocessor. */
    bsp_power_battery_information_t battery = {0};
    esp_err_t result = bsp_power_get_battery_information(&battery);

    xSemaphoreTake(ui_mutex, portMAX_DELAY);
    if (!ui_display_enabled && external_display_status() != EXTERNAL_INITIALIZED) {
        xSemaphoreGive(ui_mutex);
        return false;
    }
    bool was_available = ui_cached_battery_available;
    bool was_charging = ui_cached_battery_charging;
    unsigned int previous_percent = ui_cached_battery_percent;
    if (result != ESP_OK || !battery.battery_available) {
        ui_cached_battery_available = false;
        ui_cached_battery_charging = false;
        ui_battery_filter_valid = false;
        ui_battery_pending_direction = 0;
        ui_battery_pending_samples = 0;
    } else {
        ui_cached_battery_available = true;
        ui_cached_battery_charging = battery.battery_charging;
        if (!ui_battery_filter_valid || stale ||
            was_charging != battery.battery_charging) {
            /* Seed immediately at startup, reconnect, charge-state changes,
             * or after a long sleep instead of slowly following stale data. */
            ui_battery_filtered_mv = battery.voltage;
            ui_cached_battery_percent = ui_battery_voltage_percent(ui_battery_filtered_mv);
            ui_battery_filter_valid = true;
            ui_battery_pending_direction = 0;
            ui_battery_pending_samples = 0;
        } else {
            /* One-minute samples: give the new reading half the weight. */
            ui_battery_filtered_mv += 0.5f * (battery.voltage - ui_battery_filtered_mv);
            unsigned int candidate = ui_battery_voltage_percent(ui_battery_filtered_mv);
            int delta = (int)candidate - (int)ui_cached_battery_percent;
            /* Permit the endpoints to settle even when only 1% away. */
            int threshold = (candidate == 0 || candidate == 100)
                                ? 1 : UI_BATTERY_HYSTERESIS_PERCENT;
            int direction = delta >= threshold ? 1 : delta <= -threshold ? -1 : 0;
            if (direction == 0) {
                ui_battery_pending_direction = 0;
                ui_battery_pending_samples = 0;
            } else {
                if (direction != ui_battery_pending_direction) {
                    ui_battery_pending_direction = direction;
                    ui_battery_pending_samples = 0;
                }
                /* A quick display toggle must not count as a minute of
                 * confirmation for a pending percentage change. */
                if (elapsed >= UI_BATTERY_SAMPLE_INTERVAL) {
                    ui_battery_pending_samples++;
                }
                if (ui_battery_pending_samples >= UI_BATTERY_CONFIRM_SAMPLES) {
                    ui_cached_battery_percent = candidate;
                    ui_battery_pending_direction = 0;
                    ui_battery_pending_samples = 0;
                }
            }
        }
    }

    bool changed = was_available != ui_cached_battery_available ||
                   was_charging != ui_cached_battery_charging ||
                   previous_percent != ui_cached_battery_percent;
    xSemaphoreGive(ui_mutex);
    return changed;
}

static pax_buf_t ui_function_icons[UI_FUNCTION_ICON_COUNT] = {0};
static bool ui_function_icon_loaded[UI_FUNCTION_ICON_COUNT] = {0};

static void ui_load_function_icons(void)
{
    static const char *paths[UI_FUNCTION_ICON_COUNT] = {
        "/int/icons/f1.png",
        "/int/icons/f2.png",
        "/int/icons/f3.png",
        "/int/icons/f4.png",
        "/int/icons/f5.png",
        "/int/icons/f6.png",
    };

    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = UI_FUNCTION_ICON_COUNT,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };
    wl_handle_t wl_handle = WL_INVALID_HANDLE;
    esp_err_t mount_result = esp_vfs_fat_spiflash_mount_rw_wl(
        "/int",
        "locfd",
        &mount_config,
        &wl_handle);

    if (mount_result != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Shared icon filesystem unavailable: %s",
            esp_err_to_name(mount_result));
        return;
    }

    for (size_t index = 0; index < UI_FUNCTION_ICON_COUNT; index++) {
        FILE *file = fopen(paths[index], "rb");
        if (file == NULL) {
            ESP_LOGW(TAG, "Shared function icon unavailable: %s", paths[index]);
            continue;
        }

        void *pixels = heap_caps_calloc(
            1,
            UI_FUNCTION_ICON_BYTES,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

        if (pixels == NULL) {
            fclose(file);
            ESP_LOGW(TAG, "Unable to allocate function icon %zu", index + 1);
            continue;
        }

        pax_buf_init(
            &ui_function_icons[index],
            pixels,
            UI_FUNCTION_ICON_SIZE,
            UI_FUNCTION_ICON_SIZE,
            PAX_BUF_32_8888ARGB);

        if (!pax_insert_png_fd(&ui_function_icons[index], file, 0, 0, 0)) {
            pax_buf_destroy(&ui_function_icons[index]);
            free(pixels);
            memset(&ui_function_icons[index], 0, sizeof(pax_buf_t));
            ESP_LOGW(TAG, "Unable to decode shared function icon: %s", paths[index]);
        } else {
            ui_function_icon_loaded[index] = true;
        }

        fclose(file);
    }

    esp_err_t unmount_result =
        esp_vfs_fat_spiflash_unmount_rw_wl("/int", wl_handle);
    if (unmount_result != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Unable to release shared icon filesystem: %s",
            esp_err_to_name(unmount_result));
    }
}

static bool ui_draw_function_icon(size_t index, float center_x, float center_y)
{
    if (index >= UI_FUNCTION_ICON_COUNT ||
        !ui_function_icon_loaded[index]) {
        return false;
    }

    pax_draw_image(
        &framebuffer,
        &ui_function_icons[index],
        center_x - (UI_FUNCTION_ICON_SIZE / 2.0f),
        center_y - (UI_FUNCTION_ICON_SIZE / 2.0f));

    return true;
}

static void ui_ellipsize_text(
    const pax_font_t *font,
    const char *source,
    char *output,
    size_t output_size,
    float font_size,
    float maximum_width)
{
    if (output_size == 0) {
        return;
    }

    if (source == NULL) {
        output[0] = '\0';
        return;
    }

    const size_t source_length = strlen(source);
    size_t length = source_length;
    bool truncated = false;

    if (length >= output_size) {
        length = output_size - 1;
        truncated = true;

        /* Do not split a multi-byte UTF-8 character at the buffer edge. */
        while (length > 0 &&
               (((unsigned char)source[length] & 0xC0U) == 0x80U)) {
            length--;
        }
    }

    memcpy(output, source, length);
    output[length] = '\0';

    if (!truncated &&
        pax_text_size(font, font_size, output).x <=
            maximum_width) {
        return;
    }

    do {
        if (length == 0) {
            snprintf(output, output_size, "...");
            return;
        }

        length = pax_utf8_seekprev_l(output, length, length);
        output[length] = '\0';

        if (length + 4 <= output_size) {
            memcpy(output + length, "...", 4);
        }
    } while (pax_text_size(
                 font,
                 font_size,
                 output).x > maximum_width);
}

/* Return the remaining text after fitting a word-wrapped first line. */
static const char *ui_first_line(const pax_font_t *font, const char *source,
                                 char *line, size_t capacity, float size, float width)
{
    size_t source_length = strlen(source);
    size_t cursor = 0;
    size_t word_break = 0;
    while (cursor < source_length) {
        size_t next = pax_utf8_seeknext_l(source, source_length, cursor);
        if (next <= cursor || next >= capacity) break;

        memcpy(line + cursor, source + cursor, next - cursor);
        line[next] = '\0';
        if (pax_text_size(font, size, line).x > width) break;
        if (source[cursor] == ' ' || source[cursor] == '-')
            word_break = source[cursor] == ' ' ? cursor : next;
        cursor = next;
    }
    if (cursor == 0 && source_length) {
        ui_ellipsize_text(font, source, line, capacity, size, width);
        return "";
    }

    size_t length = cursor < source_length && word_break ? word_break : cursor;
    line[length] = '\0';
    const char *remainder = source + length;
    while (*remainder == ' ') remainder++;
    return remainder;
}

static esp_err_t ui_restore_keyboard_backlight(void)
{
    return bsp_input_set_backlight_brightness(
        UI_KEYBOARD_BACKLIGHT_RESTORE_PERCENT);
}

/* These helpers run under ui_mutex, on both drawing and input paths. */
static void ui_refresh_preview(void)
{
    radio_playback_status_t playback;
    radio_get_playback_status(&playback);
    ui_station_list_refresh(&ui_station_list, &playback.station);
}

static bool ui_main_selected_station(radio_station_t *station)
{
    return ui_station_list_selected(&ui_station_list, station);
}

static void ui_focus_current_station(void)
{
    radio_playback_status_t playback;
    radio_get_playback_status(&playback);
    ui_station_list_focus(&ui_station_list, &playback.station);
}

static void ui_draw_button_legend(void)
{
    const float ui_width =
        (float)pax_buf_get_width(&framebuffer);
    const float ui_height =
        (float)pax_buf_get_height(&framebuffer);
    const float separator_y = ui_height - UI_LEGEND_HEIGHT;
    radio_station_t *selected = &ui_render_state.selected;
    const radio_playback_status_t *playback = &ui_render_state.now_playing.playback;
    bool have_selected = ui_search_open ? radio_browser_result(ui_search_selection, selected) :
                                         ui_main_selected_station(selected);
    ui_station_action_t action = ui_station_action(have_selected ? selected : NULL, playback);
    bool saved = ui_search_open && have_selected && stations_find(selected, NULL);
    const char *labels[UI_FUNCTION_ICON_COUNT] = {
        "Options/Exit", ui_search_open ? "Back" : "Find", "Stop", ui_station_action_label(action),
        ui_search_open ? (saved ? "Saved" : "Save") : "Next", "Lights"
    };
    if (ui_remove_state.open) {
        labels[1] = "Cancel";
        labels[3] = "Remove";
        labels[4] = "";
    }

    pax_draw_line(
        &framebuffer,
        UI_COLOR_BORDER,
        8,
        separator_y,
        ui_width - 8,
        separator_y);

    for (size_t index = 0; index < UI_FUNCTION_ICON_COUNT; index++) {
        const float center_x =
            (index * UI_LEGEND_FUNCTION_WIDTH) + 24 + (index ? 16 : 0);
        const float center_y = separator_y + 31;

        if (!ui_draw_function_icon(index, center_x, center_y)) {
            char key_label[4];
            snprintf(key_label, sizeof(key_label), "F%u", (unsigned int)index + 1);
            pax_draw_round_rect(&framebuffer, UI_COLOR_FIELD,
                                center_x - 16, center_y - 13, 32, 26, 5);
            pax_draw_text(&framebuffer, UI_COLOR_TEXT, UI_FONT_BODY, UI_TEXT_SMALL,
                          center_x - 10, center_y - 9, key_label);
        }

        pax_draw_text(
            &framebuffer,
            (index == 3 && !ui_remove_state.open && action == UI_STATION_DISABLED) ||
            (index == 2 && playback->stopped) ||
            (index == 4 && (ui_search_open ? (!have_selected || saved) : !stations_count()))
                ? UI_COLOR_DISABLED : UI_COLOR_TEXT,
            UI_FONT_BODY,
            UI_TEXT_SMALL,
            center_x + 18,
            center_y - 10,
            labels[index]);
    }
}

static bool ui_blit(void)
{
    if (!ui_display_enabled) {
        return false;
    }

    /* The BSP copies into an inactive native page and waits for scanout
     * retirement before returning. PAX may repaint only after this call. */
    const void *display_pixels = pax_buf_get_pixels(&framebuffer);

    esp_err_t res = bsp_display_blit(
        0,
        0,
        display_h_res,
        display_v_res,
        display_pixels);

    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to blit to display: %s",
                 esp_err_to_name(res));
        /* Retry even if no later user action/status change requests a frame.
         * The UI task still enforces the normal 100 ms frame interval. */
        if (ui_status_task_handle) xTaskNotifyGive(ui_status_task_handle);
    }
    return res == ESP_OK;
}

/* Startup and shutdown share the embedded logo, with no persistent image
 * allocation during playback. This runs only on the display-owning task. */
static void ui_draw_session_screen(bool exiting)
{
    extern const uint8_t logo_start[] asm("_binary_tanradio_logo_display_png_start");
    extern const uint8_t logo_end[] asm("_binary_tanradio_logo_display_png_end");
    pax_buf_t logo = {0};
    bool loaded = pax_decode_png_buf(&logo, logo_start,
        logo_end - logo_start, PAX_BUF_16_565RGB, 0);
    if (!loaded) ESP_LOGW(TAG, "Session logo unavailable");
    if (exiting) welcome_draw_exit(&framebuffer, loaded ? &logo : NULL);
    else welcome_draw(&framebuffer, loaded ? &logo : NULL);
    if (loaded) pax_buf_destroy(&logo);
}

static void ui_draw_exit_confirmation(void)
{
    pax_draw_round_rect(&framebuffer, UI_COLOR_BORDER, 96, 105, 608, 270, 14);
    pax_draw_round_rect(&framebuffer, UI_COLOR_PANEL, 98, 107, 604, 266, 12);
    pax_draw_text(&framebuffer, UI_COLOR_TEXT, UI_FONT_HEADING,
                  38, 132, 129, "TanRadio Options");
    pax_draw_text(&framebuffer, UI_COLOR_ACCENT, UI_FONT_BODY,
                  UI_TEXT_BODY, 132, 203, "Press F1 again to return to launcher");
    pax_draw_text(&framebuffer, UI_COLOR_TEXT, UI_FONT_BODY,
                  UI_TEXT_BODY, 132, 254, "Press F2 or Esc to cancel");
    pax_draw_text(&framebuffer, UI_COLOR_TEXT, UI_FONT_BODY,
                  UI_TEXT_BODY, 132, 305, "Press F3 to open Settings");
}

/* Call after rendering, with no UI lock held. Never publish the radio exit
 * flag until a splash transfer succeeded and the panel had time to show it. */
static void ui_complete_exit(void)
{
    if (!ui_exit_presented || ui_exit_started) return;
    ui_exit_started = true;
    ESP_LOGI(TAG, "Exit splash submitted; returning to launcher");
    vTaskDelay(pdMS_TO_TICKS(UI_EXIT_SPLASH_MS));
    external_display_stop();
    artwork_submit(NULL, false);
    if (ui_services_started && atomic_load(&ui_services_state) == UI_SERVICES_READY) {
        radio_request_exit();
    } else {
        /* Startup may be in radio_init(), so do not rely on its worker or
         * exit flag. Audio is initialized before the UI/input task starts. */
        if (ui_services_started) audio_shutdown();
        bsp_device_restart_to_launcher();
    }
}

static const char *ui_playback_state_text(void)
{
    switch (ui_playback_state) {
        case UI_PLAYBACK_ERROR: {
            const char *error = radio_get_audio_error();
            return error ? error : "Audio error";
        }

        case UI_PLAYBACK_PLAYING:
            return "Now Playing";

        case UI_PLAYBACK_BUFFERING:
            return "Buffering";

        case UI_PLAYBACK_MUTED:
            return "Muted";

        case UI_PLAYBACK_STOPPED:
        default:
            return "Stopped";
    }
}

static void ui_draw_status_bar(void)
{
    const float ui_width =
        (float)pax_buf_get_width(&framebuffer);
    const float volume_x = ui_width - 525;
    const float battery_x = ui_width - 325;
    const float wifi_x = ui_width - 165;

    audio_status_t audio_status;
    audio_get_status(&audio_status);
    const uint8_t volume = audio_status.volume;

    char volume_line[32];
    snprintf(
        volume_line,
        sizeof(volume_line),
        "%s %u%%",
        audio_status.output == AUDIO_OUTPUT_HEADPHONES ? "Headphones" : "Speaker",
        (unsigned int)volume);

    pax_draw_text(
        &framebuffer,
        UI_COLOR_TEXT,
        UI_FONT_BODY,
        UI_STATUS_FONT_SIZE,
        volume_x,
        UI_STATUS_Y,
        volume_line);

    char battery_line[48];
    if (ui_cached_battery_available) {
        snprintf(battery_line, sizeof(battery_line),
                 ui_cached_battery_charging ? "BAT %u%% CHG" : "BAT %u%%",
                 ui_cached_battery_percent);
    } else {
        snprintf(battery_line, sizeof(battery_line), "BAT N/A");
    }

    pax_draw_text(
        &framebuffer,
        UI_COLOR_TEXT,
        UI_FONT_BODY,
        UI_STATUS_FONT_SIZE,
        battery_x,
        UI_STATUS_Y,
        battery_line);

    if (ui_cached_wifi_connected) {

        char wifi_line[UI_STATUS_SSID_LENGTH + 8];

        snprintf(
            wifi_line,
            sizeof(wifi_line),
            "WiFi %.20s",
            ui_cached_ssid);

        char wifi_display[UI_STATUS_SSID_LENGTH + 8];
        ui_ellipsize_text(UI_FONT_BODY,
            wifi_line,
            wifi_display,
            sizeof(wifi_display),
            UI_STATUS_FONT_SIZE,
            145);

        pax_draw_text(
            &framebuffer,
            UI_COLOR_SUCCESS,
            UI_FONT_BODY,
            UI_STATUS_FONT_SIZE,
            wifi_x,
            UI_STATUS_Y,
            wifi_display);
    } else {
        pax_draw_text(
            &framebuffer,
            UI_COLOR_ERROR,
            UI_FONT_BODY,
            UI_STATUS_FONT_SIZE,
            wifi_x,
            UI_STATUS_Y,
            "WiFi --");
    }
}

static bool ui_status_changed(void)
{
    audio_status_t audio_status;
    audio_get_status(&audio_status);
    const uint8_t volume = audio_status.volume;
    ui_playback_state_t playback_state = UI_PLAYBACK_PLAYING;

    if (radio_get_audio_error()) {
        playback_state = UI_PLAYBACK_ERROR;
    } else if (atomic_load(&ui_services_state) != UI_SERVICES_READY || radio_is_stopped()) {
        playback_state = UI_PLAYBACK_STOPPED;
    } else if (radio_is_muted()) {
        playback_state = UI_PLAYBACK_MUTED;
    } else if (radio_is_buffering()) {
        playback_state = UI_PLAYBACK_BUFFERING;
    }

    bool battery_changed = ui_update_battery();

    char ssid[UI_STATUS_SSID_LENGTH + 1] = {0};

    bool wifi_connected =
        network_is_connected() &&
        network_get_ssid(ssid, sizeof(ssid));

    xSemaphoreTake(ui_mutex, portMAX_DELAY);
    if (!ui_status_cache_valid ||
        ui_cached_volume != volume ||
        ui_cached_output != audio_status.output ||
        battery_changed ||
        ui_cached_wifi_connected != wifi_connected ||
        ui_cached_playback_state != playback_state ||
        strcmp(ui_cached_ssid, ssid) != 0) {

        ui_cached_volume = volume;
        ui_cached_output = audio_status.output;
        ui_cached_wifi_connected = wifi_connected;
        ui_cached_playback_state = playback_state;
        ui_playback_state = playback_state;

        snprintf(
            ui_cached_ssid,
            sizeof(ui_cached_ssid),
            "%s",
            ssid);

        ui_status_cache_valid = true;
        xSemaphoreGive(ui_mutex);
        return true;
    }

    xSemaphoreGive(ui_mutex);
    return false;
}

static void ui_draw_search(void)
{
    radio_browser_status_t search;
    radio_browser_status(&search);
    if (ui_search_selection >= search.count) ui_search_selection = 0;
    pax_draw_text(&framebuffer, UI_COLOR_TEXT, UI_FONT_HEADING,
                  UI_TEXT_TITLE, 24, 72, ui_search_mode == BROWSER_SEARCH_COUNTRY ?
                  "Add station · Country" : "Add station · Name");
    pax_draw_text(&framebuffer, UI_COLOR_SECONDARY, UI_FONT_BODY,
                  UI_TEXT_SMALL, 530, 85, "Radio Browser · MP3");
    pax_draw_round_rect(&framebuffer, UI_COLOR_FIELD, 24, 116, 752, 44, 8);
    char line[160];
    ui_ellipsize_text(UI_FONT_BODY, ui_search_query[0] ? ui_search_query :
                      (ui_search_mode == BROWSER_SEARCH_COUNTRY ? "Country name or code (e.g. Bulgaria or BG)" :
                       "Station name (Enter to search)"),
                      line, sizeof(line), 22, 716);
    pax_draw_text(&framebuffer, UI_COLOR_TEXT, UI_FONT_BODY, 22, 38, 124, line);
    ui_ellipsize_text(UI_FONT_BODY, ui_search_notice[0] ? ui_search_notice : search.message,
                      line, sizeof(line), UI_TEXT_SMALL, 752);
    pax_draw_text(&framebuffer, UI_COLOR_SECONDARY, UI_FONT_BODY, UI_TEXT_SMALL, 24, 170, line);

    size_t first = (ui_search_selection / 5) * 5;
    for (size_t row = 0; row < 5 && first + row < search.count; row++) {
        radio_station_t *station = &ui_render_state.row;
        if (!radio_browser_result(first + row, station)) continue;
        float y = 205 + row * 33;
        if (first + row == ui_search_selection) {
            pax_draw_round_rect(&framebuffer, UI_COLOR_ACCENT, 24, y - 3, 752, 31, 6);
            pax_draw_round_rect(&framebuffer, UI_COLOR_SELECTION, 26, y - 1, 748, 27, 4);
        }
        bool saved = stations_find(station, NULL);
        ui_ellipsize_text(UI_FONT_BODY, station->name, line, sizeof(line), 20, 490);
        pax_draw_text(&framebuffer, UI_COLOR_TEXT, UI_FONT_BODY, 20, 36, y, line);
        if (station->bitrate_kbps) {
            snprintf(line, sizeof(line), "%s%s  %u kbps", saved ? "Saved  " : "",
                     station->country, station->bitrate_kbps);
        } else {
            snprintf(line, sizeof(line), "%s%s  MP3", saved ? "Saved  " : "", station->country);
        }
        char details[160];
        ui_ellipsize_text(UI_FONT_BODY, line, details, sizeof(details), UI_TEXT_SMALL, 218);
        pax_draw_text(&framebuffer, UI_COLOR_SECONDARY, UI_FONT_BODY, UI_TEXT_SMALL, 546, y + 1, details);
    }
    pax_draw_text(&framebuffer, UI_COLOR_SECONDARY, UI_FONT_BODY, UI_TEXT_SMALL, 280, 376,
                  "Tab Name/Country   Enter Search   ↑↓ Select");
    if (search.count) {
        snprintf(line, sizeof(line), "%zu / %zu results", ui_search_selection + 1, search.count);
        pax_draw_text(&framebuffer, UI_COLOR_SECONDARY, UI_FONT_BODY, UI_TEXT_SMALL, 24, 376, line);
    }
    const radio_station_t *playing = &ui_render_state.now_playing.playback.station;
    if (playing->url[0]) {
        snprintf(line, sizeof(line), "%s · %s", ui_playback_state_text(), playing->name);
        char display[160];
        ui_ellipsize_text(UI_FONT_BODY, line, display, sizeof(display), UI_TEXT_SMALL, 740);
        pax_draw_text(&framebuffer, UI_COLOR_SECONDARY, UI_FONT_BODY, UI_TEXT_SMALL, 24, 398, display);
    }
    ui_draw_button_legend();
}

static void ui_draw_remove_dialog(void)
{
    char first[STATIONS_MAX_NAME_LENGTH + 1], second[STATIONS_MAX_NAME_LENGTH + 1];
    const char *remainder = ui_first_line(UI_FONT_BODY, ui_remove_state.station.name,
                                          first, sizeof(first), 28, 700);
    ui_ellipsize_text(UI_FONT_BODY, remainder, second, sizeof(second), 28, 700);
    pax_draw_round_rect(&framebuffer, UI_COLOR_PANEL, 16, UI_CONTENT_TOP,
                        768, UI_CONTENT_BOTTOM - UI_CONTENT_TOP, 12);
    pax_draw_text(&framebuffer, UI_COLOR_TEXT, UI_FONT_HEADING, UI_TEXT_TITLE,
                  36, 104, "Remove this station from saved stations?");
    pax_draw_text(&framebuffer, UI_COLOR_TEXT, UI_FONT_BODY, 28, 36, 164, first);
    pax_draw_text(&framebuffer, UI_COLOR_TEXT, UI_FONT_BODY, 28, 36, 204, second);
    pax_draw_text(&framebuffer, UI_COLOR_SECONDARY, UI_FONT_BODY, 20, 36, 274,
                  "F4 Remove     F2 Cancel");
    if (ui_remove_state.failed) {
        pax_draw_text(&framebuffer, UI_COLOR_WARNING, UI_FONT_BODY, 18, 36, 330,
                      "Could not remove. Check the SD card. F4 retries; F2 cancels.");
    }
    ui_draw_button_legend();
}

static void ui_draw_startup(void)
{
    pax_background(&framebuffer, UI_COLOR_BACKGROUND);
    pax_draw_text(&framebuffer, UI_COLOR_TEXT, UI_FONT_HEADING,
                  UI_TEXT_TITLE, 22, 12, "TanRadio");
    ui_draw_status_bar();
    if (ui_exit_state == UI_EXIT_CONFIRM) {
        ui_draw_exit_confirmation();
        return;
    }
    if (ui_exit_state == UI_EXIT_SETTINGS) {
        ui_settings_draw(&framebuffer, &ui_settings, ui_services_started);
        return;
    }
    pax_draw_round_rect(&framebuffer, UI_COLOR_PANEL, 16, UI_CONTENT_TOP,
                        768, UI_CONTENT_BOTTOM - UI_CONTENT_TOP, 12);
    bool failed = atomic_load(&ui_services_state) == UI_SERVICES_FAILED;
    pax_draw_text(&framebuffer, UI_COLOR_TEXT, UI_FONT_HEADING,
                  UI_TEXT_TITLE, 44, 172, failed ? "Radio startup unavailable" : "Starting radio...");
    pax_draw_text(&framebuffer, UI_COLOR_SECONDARY, UI_FONT_BODY,
                  UI_TEXT_BODY, 44, 232, failed ? "Open F1 Options to return to the launcher and try again." :
                  "Preparing Wi-Fi and your SD-card stations.");
    pax_draw_text(&framebuffer, UI_COLOR_SECONDARY, UI_FONT_BODY,
                  UI_TEXT_SMALL, 24, 439, "F1 Options/Exit     F6 Lights     Volume keys remain available");
}

static void ui_draw_station_logo(const radio_playback_status_t *playback)
{
    const float x = 428, y = 129;
    if (!playback->stopped && ui_artwork_ready && ui_artwork_online == ui_preferences.online_logos &&
        stations_same(&playback->station, &ui_now_playing.playback.station)) {
        static pax_buf_t thumbnail;
        pax_buf_init(&thumbnail, ui_station_logo.color, STATION_LOGO_SIZE, STATION_LOGO_SIZE, PAX_BUF_16_565RGB);
        pax_draw_image_op(&framebuffer, &thumbnail, x, y);
    } else {
        const char *name = playback->station.name;
        char initials[3] = {'?', 0, 0};
        if (*name >= '!' && *name <= '~') initials[0] = *name;
        const char *space = strchr(name, ' ');
        if (space && space[1] >= '!' && space[1] <= '~') initials[1] = space[1];
        pax_draw_round_rect(&framebuffer, UI_COLOR_FIELD, x, y, 48, 48, 7);
        float width = pax_text_size(UI_FONT_HEADING, 23, initials).x;
        pax_draw_text(&framebuffer, UI_COLOR_ACCENT, UI_FONT_HEADING, 23,
                      x + (48 - width) / 2, y + 10, initials);
    }
}

static unsigned ui_scroll_offset(const ui_scroll_label_t *label, TickType_t now)
{
    return text_scroll_at(now - label->started, label->distance,
                          pdMS_TO_TICKS(UI_NOW_PLAYING_INTERVAL_MS), pdMS_TO_TICKS(UI_SCROLL_PAUSE_MS)).offset;
}

static void ui_draw_marquee(ui_scroll_label_t *label, const pax_font_t *font,
                            pax_col_t color, const char *text, unsigned size, int y, int width)
{
    const int x = 44;
    float text_width = pax_text_size(font, size, text).x;
    unsigned overflow = text_width > width ? (unsigned)ceilf(text_width - width) : 0;
    unsigned distance = (overflow + UI_SCROLL_STEP_PX - 1) / UI_SCROLL_STEP_PX;
    TickType_t now = xTaskGetTickCount();
    if (distance != label->distance || strcmp(text, label->text)) {
        strlcpy(label->text, text, sizeof(label->text));
        label->distance = distance;
        label->started = now;
    }
    label->offset = ui_scroll_offset(label, now);
    if (!*text) return;
    if (!overflow) {
        pax_draw_text(&framebuffer, color, font, size, x, y, text);
        return;
    }

    unsigned shift = label->offset * UI_SCROLL_STEP_PX;
    if (shift > overflow) shift = overflow;
    pax_recti clip = pax_get_clip(&framebuffer);
    pax_clip(&framebuffer, x, y, width, size + 4);
    pax_draw_text(&framebuffer, color, font, size, x - (int)shift, y, text);
    pax_set_clip(&framebuffer, clip);
}

static void ui_draw_now_playing(const radio_now_playing_t *snapshot)
{
    if (ui_scroll_connection != snapshot->connection) {
        memset(ui_now_playing_scroll, 0, sizeof(ui_now_playing_scroll));
        ui_scroll_connection = snapshot->connection;
    }
    if (ui_scroll_metadata_revision != snapshot->metadata_revision) {
        ui_now_playing_scroll[UI_SCROLL_TRACK] = (ui_scroll_label_t){0};
        ui_scroll_metadata_revision = snapshot->metadata_revision;
    }

    const radio_playback_status_t *playback = &snapshot->playback;
    const radio_station_t *station = &playback->station;
    const radio_metadata_t *metadata = &snapshot->metadata;
    const char *name = station->name[0] ? station->name : metadata->name;
    char first[160];
    const char *remainder = ui_first_line(UI_FONT_HEADING, name, first, sizeof(first), UI_TEXT_STATION, 364);
    pax_draw_text(&framebuffer, UI_COLOR_TEXT, UI_FONT_HEADING, UI_TEXT_STATION, 44, 122, first);
    ui_draw_marquee(&ui_now_playing_scroll[UI_SCROLL_NAME], UI_FONT_HEADING, UI_COLOR_TEXT,
                    remainder, UI_TEXT_STATION, 164, 364);
    ui_draw_station_logo(playback);

    char details[112] = {0};
    uint16_t rate = metadata->bitrate_kbps ? metadata->bitrate_kbps : station->bitrate_kbps;
    snprintf(details, sizeof(details), "%s%s%s", station->country,
             station->country[0] && metadata->genre[0] ? " · " : "", metadata->genre);
    if (rate) {
        size_t used = strlen(details);
        snprintf(details + used, sizeof(details) - used, "%s%u kbps", used ? " · " : "", (unsigned)rate);
    }
    ui_draw_marquee(&ui_now_playing_scroll[UI_SCROLL_DETAILS], UI_FONT_BODY, UI_COLOR_SECONDARY,
                    details, UI_TEXT_SMALL, 212, 432);

    const char *title = metadata->title;
    if (*title) {
        remainder = ui_first_line(UI_FONT_BODY, title, first, sizeof(first), 25, 432);
        pax_draw_text(&framebuffer, UI_COLOR_TEXT, UI_FONT_BODY, 25, 44, 252, first);
        ui_draw_marquee(&ui_now_playing_scroll[UI_SCROLL_TRACK], UI_FONT_BODY, UI_COLOR_TEXT,
                        remainder, 25, 283, 432);
    } else {
        const char *description = metadata->description[0] ? metadata->description :
                                  strcmp(name, metadata->name) ? metadata->name : "";
        ui_draw_marquee(&ui_now_playing_scroll[UI_SCROLL_TRACK], UI_FONT_BODY, UI_COLOR_SECONDARY,
                        description, UI_TEXT_SMALL, 264, 432);
    }
}

static void ui_render(void)
{
    if (ui_mutex != NULL) {
        xSemaphoreTake(ui_mutex, portMAX_DELAY);
    }

    if (ui_exit_state == UI_EXIT_COMMITTED) {
        ui_draw_session_screen(true);
        ui_exit_presented = ui_blit();
        if (ui_mutex) xSemaphoreGive(ui_mutex);
        return;
    }

    if (atomic_load(&ui_services_state) != UI_SERVICES_READY) {
        ui_draw_startup();
        if (ui_about_open && ui_exit_state == UI_EXIT_IDLE) ui_about_draw(&framebuffer);
        ui_blit();
        if (ui_mutex) xSemaphoreGive(ui_mutex);
        return;
    }

    radio_playback_status_t *playback = &ui_render_state.now_playing.playback;
    radio_get_now_playing(&ui_render_state.now_playing);
    ui_station_list_refresh(&ui_station_list, &playback->station);
    ui_playback_state = radio_get_audio_error() ? UI_PLAYBACK_ERROR : playback->stopped ? UI_PLAYBACK_STOPPED :
        playback->muted ? UI_PLAYBACK_MUTED :
        playback->buffering ? UI_PLAYBACK_BUFFERING : UI_PLAYBACK_PLAYING;

    pax_background(&framebuffer, UI_COLOR_BACKGROUND);

    const float ui_width =
        (float)pax_buf_get_width(&framebuffer);

    pax_draw_text(
        &framebuffer,
        UI_COLOR_TEXT,
        UI_FONT_HEADING,
        UI_TEXT_TITLE,
        22,
        12,
        "TanRadio");

    ui_draw_status_bar();

    pax_draw_line(
        &framebuffer,
        UI_COLOR_BORDER,
        8,
        UI_HEADER_HEIGHT,
        ui_width - 8,
        UI_HEADER_HEIGHT);

    if (ui_exit_state == UI_EXIT_CONFIRM || ui_exit_state == UI_EXIT_SETTINGS) {
        if (ui_exit_state == UI_EXIT_CONFIRM) ui_draw_exit_confirmation();
        else ui_settings_draw(&framebuffer, &ui_settings, ui_services_started);
        ui_blit();
        if (ui_mutex) xSemaphoreGive(ui_mutex);
        return;
    }

    if (ui_about_open) {
        ui_about_draw(&framebuffer);
        ui_blit();
        if (ui_mutex) xSemaphoreGive(ui_mutex);
        return;
    }

    if (ui_remove_state.open) {
        ui_draw_remove_dialog();
        ui_blit();
        if (ui_mutex) xSemaphoreGive(ui_mutex);
        return;
    }

    if (ui_search_open) {
        ui_draw_search();
        ui_blit();
        if (ui_mutex) xSemaphoreGive(ui_mutex);
        return;
    }

    size_t saved_count = radio_station_count();
    size_t station_count = saved_count + (ui_station_list.has_preview ? 1 : 0);
    const radio_station_t *playing = &playback->station;
    bool have_playing = playing->url[0] != '\0';

    if (!have_playing) memset(ui_now_playing_scroll, 0, sizeof(ui_now_playing_scroll));
    if (station_count == 0 && !have_playing) {
        pax_draw_round_rect(
            &framebuffer,
            UI_COLOR_PANEL,
            16,
            UI_CONTENT_TOP,
            ui_width - 32,
            UI_CONTENT_BOTTOM - UI_CONTENT_TOP,
            12);

        pax_draw_text(
            &framebuffer,
            UI_COLOR_TEXT,
            UI_FONT_HEADING,
            UI_TEXT_TITLE,
            36,
            210,
            "No stations configured");

        pax_draw_text(
            &framebuffer,
            UI_COLOR_SECONDARY,
            UI_FONT_BODY,
            22,
            36,
            260,
            "Press F2 to find and save stations");

        ui_draw_button_legend();
        ui_blit();

        if (ui_mutex != NULL) {
            xSemaphoreGive(ui_mutex);
        }

        return;
    }

    const pax_col_t playback_state_color =
        ui_playback_state == UI_PLAYBACK_BUFFERING ? UI_COLOR_WARNING :
        ui_playback_state == UI_PLAYBACK_PLAYING ? UI_COLOR_SUCCESS : UI_COLOR_TEXT;

    pax_draw_round_rect(&framebuffer, UI_COLOR_PANEL, 16, UI_CONTENT_TOP, UI_NOW_PLAYING_WIDTH,
                        UI_CONTENT_BOTTOM - UI_CONTENT_TOP, 12);
    pax_draw_text(&framebuffer, UI_COLOR_SECONDARY, UI_FONT_BODY, UI_TEXT_SMALL,
                  44, 100, "CURRENT STATION");
    if (have_playing) ui_draw_now_playing(&ui_render_state.now_playing);

    pax_draw_text(
        &framebuffer,
        playback_state_color,
        UI_FONT_BODY,
        21,
        44,
        350,
        ui_playback_state_text());

    pax_draw_text(
        &framebuffer,
        UI_COLOR_SECONDARY,
        UI_FONT_BODY,
        UI_TEXT_SMALL,
        UI_STATION_LIST_X,
        84,
        "STATIONS");

    pax_draw_text(
        &framebuffer,
        UI_COLOR_SECONDARY,
        UI_FONT_BODY,
        UI_TEXT_SMALL,
        UI_STATION_LIST_X + 156,
        85,
        "↑↓ Browse");

    if (ui_station_list.selection < saved_count) {
        pax_draw_text(&framebuffer, UI_COLOR_SECONDARY, UI_FONT_BODY, UI_TEXT_SMALL,
                      UI_STATION_LIST_X, 386, "Backspace Remove");
    }

    size_t visible = station_count < UI_STATION_VISIBLE_COUNT ? station_count : UI_STATION_VISIBLE_COUNT;
    size_t selected_row = visible / 2;
    size_t selection_ordinal = ui_station_list.selection == SIZE_MAX ? saved_count : ui_station_list.selection;
    for (size_t row = 0;
         row < visible;
         row++) {

        int offset =
            (int)row - (int)selected_row;

        size_t index = (selection_ordinal + 2 * station_count + offset) % station_count;

        const char *station_name =
            index == saved_count ? ui_station_list.preview.name : radio_station_get_name(index);

        if (station_name == NULL) {
            continue;
        }

        char station_line[128];

        if (index == saved_count) {
            snprintf(station_line, sizeof(station_line), "Preview: %s", station_name);
        } else {
            snprintf(station_line, sizeof(station_line), "%zu. %s", index + 1, station_name);
        }

        const bool selected = row == selected_row;
        const float row_y = UI_STATION_FIRST_Y +
            ((row + UI_STATION_SELECTED_ROW - selected_row) * UI_STATION_ROW_HEIGHT);
        char station_line_display[128];
        const float station_font_size = selected ? UI_TEXT_BODY : 20;
        ui_ellipsize_text(UI_FONT_BODY,
            station_line,
            station_line_display,
            sizeof(station_line_display),
            station_font_size,
            UI_STATION_LIST_WIDTH - 32);

        if (selected) {
            pax_draw_round_rect(
                &framebuffer,
                UI_COLOR_ACCENT,
                UI_STATION_LIST_X,
                row_y - 8,
                UI_STATION_LIST_WIDTH,
                48,
                8);
            pax_draw_round_rect(&framebuffer, UI_COLOR_SELECTION,
                                UI_STATION_LIST_X + 2, row_y - 6,
                                UI_STATION_LIST_WIDTH - 4, 44, 6);
        }

        pax_draw_text(
            &framebuffer,
            UI_COLOR_TEXT,
            UI_FONT_BODY,
            station_font_size,
            UI_STATION_LIST_X + 14,
            row_y,
            station_line_display);
    }

    ui_draw_button_legend();
    ui_blit();

    if (ui_mutex != NULL) {
        xSemaphoreGive(ui_mutex);
    }
}

/*******************************************************************************
 * Public Functions
 ******************************************************************************/

/* Input and playback callers only request a frame. Rendering belongs to the
 * UI task, so opening search cannot exhaust the input task's stack. */
static void ui_draw(void)
{
    if (ui_status_task_handle) xTaskNotifyGive(ui_status_task_handle);
}

void ui_set_services_state(ui_services_state_t state)
{
    atomic_store(&ui_services_state, state);
    ui_draw();
}

bool ui_handle_exit_navigation(bsp_input_navigation_key_t key)
{
    xSemaphoreTake(ui_mutex, portMAX_DELAY);
    ui_exit_state_t previous = ui_exit_state;
    bool previous_about = ui_about_open;
    bool consumed = ui_exit_state == UI_EXIT_IDLE && ui_about_navigation(&ui_about_open, key);
    ui_exit_state_t next = previous;
    if (!consumed) consumed = ui_exit_navigation(&next, key);
    if (next == UI_EXIT_COMMITTED && previous != next && ui_services_started) {
        /* Keep the restart behind the splash, but mute and close PCM admission
         * on the confirming key press. The input task owns this transition. */
        xSemaphoreGive(ui_mutex);
        radio_stop_for_exit();
        xSemaphoreTake(ui_mutex, portMAX_DELAY);
    }
    ui_exit_state = next;
    bool changed = previous != next || previous_about != ui_about_open;
    bool apply = previous == UI_EXIT_SETTINGS && key == BSP_INPUT_NAVIGATION_KEY_F4;
    ui_preferences_t draft = ui_settings.draft;
    if (ui_exit_state == UI_EXIT_SETTINGS) {
        if (previous != UI_EXIT_SETTINGS) {
            ui_settings = (ui_settings_t){.draft = ui_preferences};
        } else if (!apply) {
            changed = ui_settings_edit(&ui_settings, key) || changed;
        }
    }
    bool wake = changed && (ui_exit_state != UI_EXIT_IDLE || ui_about_open) && !ui_display_enabled;
    xSemaphoreGive(ui_mutex);
    if (apply) {
        bool saved = ui_preferences_save(&draft);
        bool led_applied = saved && bsp_led_set_brightness(draft.led_brightness) == ESP_OK;
        xSemaphoreTake(ui_mutex, portMAX_DELAY);
        if (saved) {
            ui_preferences = draft;
            ++ui_settings_revision;
        }
        ui_settings.notice = !saved ? "Could not save settings · F4 retries" :
                             !led_applied ? "Settings saved · LED update failed · F4 retries" :
                             "Settings applied";
        xSemaphoreGive(ui_mutex);
        changed = true;
    }
    if (wake) ui_toggle_display();
    if (changed) ui_draw();
    return consumed;
}

static bool ui_remove_navigation(bsp_input_navigation_key_t key)
{
    xSemaphoreTake(ui_mutex, portMAX_DELAY);
    if (!ui_remove_state.open) {
        if (ui_search_open || key != BSP_INPUT_NAVIGATION_KEY_BACKSPACE) {
            xSemaphoreGive(ui_mutex);
            return false;
        }
        ui_refresh_preview();
        if (ui_station_list.selection < stations_count() &&
            ui_main_selected_station(&ui_remove_state.station)) {
            ui_remove_state.index = ui_station_list.selection;
            ui_remove_state.failed = false;
            ui_remove_state.open = true;
        }
    } else if (key == BSP_INPUT_NAVIGATION_KEY_F2 || key == BSP_INPUT_NAVIGATION_KEY_ESC) {
        ui_remove_state.open = false;
    } else if (key == BSP_INPUT_NAVIGATION_KEY_F4) {
        /* Unlike append, removal compacts published records. Exclude catalog
         * readers with ui_mutex throughout the bounded SD transaction. The
         * audio worker only uses owned records and never needs this mutex. */
        esp_err_t result = stations_remove(STATIONS_FILE_PATH, ui_remove_state.index);
        ui_remove_state.failed = result != ESP_OK;
        if (result == ESP_OK) {
            radio_catalog_changed();
            ui_remove_state.open = false;
            ui_refresh_preview();
            /* Removing the active entry keeps it selected as a Preview. */
            if (ui_station_list.has_preview &&
                stations_same(&ui_station_list.preview, &ui_remove_state.station)) {
                ui_station_list.selection = SIZE_MAX;
            } else if (ui_remove_state.index < stations_count()) {
                ui_station_list.selection = ui_remove_state.index;
            } else if (stations_count()) {
                ui_station_list.selection = stations_count() - 1;
            }
        }
    } else if (key == BSP_INPUT_NAVIGATION_KEY_F3 || key == BSP_INPUT_NAVIGATION_KEY_F6) {
        xSemaphoreGive(ui_mutex);
        return false; /* Keep Stop and Lights available in the dialog. */
    }
    xSemaphoreGive(ui_mutex);
    ui_draw();
    return true;
}

static bool ui_search_navigation(bsp_input_navigation_key_t key)
{
    xSemaphoreTake(ui_mutex, portMAX_DELAY);
    if (!ui_search_open) {
        if (key != BSP_INPUT_NAVIGATION_KEY_F2) {
            xSemaphoreGive(ui_mutex);
            return false;
        }
        ui_search_open = true;
        ui_search_selection = 0;
        ui_search_notice[0] = 0;
        radio_browser_cancel();
        xSemaphoreGive(ui_mutex);
        ui_draw();
        return true;
    }
    radio_browser_status_t search;
    radio_browser_status(&search);
    if (key == BSP_INPUT_NAVIGATION_KEY_ESC || key == BSP_INPUT_NAVIGATION_KEY_F2) {
        ui_search_open = false;
        radio_browser_cancel();
        ui_refresh_preview();
        if (ui_station_list.has_preview) ui_station_list.selection = SIZE_MAX;
    } else if (key == BSP_INPUT_NAVIGATION_KEY_TAB) {
        ui_search_mode = ui_search_mode == BROWSER_SEARCH_NAME ? BROWSER_SEARCH_COUNTRY : BROWSER_SEARCH_NAME;
        ui_search_query[0] = 0;
        ui_search_notice[0] = 0;
        ui_search_selection = 0;
        radio_browser_cancel();
    } else if (key == BSP_INPUT_NAVIGATION_KEY_BACKSPACE) {
        size_t length = strlen(ui_search_query);
        if (length) {
            do { length--; } while (length && ((unsigned char)ui_search_query[length] & 0xc0) == 0x80);
            ui_search_query[length] = 0;
            ui_search_notice[0] = 0;
            ui_search_selection = 0;
            radio_browser_cancel();
        }
    } else if (key == BSP_INPUT_NAVIGATION_KEY_RETURN) {
        bool nonspace = false;
        for (const unsigned char *p = (const unsigned char *)ui_search_query; *p; p++) {
            if (*p > 32) nonspace = true;
        }
        ui_search_notice[0] = 0;
        ui_search_selection = 0;
        if (nonspace) {
            if (!radio_browser_search_by(ui_search_query, ui_search_mode)) {
                snprintf(ui_search_notice, sizeof(ui_search_notice), "Search unavailable or busy. Press Enter to retry.");
            }
        } else {
            snprintf(ui_search_notice, sizeof(ui_search_notice), "%s", ui_search_mode == BROWSER_SEARCH_COUNTRY ?
                     "Type a country name or code first." : "Type a station name first.");
        }
    } else if (key == BSP_INPUT_NAVIGATION_KEY_UP && search.count) {
        ui_search_selection = (ui_search_selection + search.count - 1) % search.count;
        ui_search_notice[0] = 0;
    } else if (key == BSP_INPUT_NAVIGATION_KEY_DOWN && search.count) {
        ui_search_selection = (ui_search_selection + 1) % search.count;
        ui_search_notice[0] = 0;
    } else if (key == BSP_INPUT_NAVIGATION_KEY_F4 || key == BSP_INPUT_NAVIGATION_KEY_F5) {
        radio_station_t station;
        bool selected = radio_browser_result(ui_search_selection, &station);
        /* Audio control and SD I/O never run under the framebuffer mutex. */
        xSemaphoreGive(ui_mutex);
        char notice[96];
        size_t saved_index = 0;
        bool saved = false;
        if (!selected) {
            snprintf(notice, sizeof(notice), "Select a search result first.");
        } else if (key == BSP_INPUT_NAVIGATION_KEY_F4) {
            bool playing = ui_activate_station(&station, true);
            snprintf(notice, sizeof(notice), "%s", playing ? "" :
                     "Playback unavailable. Check the SD card and restart TanRadio.");
        } else {
            bool duplicate;
            esp_err_t result = stations_save(STATIONS_FILE_PATH, &station, &saved_index, &duplicate);
            saved = result == ESP_OK;
            snprintf(notice, sizeof(notice), "%s", saved ? (duplicate ? "Already saved in your stations." :
                     "Saved to your stations.") : result == ESP_ERR_NO_MEM ? "Station list is full (128 stations)." :
                     result == ESP_ERR_INVALID_SIZE ? "Station file is full (64 KiB)." :
                     "Could not save. Check the SD card and free space.");
        }
        xSemaphoreTake(ui_mutex, portMAX_DELAY);
        if (saved) ui_station_list.selection = saved_index;
        else if (selected && key == BSP_INPUT_NAVIGATION_KEY_F4) ui_focus_current_station();
        ui_refresh_preview();
        snprintf(ui_search_notice, sizeof(ui_search_notice), "%s", notice);
    } else if (key == BSP_INPUT_NAVIGATION_KEY_F3 ||
               key == BSP_INPUT_NAVIGATION_KEY_F6) {
        xSemaphoreGive(ui_mutex);
        return false; /* Existing stop and illumination controls. */
    } else {
        xSemaphoreGive(ui_mutex);
        return true; /* Text editing is handled by keyboard events only. */
    }
    xSemaphoreGive(ui_mutex);
    ui_draw();
    return true;
}

void ui_toggle_display(void)
{
    bool enable_display;
    uint8_t brightness = ui_saved_backlight_brightness;

    if (ui_mutex != NULL) {
        xSemaphoreTake(ui_mutex, portMAX_DELAY);
    }

    /* Keep confirmation and the goodbye visible, including power actions.
     * A first F1 may still restore illumination when the screen was dark. */
    if (ui_exit_state != UI_EXIT_IDLE && ui_display_enabled) {
        if (ui_mutex) xSemaphoreGive(ui_mutex);
        return;
    }

    enable_display = !ui_display_enabled;

    if (!enable_display) {
        uint8_t current_brightness = 0;

        if (bsp_display_get_backlight_brightness(
                &current_brightness) == ESP_OK &&
            current_brightness > 0) {
            ui_saved_backlight_brightness = current_brightness;
        }

        brightness = 0;
    } else {
        brightness = ui_saved_backlight_brightness;
    }

    esp_err_t res;
    esp_err_t keyboard_res;

    /*
     * When enabling, restore the display first, then restore the keyboard at
     * the verified-stable 50% PWM level. Higher keyboard PWM values can turn
     * the physical backlight off after a zero-brightness transition. When
     * disabling, turn the keyboard fully off before the display.
     */
    if (enable_display) {
        res = bsp_display_set_backlight_brightness(brightness);

        if (res == ESP_OK) {
            keyboard_res = ui_restore_keyboard_backlight();
        } else {
            keyboard_res = ESP_ERR_INVALID_STATE;
        }
    } else {
        keyboard_res =
            bsp_input_set_backlight_brightness(0);
        res = bsp_display_set_backlight_brightness(brightness);
    }

    if (keyboard_res != ESP_OK &&
        !(enable_display && res != ESP_OK)) {
        ESP_LOGE(
            TAG,
            "Failed to set keyboard backlight: %s",
            esp_err_to_name(keyboard_res));
    }

    if (res == ESP_OK) {
        ui_display_enabled = enable_display;
        if (enable_display) {
            ui_battery_refresh_requested = true;
        }

        if (keyboard_res == ESP_OK) {
            ESP_LOGI(
                TAG,
                "Display and keyboard backlight %s",
                enable_display ? "enabled" : "disabled");
        } else {
            ESP_LOGW(
                TAG,
                "Display %s, but keyboard backlight update failed",
                enable_display ? "enabled" : "disabled");
        }
    } else {
        ESP_LOGE(
            TAG,
            "Failed to set display backlight: %s",
            esp_err_to_name(res));
    }

    if (ui_mutex != NULL) {
        xSemaphoreGive(ui_mutex);
    }

    if (res == ESP_OK && enable_display) {
        if (ui_status_task_handle != NULL) {
            xTaskNotifyGive(ui_status_task_handle);
        }
        ui_draw();
    }
}

/**
 * @brief Initialize the TanRadio user interface.
 */
void ui_handle_navigation(bsp_input_navigation_key_t key, bool state)
{
    if (!state) return;
    if (atomic_load(&ui_services_state) != UI_SERVICES_READY) {
        if (key == BSP_INPUT_NAVIGATION_KEY_F6) ui_toggle_display();
        return;
    }
    if (ui_remove_navigation(key)) return;
    if (ui_search_navigation(key)) return;
    if (key == BSP_INPUT_NAVIGATION_KEY_F6) {
        ui_toggle_display();
        return;
    }
    if (key == BSP_INPUT_NAVIGATION_KEY_F3) {
        radio_stop();
        ui_draw();
        return;
    }

    radio_station_t selected;
    bool activate = false;
    xSemaphoreTake(ui_mutex, portMAX_DELAY);
    ui_refresh_preview();
    switch (key) {
        case BSP_INPUT_NAVIGATION_KEY_UP:
            ui_station_list_move(&ui_station_list, -1);
            break;
        case BSP_INPUT_NAVIGATION_KEY_DOWN:
            ui_station_list_move(&ui_station_list, 1);
            break;
        case BSP_INPUT_NAVIGATION_KEY_ESC:
            ui_focus_current_station();
            break;
        case BSP_INPUT_NAVIGATION_KEY_RETURN:
        case BSP_INPUT_NAVIGATION_KEY_F4:
            activate = ui_main_selected_station(&selected);
            break;
        case BSP_INPUT_NAVIGATION_KEY_F5: {
            size_t count = stations_count();
            if (!count) break;
            radio_playback_status_t playback;
            radio_get_playback_status(&playback);
            size_t current;
            ui_station_list.selection = stations_find(&playback.station, &current) ? (current + 1) % count : 0;
            activate = ui_main_selected_station(&selected);
            break;
        }
        default:
            xSemaphoreGive(ui_mutex);
            return;
    }
    xSemaphoreGive(ui_mutex);
    if (activate && !ui_activate_station(&selected, key == BSP_INPUT_NAVIGATION_KEY_F4)) {
        ESP_LOGW(TAG, "Unable to activate selected station");
    }
    ui_draw();
}

/**
 * @brief Handle a BSP keyboard event.
 */
void ui_handle_keyboard(
    char ascii,
    const char *utf8,
    uint32_t modifiers)
{
    if (utf8 == NULL) {
        ESP_LOGW(
            TAG,
            "Keyboard event received with NULL UTF-8 string");
        return;
    }

    if ((unsigned char)utf8[0] >= 32 && (unsigned char)utf8[0] != 127 &&
        !(modifiers & (BSP_INPUT_MODIFIER_CTRL | BSP_INPUT_MODIFIER_ALT_L | BSP_INPUT_MODIFIER_SUPER))) {
        xSemaphoreTake(ui_mutex, portMAX_DELAY);
        if (ui_about_open || ui_exit_state != UI_EXIT_IDLE || atomic_load(&ui_services_state) != UI_SERVICES_READY) {
            xSemaphoreGive(ui_mutex);
            return;
        }
        if (ui_search_open) {
            size_t length = strlen(ui_search_query), added = strlen(utf8);
            if (added && length + added <= BROWSER_QUERY_LENGTH) {
                memcpy(ui_search_query + length, utf8, added + 1);
                ui_search_notice[0] = 0;
                ui_search_selection = 0;
                radio_browser_cancel();
            } else if (added) {
                snprintf(ui_search_notice, sizeof(ui_search_notice),
                         "Search text is limited to %d bytes.", BROWSER_QUERY_LENGTH);
            }
            xSemaphoreGive(ui_mutex);
            ui_draw();
            return;
        }
        xSemaphoreGive(ui_mutex);
    }

    ESP_LOGD(
        TAG,
        "Keyboard input: ASCII=0x%02x UTF-8=\"%s\" modifiers=0x%08lx",
        (unsigned int)(unsigned char)ascii,
        utf8,
        (unsigned long)modifiers);

}

/**
 * @brief Handle a BSP scancode event.
 */
void ui_handle_scancode(
    bsp_input_scancode_t scancode)
{
    ESP_LOGD(
        TAG,
        "Keyboard scancode: 0x%02x",
        (unsigned int)scancode);
}

void ui_set_playback_state(ui_playback_state_t state)
{
    ui_playback_state = state;
    ui_draw();
}

ui_playback_state_t ui_get_playback_state(void)
{
    return ui_playback_state;
}

static bool ui_sync_now_playing(void)
{
    static unsigned applied_settings;
    static external_display_status_t display_status;
    external_frame_t frame = {0};
    xSemaphoreTake(ui_mutex, portMAX_DELAY);
    ui_preferences_t preferences = ui_preferences;
    if (preferences.display != EXTERNAL_DISPLAY_OFF) {
        frame.battery_percent = ui_cached_battery_percent;
        frame.battery_available = ui_cached_battery_available;
        frame.battery_charging = ui_cached_battery_charging;
    }
    unsigned revision = ui_settings_revision;
    bool closing = ui_exit_state == UI_EXIT_COMMITTED;
    bool settings = ui_exit_state == UI_EXIT_SETTINGS;
    bool scroll_visible = ui_display_enabled && ui_exit_state == UI_EXIT_IDLE && !ui_search_open &&
                          !ui_about_open && !ui_remove_state.open;
    xSemaphoreGive(ui_mutex);
    if (closing) return false;

    uint32_t previous = ui_now_playing.metadata_revision;
    if (atomic_load(&ui_services_state) == UI_SERVICES_READY)
        radio_get_now_playing(&ui_now_playing);
    else ui_now_playing.playback.stopped = true;
    const radio_playback_status_t *playback = &ui_now_playing.playback;
    bool active = !playback->stopped && playback->station.url[0];
    unsigned request = artwork_submit(active ? &playback->station : NULL, preferences.online_logos);
    bool changed = previous != ui_now_playing.metadata_revision;
    if (scroll_visible && playback->station.url[0]) {
        TickType_t now = xTaskGetTickCount();
        for (unsigned i = 0; i < UI_SCROLL_COUNT; i++) {
            const ui_scroll_label_t *label = &ui_now_playing_scroll[i];
            if (label->offset != ui_scroll_offset(label, now)) changed = true;
        }
    }
    if (request != ui_artwork_request) {
        ui_artwork_request = request;
        ui_artwork_ready = false;
        ui_artwork_online = preferences.online_logos;
        changed = true;
    }
    if (active && !ui_artwork_ready && artwork_copy(request, &ui_station_logo)) {
        ui_artwork_ready = true;
        changed = true;
    }

    const char *state = radio_get_audio_error() ? "Audio error" :
                        playback->stopped ? "Stopped" :
                        playback->muted ? "Muted" :
                        playback->buffering ? "Buffering" : "Playing";
    if (ui_external_state != state) {
        ui_external_state = state;
        changed = true;
    }
    if (preferences.display != EXTERNAL_DISPLAY_OFF) {
        frame.volume_percent = audio_get_volume();
        frame.muted = playback->muted;
        strlcpy(frame.station, playback->station.name[0] ? playback->station.name : "TanRadio",
                sizeof(frame.station));
        strlcpy(frame.title, ui_now_playing.metadata.title, sizeof(frame.title));
        snprintf(frame.state, sizeof(frame.state), "%s", state);
        frame.have_logo = active && ui_artwork_ready;
        if (frame.have_logo) memcpy(frame.logo, ui_station_logo.mono, sizeof(frame.logo));
    }
    bool retry = revision != applied_settings && external_display_status() == EXTERNAL_FAILED;
    external_display_submit(preferences.display, preferences.contrast, &frame, retry);
    applied_settings = revision;
    external_display_status_t current = external_display_status();
    if (settings && current != display_status) changed = true;
    display_status = current;
    return changed;
}

static void ui_status_task(void *arg)
{
    (void)arg;
    const TickType_t poll_interval = pdMS_TO_TICKS(1000);
    const TickType_t frame_interval = pdMS_TO_TICKS(UI_FRAME_INTERVAL_MS);
    const TickType_t playback_interval = pdMS_TO_TICKS(UI_NOW_PLAYING_INTERVAL_MS);
    TickType_t last_playback = xTaskGetTickCount() - playback_interval;
    TickType_t last_poll = xTaskGetTickCount();
    TickType_t last_frame = last_poll - frame_interval;

    while (true) {
        TickType_t elapsed = xTaskGetTickCount() - last_poll;
        TickType_t wait = elapsed < poll_interval ? poll_interval - elapsed : 0;
        elapsed = xTaskGetTickCount() - last_playback;
        TickType_t playback_wait = elapsed < playback_interval ? playback_interval - elapsed : 0;
        if (playback_wait < wait) wait = playback_wait;
        bool changed = ulTaskNotifyTake(pdTRUE, wait) > 0;
        TickType_t now = xTaskGetTickCount();
        if (now - last_playback >= playback_interval) {
            last_playback = now;
            changed = ui_sync_now_playing() || changed;
        }
        if (now - last_poll >= poll_interval) {
            last_poll = now;
            /* Rapid typing requests frames, not Wi-Fi/coproc status RPCs.
             * Keep hardware polling on its original one-second cadence. */
            changed = ui_status_changed() || changed;
        }
        xSemaphoreTake(ui_mutex, portMAX_DELAY);
        if (ui_exit_state == UI_EXIT_COMMITTED) changed = true;
        if (ui_search_open) {
            radio_browser_status_t search;
            radio_browser_status(&search);
            if (search.revision != ui_search_revision) {
                ui_search_revision = search.revision;
                changed = true;
            }
        }
        xSemaphoreGive(ui_mutex);
        if (changed) {
            /* Coalesce rapid selection/playback updates into the latest UI.
             * Full-frame rendering/copies compete with audio and TLS for
             * PSRAM bandwidth; keep them to at most ten frames per second. */
            TickType_t since_frame = xTaskGetTickCount() - last_frame;
            if (since_frame < frame_interval) {
                vTaskDelay(frame_interval - since_frame);
            }
            /* The renderer reads current state, including requests that
             * arrived while waiting. Requests during rendering remain queued. */
            ulTaskNotifyTake(pdTRUE, 0);
            last_frame = xTaskGetTickCount();
            ui_render();
            ui_complete_exit();
        }
    }
}

void ui_init(void)
{
    esp_err_t res;

    /*
     * Get display parameters from the BSP.
     *
     * The BSP owns the display hardware and provides the
     * parameters required by the application graphics layer.
     */
    res = bsp_display_get_parameters(
        &display_h_res,
        &display_v_res,
        &display_color_format,
        &display_data_endian);

    if (res == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGI(TAG, "Display not supported");
        return;
    }

    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get display parameters: %s",
                 esp_err_to_name(res));
        return;
    }

    /*
     * Convert the BSP display color format to the corresponding
     * PAX framebuffer format.
     */
    pax_buf_type_t format = PAX_BUF_24_888RGB;

    switch (display_color_format) {
        case BSP_DISPLAY_COLOR_FORMAT_1_PAL:
            format = PAX_BUF_1_PAL;
            break;

        case BSP_DISPLAY_COLOR_FORMAT_2_PAL:
            format = PAX_BUF_2_PAL;
            break;

        case BSP_DISPLAY_COLOR_FORMAT_4_PAL:
            format = PAX_BUF_4_PAL;
            break;

        case BSP_DISPLAY_COLOR_FORMAT_8_PAL:
            format = PAX_BUF_8_PAL;
            break;

        case BSP_DISPLAY_COLOR_FORMAT_16_PAL:
            format = PAX_BUF_16_PAL;
            break;

        case BSP_DISPLAY_COLOR_FORMAT_1_GREY:
            format = PAX_BUF_1_GREY;
            break;

        case BSP_DISPLAY_COLOR_FORMAT_2_GREY:
            format = PAX_BUF_2_GREY;
            break;

        case BSP_DISPLAY_COLOR_FORMAT_4_GREY:
            format = PAX_BUF_4_GREY;
            break;

        case BSP_DISPLAY_COLOR_FORMAT_8_GREY:
            format = PAX_BUF_8_GREY;
            break;

        case BSP_DISPLAY_COLOR_FORMAT_8_332RGB:
            format = PAX_BUF_8_332RGB;
            break;

        case BSP_DISPLAY_COLOR_FORMAT_16_565RGB:
            format = PAX_BUF_16_565RGB;
            break;

        case BSP_DISPLAY_COLOR_FORMAT_4_1111ARGB:
            format = PAX_BUF_4_1111ARGB;
            break;

        case BSP_DISPLAY_COLOR_FORMAT_8_2222ARGB:
            format = PAX_BUF_8_2222ARGB;
            break;

        case BSP_DISPLAY_COLOR_FORMAT_16_4444ARGB:
            format = PAX_BUF_16_4444ARGB;
            break;

        case BSP_DISPLAY_COLOR_FORMAT_24_888RGB:
            format = PAX_BUF_24_888RGB;
            break;

        case BSP_DISPLAY_COLOR_FORMAT_32_8888ARGB:
            format = PAX_BUF_32_8888ARGB;
            break;

        case BSP_DISPLAY_COLOR_FORMAT_18_666RGB:
        default:
            ESP_LOGW(TAG,
                     "BSP display color format not supported by PAX (%u)",
                     display_color_format);
            return;
    }

    /*
     * Convert BSP display rotation to PAX orientation.
     */
    bsp_display_rotation_t display_rotation =
        bsp_display_get_default_rotation();

    pax_orientation_t orientation = PAX_O_UPRIGHT;

    switch (display_rotation) {
        case BSP_DISPLAY_ROTATION_90:
            orientation = PAX_O_ROT_CCW;
            break;

        case BSP_DISPLAY_ROTATION_180:
            orientation = PAX_O_ROT_HALF;
            break;

        case BSP_DISPLAY_ROTATION_270:
            orientation = PAX_O_ROT_CW;
            break;

        case BSP_DISPLAY_ROTATION_0:
        default:
            orientation = PAX_O_UPRIGHT;
            break;
    }

    /*
     * Initialize the PAX framebuffer.
     *
     * The framebuffer belongs to TanRadio.
     * The physical display remains owned by the BSP.
     */
    pax_buf_init(
        &framebuffer,
        NULL,
        display_h_res,
        display_v_res,
        format);

    pax_buf_reversed(
        &framebuffer,
        display_data_endian == BSP_DISPLAY_ENDIAN_BIG);

    pax_buf_set_orientation(
        &framebuffer,
        orientation);

    ESP_LOGI(TAG,
             "Display initialized: %zux%zu format=%u endian=%u rotation=%u",
             display_h_res,
             display_v_res,
             display_color_format,
             display_data_endian,
             display_rotation);

    ui_preferences_load(&ui_preferences);
    ui_mutex = xSemaphoreCreateMutex();
    if (ui_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create UI mutex");
        return;
    }
    res = bsp_led_set_brightness(ui_preferences.led_brightness);
    if (res != ESP_OK)
        ESP_LOGW(TAG, "Failed to set LED brightness: %s", esp_err_to_name(res));

}

void ui_start(void)
{
    ui_services_started = true;
    /* Shared launcher icons are read-only; vector symbols remain the fallback. */
    ui_load_function_icons();

    /*
     * Prime the status cache before the initial render.
     * The initial screen is rendered unconditionally below.
     */
    ui_status_changed();
    ui_render();

    BaseType_t task_res = xTaskCreate(
        ui_status_task,
        "tanradio_ui_status",
        8192,
        NULL,
        2,
        &ui_status_task_handle);

    if (task_res != pdPASS) {
        ESP_LOGE(TAG, "Failed to create UI status refresh task");
        return;
    }

    ESP_LOGI(TAG, "UI status polling enabled: 1s; battery: 60s while a display is active");
    ESP_LOGI(TAG, "UI subsystem initialized");
}

/* Return true only for Space on the unobstructed welcome screen. */
static bool ui_welcome_event(input_keys_t *keys, const bsp_input_event_t *event, bool *redraw)
{
    bool space = false;
    if (event->type == INPUT_EVENT_TYPE_NAVIGATION &&
        input_keys_event(keys, event->args_navigation.key,
                         event->args_navigation.state, xTaskGetTickCount())) {
        bsp_input_navigation_key_t key = event->args_navigation.key;
        ui_exit_state_t previous = ui_exit_state;
        bool previous_about = ui_about_open;
        bool consumed = ui_handle_exit_navigation(key);
        *redraw |= previous != ui_exit_state || previous_about != ui_about_open ||
                   (consumed && previous == UI_EXIT_SETTINGS);
        if (!consumed) {
            space = key == BSP_INPUT_NAVIGATION_KEY_SPACE_L ||
                    key == BSP_INPUT_NAVIGATION_KEY_SPACE_M ||
                    key == BSP_INPUT_NAVIGATION_KEY_SPACE_R;
            if (key == BSP_INPUT_NAVIGATION_KEY_F6) {
                ui_toggle_display();
                *redraw = true;
            }
        }
    } else if (event->type == INPUT_EVENT_TYPE_KEYBOARD) {
        space = event->args_keyboard.ascii == ' ';
    } else if (event->type == INPUT_EVENT_TYPE_ACTION &&
               event->args_action.type == BSP_INPUT_ACTION_TYPE_POWER_BUTTON &&
               event->args_action.state) {
        ui_toggle_display();
        *redraw = true;
    }
    return space && ui_exit_state == UI_EXIT_IDLE && !ui_about_open;
}

bool ui_show_welcome(void)
{
    QueueHandle_t queue = NULL;
    if (ui_mutex == NULL ||
        bsp_input_get_queue(&queue) != ESP_OK || queue == NULL) {
        ESP_LOGE(TAG, "Welcome screen display or keyboard unavailable");
        return false;
    }

    /* Keep one composed background in PSRAM until Space or exit. Restoring it
     * avoids decoding the PNG and redrawing the welcome art for every key. */
    ui_draw_session_screen(false);
    const size_t frame_size = pax_buf_get_size(&framebuffer);
    void *background = heap_caps_malloc(frame_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (background) memcpy(background, pax_buf_get_pixels(&framebuffer), frame_size);
    bool redraw = !ui_blit();
    input_keys_t keys;
    input_keys_init(&keys, pdMS_TO_TICKS(500), pdMS_TO_TICKS(100));

    /* This loop owns input and rendering until the normal tasks start.
     * Preserve the order of key events and draw only their latest state. */
    while (true) {
        bool queue_drained = false;
        TickType_t wait = pdMS_TO_TICKS(50);
        for (unsigned i = 0; i < 32; i++) {
            bsp_input_event_t event;
            if (xQueueReceive(queue, &event, wait) != pdTRUE) {
                queue_drained = true;
                break;
            }
            wait = 0;
            if (ui_welcome_event(&keys, &event, &redraw)) {
                free(background);
                pax_background(&framebuffer, UI_COLOR_BACKGROUND);
                pax_draw_text(&framebuffer, UI_COLOR_TEXT, UI_FONT_HEADING,
                              UI_TEXT_TITLE, 280, 210, "Starting TanRadio...");
                ui_blit();
                return true;
            }
            if (ui_exit_state == UI_EXIT_COMMITTED) break;
        }

        /* Reconcile dropped releases only after queued press/release edges.
         * Hardware can be ahead of an older press still in the queue. */
        if (queue_drained && ui_exit_state != UI_EXIT_COMMITTED) {
            for (size_t i = 0; i < sizeof(keys.held) / sizeof(keys.held[0]); i++) {
                if (!keys.held[i]) continue;
                bool pressed = false;
                esp_err_t result = bsp_input_read_navigation_key((bsp_input_navigation_key_t)i, &pressed);
                if (result == ESP_OK && !pressed) {
                    input_keys_event(&keys, (bsp_input_navigation_key_t)i, false, xTaskGetTickCount());
                } else if (result != ESP_OK) {
                    /* A failed sample is not a release. */
                    input_keys_cancel_repeat(&keys);
                }
            }
        }

        if (redraw && ui_display_enabled) {
            bool exiting = ui_exit_state == UI_EXIT_COMMITTED;
            if (exiting) {
                free(background);
                background = NULL;
                ui_draw_session_screen(true);
            } else if (background) {
                memcpy(pax_buf_get_pixels_rw(&framebuffer), background, frame_size);
            } else {
                ui_draw_session_screen(false);
            }
            if (ui_exit_state == UI_EXIT_CONFIRM) ui_draw_exit_confirmation();
            else if (ui_exit_state == UI_EXIT_SETTINGS) ui_settings_draw(&framebuffer, &ui_settings, false);
            else if (ui_exit_state == UI_EXIT_IDLE && ui_about_open) ui_about_draw(&framebuffer);
            bool submitted = ui_blit();
            redraw = !submitted;
            if (exiting) ui_exit_presented = submitted;
            ui_complete_exit();
        }
    }
}

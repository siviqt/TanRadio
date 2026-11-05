#!/usr/bin/env python3
"""Exercise real exit input routing, welcome handling and shutdown gating."""
from pathlib import Path
import os
import subprocess
import tempfile

from test_controls import STUBS
from test_display_updates import function

ROOT = Path(__file__).resolve().parents[2]
HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <stdatomic.h>
#include "ui/ui.h"
#include "input/keys.h"
#include "ui/exit.h"
#include "ui/about.h"
#include "ui/settings.h"
#include "radio/artwork.h"
#define pdTRUE 1
#define pdFALSE 0
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
typedef uint32_t TickType_t;
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) (ms)
#define UI_EXIT_SPLASH_MS 1000
#define TAG "exit-test"
#define UI_COLOR_BACKGROUND 0
#define UI_COLOR_TEXT 1
#define UI_FONT_HEADING NULL
#define UI_TEXT_TITLE 32
static void *ui_mutex = (void *)1;
static ui_exit_state_t ui_exit_state;
static ui_settings_t ui_settings;
static ui_preferences_t ui_preferences;
static unsigned ui_settings_revision, settings_frames, display_stops, logo_cancels;
static bool fail_settings_save, fail_led_update;
static unsigned led_updates;
static uint8_t led_brightness;
static atomic_int ui_services_state = UI_SERVICES_READY;
static unsigned audio_shutdowns, early_stops;
static void audio_shutdown(void) { audio_shutdowns++; }
static bool ui_display_enabled, ui_exit_presented, ui_exit_started, ui_services_started, ui_about_open;
static unsigned locks, redraws, toggles, routed, volumes, exits, restarts, delays;
static unsigned welcome_draws, splash_frames, confirm_frames, blits;
static pax_buf_t framebuffer;
static bool submit_ok = true, fail_next_splash, read_failed, no_cache;
static jmp_buf restarted;
static int xSemaphoreTake(void *m, unsigned wait) {
    (void)wait; assert(m == ui_mutex && locks++ == 0); return pdTRUE;
}
static void xSemaphoreGive(void *m) { assert(m == ui_mutex && locks-- == 1); }
static void ui_draw(void) { assert(!locks); redraws++; }
void ui_toggle_display(void) {
    assert(!locks); toggles++; ui_display_enabled = !ui_display_enabled;
}
void ui_handle_navigation(bsp_input_navigation_key_t key, bool pressed) {
    (void)key; assert(pressed && !locks); routed++;
}
static int audio_volume_up(void) { volumes++; return 0; }
static int audio_volume_down(void) { volumes++; return 0; }
static void vTaskDelay(unsigned ticks) { assert(!locks); delays += ticks; }
static void radio_stop_for_exit(void) {
    assert(!locks && ui_exit_state == UI_EXIT_CONFIRM && !ui_exit_presented && !delays);
    early_stops++;
}
static void radio_request_exit(void) {
    assert(!locks && ui_exit_presented && delays >= UI_EXIT_SPLASH_MS); exits++;
}
static void bsp_device_restart_to_launcher(void) {
    assert(!locks && ui_exit_presented && delays >= UI_EXIT_SPLASH_MS);
    restarts++; longjmp(restarted, 1);
}
static void ui_draw_session_screen(bool exiting) {
    if (exiting) {
        splash_frames++;
        if (fail_next_splash) { submit_ok = false; fail_next_splash = false; }
    } else {
        welcome_draws++;
        framebuffer = 1;
    }
}
static size_t pax_buf_get_size(pax_buf_t *buffer) { return sizeof(*buffer); }
static const void *pax_buf_get_pixels(pax_buf_t *buffer) { return buffer; }
static void *pax_buf_get_pixels_rw(pax_buf_t *buffer) { return buffer; }
static void *heap_caps_malloc(size_t size, unsigned caps) {
    assert(size == sizeof(framebuffer) && caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    return no_cache ? NULL : malloc(size);
}
static void ui_draw_exit_confirmation(void) { assert(framebuffer == 1); framebuffer = 2; confirm_frames++; }
static void ui_about_draw(void *buffer) { (void)buffer; }
void ui_settings_draw(pax_buf_t *buffer, const ui_settings_t *settings, bool started) {
    assert(*buffer == 1 && settings == &ui_settings && !started); *buffer = 3; settings_frames++;
}
bool ui_preferences_save(const ui_preferences_t *preferences) {
    assert(preferences && !locks); return !fail_settings_save;
}
esp_err_t bsp_led_set_brightness(uint8_t percentage) {
    assert(!locks && !fail_settings_save);
    led_updates++;
    if (fail_led_update) return ESP_FAIL;
    led_brightness = percentage;
    return ESP_OK;
}
void external_display_stop(void) { assert(!locks); display_stops++; }
unsigned artwork_submit(const radio_station_t *station, bool online) {
    assert(!station && !online && !locks); logo_cancels++; return 0;
}
static bool ui_blit(void) { blits++; bool ok = submit_ok; submit_ok = true; return ok; }
static void pax_background(void *buf, int color) { (void)buf; (void)color; }
static void pax_draw_text(void *buf, int color, void *font, int size, int x, int y, const char *text) {
    (void)buf; (void)color; (void)font; (void)size; (void)x; (void)y; (void)text;
}
static bsp_input_event_t events[32];
static unsigned event_count, event_index, burst_from, burst_until;
static bool hardware_down[BSP_INPUT_NAVIGATION_KEY_VOLUME_DOWN + 1];
static unsigned xTaskGetTickCount(void) { return event_index * 100; }
esp_err_t bsp_input_get_queue(QueueHandle_t *queue) { *queue = (void *)2; return ESP_OK; }
static int xQueueReceive(QueueHandle_t queue, bsp_input_event_t *out, unsigned wait) {
    assert(queue && (wait == 0 || wait == 50));
    if (!wait && (event_index < burst_from || event_index >= burst_until)) return pdFALSE;
    assert(event_index < event_count);
    *out = events[event_index++];
    if (out->type == INPUT_EVENT_TYPE_NAVIGATION)
        hardware_down[out->args_navigation.key] = out->args_navigation.state;
    return pdTRUE;
}
esp_err_t bsp_input_read_navigation_key(bsp_input_navigation_key_t key, bool *pressed) {
    assert(event_index < burst_from || event_index >= burst_until);
    *pressed = hardware_down[key]; return read_failed ? ESP_FAIL : ESP_OK;
}
'''
TEST = r'''
static void reset(void) {
    assert(!locks); ui_exit_state = UI_EXIT_IDLE; ui_display_enabled = true;
    ui_about_open = false;
    atomic_store(&ui_services_state, UI_SERVICES_READY); audio_shutdowns = early_stops = 0;
    ui_exit_presented = ui_exit_started = ui_services_started = false;
    redraws = toggles = routed = volumes = exits = restarts = delays = 0;
    welcome_draws = splash_frames = confirm_frames = blits = event_count = event_index = 0;
    burst_from = burst_until = 0;
    ui_preferences = (ui_preferences_t){.contrast = 60, .led_brightness = UI_LED_BRIGHTNESS_DEFAULT};
    ui_settings_revision = settings_frames = display_stops = logo_cancels = 0;
    fail_settings_save = fail_led_update = false;
    led_updates = 0;
    led_brightness = UI_LED_BRIGHTNESS_DEFAULT;
    submit_ok = true; fail_next_splash = read_failed = no_cache = false;
    for (unsigned i = 0; i < sizeof(hardware_down)/sizeof(*hardware_down); i++) hardware_down[i] = false;
}
static void nav(bsp_input_navigation_key_t key, bool state) {
    events[event_count++] = (bsp_input_event_t){.type = INPUT_EVENT_TYPE_NAVIGATION,
        .args_navigation = {.key = key, .state = state}};
}
static void space(void) {
    events[event_count++] = (bsp_input_event_t){.type = INPUT_EVENT_TYPE_KEYBOARD,
        .args_keyboard = {.ascii = ' '}};
}
int main(void) {
    reset(); input_keys_t keys; bsp_input_navigation_key_t repeat;
    input_keys_init(&keys, 500, 100);
    /* A held arrow repeat is canceled by F1; held F1 never confirms. */
    assert(input_keys_event(&keys, BSP_INPUT_NAVIGATION_KEY_DOWN, true, 0));
    ui_display_enabled = false;
    assert(input_keys_event(&keys, BSP_INPUT_NAVIGATION_KEY_F1, true, 10));
    input_dispatch_navigation(BSP_INPUT_NAVIGATION_KEY_F1);
    assert(ui_exit_state == UI_EXIT_CONFIRM && toggles == 1 && ui_display_enabled);
    assert(!input_keys_event(&keys, BSP_INPUT_NAVIGATION_KEY_F1, true, 10000));
    assert(!input_keys_due(&keys, 20000, &repeat) && !exits);
    input_dispatch_navigation(BSP_INPUT_NAVIGATION_KEY_RETURN);
    input_dispatch_navigation(BSP_INPUT_NAVIGATION_KEY_F4);
    input_dispatch_navigation(BSP_INPUT_NAVIGATION_KEY_BACKSPACE);
    assert(!routed && ui_exit_state == UI_EXIT_CONFIRM);
    input_dispatch_navigation(BSP_INPUT_NAVIGATION_KEY_VOLUME_UP);
    assert(volumes == 1);
    input_dispatch_navigation(BSP_INPUT_NAVIGATION_KEY_ESC);
    assert(ui_exit_state == UI_EXIT_IDLE);
    input_dispatch_navigation(BSP_INPUT_NAVIGATION_KEY_F4); assert(routed == 1);
    input_dispatch_navigation(BSP_INPUT_NAVIGATION_KEY_F1);
    input_dispatch_navigation(BSP_INPUT_NAVIGATION_KEY_F2);
    assert(ui_exit_state == UI_EXIT_IDLE);

    /* F3 opens Settings only from Options. Edits are a draft until F4. */
    input_dispatch_navigation(BSP_INPUT_NAVIGATION_KEY_F1);
    input_dispatch_navigation(BSP_INPUT_NAVIGATION_KEY_F3);
    assert(ui_exit_state == UI_EXIT_SETTINGS && routed == 1);
    input_dispatch_navigation(BSP_INPUT_NAVIGATION_KEY_UP);
    assert(ui_settings.row == UI_SETTING_LED_BRIGHTNESS && ui_settings.draft.led_brightness == 50);
    input_dispatch_navigation(BSP_INPUT_NAVIGATION_KEY_RIGHT);
    assert(ui_settings.draft.led_brightness == 55 && ui_preferences.led_brightness == 50 && !led_updates);
    input_dispatch_navigation(BSP_INPUT_NAVIGATION_KEY_DOWN);
    input_dispatch_navigation(BSP_INPUT_NAVIGATION_KEY_RIGHT);
    assert(ui_settings.draft.display == EXTERNAL_DISPLAY_NOKIA5110 && ui_preferences.display == 0);
    input_dispatch_navigation(BSP_INPUT_NAVIGATION_KEY_DOWN);
    input_dispatch_navigation(BSP_INPUT_NAVIGATION_KEY_RIGHT);
    fail_settings_save = true;
    input_dispatch_navigation(BSP_INPUT_NAVIGATION_KEY_F4);
    assert(!ui_settings_revision && ui_preferences.contrast == 60 && !led_updates);
    fail_settings_save = false;
    input_dispatch_navigation(BSP_INPUT_NAVIGATION_KEY_F4);
    assert(ui_settings_revision == 1 && ui_preferences.contrast == 61);
    assert(ui_preferences.led_brightness == 55 && led_brightness == 55 && led_updates == 1);
    input_dispatch_navigation(BSP_INPUT_NAVIGATION_KEY_RIGHT);
    input_dispatch_navigation(BSP_INPUT_NAVIGATION_KEY_F1);
    assert(ui_exit_state == UI_EXIT_SETTINGS && !exits);
    input_dispatch_navigation(BSP_INPUT_NAVIGATION_KEY_F2);
    assert(ui_exit_state == UI_EXIT_CONFIRM && ui_preferences.contrast == 61);
    input_dispatch_navigation(BSP_INPUT_NAVIGATION_KEY_F2);
    assert(ui_exit_state == UI_EXIT_IDLE);

    /* Only a released and re-pressed F1 can reach the committed state. */
    ui_services_started = true;
    input_keys_event(&keys, BSP_INPUT_NAVIGATION_KEY_F1, false, 21000);
    assert(input_keys_event(&keys, BSP_INPUT_NAVIGATION_KEY_F1, true, 21001));
    input_dispatch_navigation(BSP_INPUT_NAVIGATION_KEY_F1);
    assert(!early_stops);
    input_keys_event(&keys, BSP_INPUT_NAVIGATION_KEY_F1, false, 21002);
    assert(input_keys_event(&keys, BSP_INPUT_NAVIGATION_KEY_F1, true, 21003));
    input_dispatch_navigation(BSP_INPUT_NAVIGATION_KEY_F1);
    assert(ui_exit_state == UI_EXIT_COMMITTED && early_stops == 1 && !exits);
    input_dispatch_navigation(BSP_INPUT_NAVIGATION_KEY_F2);
    input_dispatch_navigation(BSP_INPUT_NAVIGATION_KEY_VOLUME_DOWN);
    assert(ui_exit_state == UI_EXIT_COMMITTED && volumes == 1);
    ui_complete_exit(); assert(!delays && !exits && early_stops == 1); /* Splash retry stays silent. */
    ui_exit_presented = ui_services_started = true;
    ui_complete_exit(); assert(exits == 1 && delays == UI_EXIT_SPLASH_MS);
    assert(display_stops == 1 && logo_cancels == 1);
    ui_complete_exit(); assert(exits == 1); /* Shutdown is requested once. */

    /* Exit during background radio initialization does not depend on its worker. */
    reset(); ui_services_started = ui_exit_presented = true;
    atomic_store(&ui_services_state, UI_SERVICES_STARTING);
    if (!setjmp(restarted)) { ui_complete_exit(); assert(false); }
    assert(restarts == 1 && !exits && audio_shutdowns == 1);

    /* Welcome Space cannot bypass confirmation; cancellation still starts. */
    reset(); read_failed = true; nav(BSP_INPUT_NAVIGATION_KEY_F1, true); space();
    nav(BSP_INPUT_NAVIGATION_KEY_F1, true); /* Duplicate, still held. */
    nav(BSP_INPUT_NAVIGATION_KEY_F2, true); space();
    assert(ui_show_welcome() && event_index == event_count);
    assert(confirm_frames == 1 && !splash_frames && !restarts);

    reset();
    nav(BSP_INPUT_NAVIGATION_KEY_F1, true); nav(BSP_INPUT_NAVIGATION_KEY_F3, true);
    space(); nav(BSP_INPUT_NAVIGATION_KEY_RIGHT, true);
    nav(BSP_INPUT_NAVIGATION_KEY_F4, true); nav(BSP_INPUT_NAVIGATION_KEY_F2, true);
    nav(BSP_INPUT_NAVIGATION_KEY_F2, false); nav(BSP_INPUT_NAVIGATION_KEY_F2, true);
    space();
    assert(ui_show_welcome() && event_index == event_count && settings_frames > 0);
    assert(ui_preferences.display == EXTERNAL_DISPLAY_NOKIA5110 && ui_settings_revision == 1);

    /* A burst edits the draft in order and redraws the final settings once.
     * A Space queued while the dialog is open cannot start the app. */
    reset();
    nav(BSP_INPUT_NAVIGATION_KEY_F1, true);
    nav(BSP_INPUT_NAVIGATION_KEY_F3, true);
    space(); burst_from = event_count;
    nav(BSP_INPUT_NAVIGATION_KEY_RIGHT, true); nav(BSP_INPUT_NAVIGATION_KEY_RIGHT, false);
    nav(BSP_INPUT_NAVIGATION_KEY_DOWN, true); nav(BSP_INPUT_NAVIGATION_KEY_DOWN, false);
    nav(BSP_INPUT_NAVIGATION_KEY_RIGHT, true); nav(BSP_INPUT_NAVIGATION_KEY_RIGHT, true);
    nav(BSP_INPUT_NAVIGATION_KEY_RIGHT, false);
    nav(BSP_INPUT_NAVIGATION_KEY_RIGHT, true); nav(BSP_INPUT_NAVIGATION_KEY_RIGHT, false);
    nav(BSP_INPUT_NAVIGATION_KEY_F4, true); nav(BSP_INPUT_NAVIGATION_KEY_F4, false);
    burst_until = event_count;
    nav(BSP_INPUT_NAVIGATION_KEY_F2, true); nav(BSP_INPUT_NAVIGATION_KEY_F2, false);
    nav(BSP_INPUT_NAVIGATION_KEY_F2, true); space();
    assert(ui_show_welcome() && event_index == event_count);
    assert(welcome_draws == 1 && settings_frames == 2 && confirm_frames == 2);
    assert(ui_settings_revision == 1 && ui_preferences.contrast == 62);

    reset(); no_cache = true;
    nav(BSP_INPUT_NAVIGATION_KEY_F1, true); nav(BSP_INPUT_NAVIGATION_KEY_F2, true); space();
    assert(ui_show_welcome() && welcome_draws == 3 && confirm_frames == 1);

    /* Startup exit bypasses uninitialized radio/audio, retries a failed splash. */
    reset(); nav(BSP_INPUT_NAVIGATION_KEY_F1, true);
    nav(BSP_INPUT_NAVIGATION_KEY_F1, true); space();
    nav(BSP_INPUT_NAVIGATION_KEY_F1, false); nav(BSP_INPUT_NAVIGATION_KEY_F1, true);
    events[event_count++] = (bsp_input_event_t){0};
    fail_next_splash = true;
    if (!setjmp(restarted)) { ui_show_welcome(); assert(false); }
    assert(restarts == 1 && !exits && splash_frames == 2 && !locks && !early_stops);
    assert(delays == UI_EXIT_SPLASH_MS && event_index == event_count);

    /* Brightness can reach either endpoint. An I/O failure can be retried. */
    reset();
    input_dispatch_navigation(BSP_INPUT_NAVIGATION_KEY_F1);
    input_dispatch_navigation(BSP_INPUT_NAVIGATION_KEY_F3);
    input_dispatch_navigation(BSP_INPUT_NAVIGATION_KEY_UP);
    for (unsigned i = 0; i < 11; i++) input_dispatch_navigation(BSP_INPUT_NAVIGATION_KEY_LEFT);
    assert(ui_settings.draft.led_brightness == 0);
    input_dispatch_navigation(BSP_INPUT_NAVIGATION_KEY_RETURN);
    assert(ui_settings.draft.led_brightness == 0);
    fail_led_update = true;
    input_dispatch_navigation(BSP_INPUT_NAVIGATION_KEY_F4);
    assert(ui_preferences.led_brightness == 0 && led_brightness == 50 && led_updates == 1);
    assert(strstr(ui_settings.notice, "LED update failed"));
    fail_led_update = false;
    input_dispatch_navigation(BSP_INPUT_NAVIGATION_KEY_F4);
    assert(led_brightness == 0 && led_updates == 2);
    for (unsigned i = 0; i < 21; i++) input_dispatch_navigation(BSP_INPUT_NAVIGATION_KEY_RIGHT);
    assert(ui_settings.draft.led_brightness == 100);
    input_dispatch_navigation(BSP_INPUT_NAVIGATION_KEY_F2);
    assert(led_brightness == 0 && led_updates == 2);
    input_dispatch_navigation(BSP_INPUT_NAVIGATION_KEY_F2);

    puts("PASS: two distinct F1 presses, holds, modal routing, settings apply/cancel, LED brightness bounds and retries, wake, cached welcome and batched input, immediate audio stop, splash retry and restart ordering");
}
'''


def main():
    source = HARNESS
    source += function(ROOT / 'main/ui/settings.c', 'bool ui_settings_edit(')
    source += function(ROOT / 'main/ui/ui.c', 'bool ui_handle_exit_navigation(')
    source += function(ROOT / 'main/input/input.c', 'static void input_dispatch_navigation(')
    source += function(ROOT / 'main/ui/ui.c', 'static void ui_complete_exit(')
    source += function(ROOT / 'main/ui/ui.c', 'static bool ui_welcome_event(')
    source += function(ROOT / 'main/ui/ui.c', 'bool ui_show_welcome(') + TEST
    with tempfile.TemporaryDirectory(prefix='tanradio-exit-') as directory:
        work = Path(directory)
        for name in ['esp_err.h', 'freertos/FreeRTOS.h', 'freertos/queue.h']:
            path = work / name
            path.parent.mkdir(exist_ok=True)
            path.write_text(STUBS)
        (work / "pax_gfx.h").write_text("typedef int pax_buf_t;\n")
        (work / 'check.c').write_text(source)
        subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-I', str(work),
                        '-I', str(ROOT / 'main'), '-I', str(ROOT / 'local-components/badgeteam__badge-bsp'),
                        str(work / 'check.c'), str(ROOT / 'main/ui/exit.c'),
                        str(ROOT / 'main/ui/about.c'),
                        str(ROOT / 'main/input/keys.c'), '-o', str(work / 'check')], check=True)
        subprocess.run([str(work / 'check')], check=True,
                       env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))


if __name__ == '__main__':
    main()

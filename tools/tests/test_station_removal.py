#!/usr/bin/env python3
"""Run production UI handlers, catalog and index rebasing with host I/O mocks."""
from pathlib import Path
import os
import subprocess
import tempfile
from test_controls import STUBS
from test_display_updates import function

ROOT = Path(__file__).resolve().parents[2]
HEADER = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ui/ui.h"
#include "ui/controls.h"
#include "ui/exit.h"
#include <stdatomic.h>
static atomic_int ui_services_state = UI_SERVICES_READY;
static ui_exit_state_t ui_exit_state;
#include "radio/browser.h"
#include "input/keys.h"
#define portMAX_DELAY UINT32_MAX
#define ESP_LOGD(tag, ...) ((void)(tag), (void)sizeof(printf(__VA_ARGS__)))
static const char *TAG = "removal-test";
static void *ui_mutex = (void *)1, *radio_station_mutex = (void *)2;
static bool ui_locked, radio_locked, ui_search_open, ui_about_open;
static ui_station_list_t ui_station_list;
static char ui_search_query[BROWSER_QUERY_LENGTH + 1], ui_search_notice[96];
static size_t ui_search_selection;
static radio_browser_search_mode_t ui_search_mode, last_search_mode;
static unsigned cancellations;
static unsigned notifications, plays, toggles, stops, lights, searches;
static bool fail_remove;
static radio_playback_status_t playback;
static radio_station_t radio_requested_station, radio_current_station, result;
static size_t radio_selected_station, radio_playing_station;
static int xSemaphoreTake(void *sem, uint32_t timeout) {
    assert(timeout == portMAX_DELAY);
    bool *lock = sem == ui_mutex ? &ui_locked : &radio_locked;
    assert(!*lock); *lock = true; return 1;
}
static void xSemaphoreGive(void *sem) {
    bool *lock = sem == ui_mutex ? &ui_locked : &radio_locked;
    assert(*lock); *lock = false;
}
static void ui_draw(void) { notifications++; }
void ui_toggle_display(void) { lights++; }
void radio_get_playback_status(radio_playback_status_t *out) { *out = playback; }
bool radio_is_muted(void) { return playback.muted; }
bool radio_toggle_mute(void) { toggles++; return playback.muted = !playback.muted; }
bool radio_play_record(const radio_station_t *station) {
    plays++; playback.station = *station; playback.stopped = false;
    radio_requested_station = radio_current_station = *station;
    return true;
}
bool radio_stop(void) { stops++; playback.stopped = true; return true; }
void radio_browser_cancel(void) { cancellations++; }
bool radio_browser_search_by(const char *query, radio_browser_search_mode_t mode) {
    assert(query[0]); last_search_mode = mode; searches++; return true;
}
void radio_browser_status(radio_browser_status_t *out) {
    *out = (radio_browser_status_t){.count = 1};
}
bool radio_browser_result(size_t index, radio_station_t *out) {
    if (index) return false;
    *out = result; return true;
}
esp_err_t __real_stations_remove(const char *, size_t);
esp_err_t __wrap_stations_remove(const char *path, size_t index) {
    assert(ui_locked && !strcmp(path, STATIONS_FILE_PATH));
    return fail_remove ? ESP_FAIL : __real_stations_remove("stations.txt", index);
}
esp_err_t __real_stations_save(const char *, const radio_station_t *, size_t *, bool *);
esp_err_t __wrap_stations_save(const char *path, const radio_station_t *station,
                               size_t *index, bool *duplicate) {
    (void)path; return __real_stations_save("stations.txt", station, index, duplicate);
}
'''

TEST = r'''
static void nav(bsp_input_navigation_key_t key) {
    ui_handle_navigation(key, true);
    assert(!ui_locked && !radio_locked);
}
static void confirm(void) { nav(BSP_INPUT_NAVIGATION_KEY_F4); }
int main(void) {
    FILE *f = fopen("stations.txt", "wb"); assert(f);
    fputs("First:::http://example.org/first:::BG:::MP3\n"
          "Active:::http://example.org/active:::BG:::MP3\n"
          "Last:::http://example.org/last:::BG:::MP3\n", f); fclose(f);
    assert(stations_load_file("stations.txt") == ESP_OK);
    playback = (radio_playback_status_t){.station = *stations_get(1), .muted = true, .buffering = true};
    radio_requested_station = radio_current_station = playback.station;
    radio_selected_station = radio_playing_station = 1;
    radio_station_t original = playback.station;

    nav(BSP_INPUT_NAVIGATION_KEY_BACKSPACE);
    assert(ui_remove_state.open && !strcmp(ui_remove_state.station.name, "First"));
    assert(stations_count() == 3);
    /* Modal selection cannot drift, Enter cannot confirm and F5 cannot play. */
    nav(BSP_INPUT_NAVIGATION_KEY_DOWN); nav(BSP_INPUT_NAVIGATION_KEY_RETURN);
    nav(BSP_INPUT_NAVIGATION_KEY_F5);
    for (unsigned i = 0; i < 10; i++) nav(BSP_INPUT_NAVIGATION_KEY_BACKSPACE);
    assert(ui_remove_state.index == 0 && stations_count() == 3 && !plays);
    nav(BSP_INPUT_NAVIGATION_KEY_F2);
    assert(!ui_remove_state.open && !ui_search_open && stations_count() == 3);
    /* A release never opens the prompt. */
    ui_handle_navigation(BSP_INPUT_NAVIGATION_KEY_BACKSPACE, false);
    assert(!ui_remove_state.open);
    nav(BSP_INPUT_NAVIGATION_KEY_BACKSPACE); nav(BSP_INPUT_NAVIGATION_KEY_ESC);
    assert(!ui_remove_state.open);

    nav(BSP_INPUT_NAVIGATION_KEY_BACKSPACE);
    fail_remove = true; confirm();
    assert(ui_remove_state.open && ui_remove_state.failed && stations_count() == 3);
    assert(!plays && !toggles && !stops);
    fail_remove = false; confirm();
    assert(!ui_remove_state.open && stations_count() == 2);
    assert(radio_selected_station == 0 && radio_playing_station == 0);
    assert(!strcmp(stations_get(0)->name, "Active"));

    /* Active removal becomes a selectable preview without changing audio. */
    nav(BSP_INPUT_NAVIGATION_KEY_BACKSPACE);
    input_keys_t keys; input_keys_init(&keys, 500, 100);
    assert(input_keys_event(&keys, BSP_INPUT_NAVIGATION_KEY_F4, true, 0));
    confirm();
    assert(!input_keys_event(&keys, BSP_INPUT_NAVIGATION_KEY_F4, true, 1));
    assert(stations_count() == 1 && ui_station_list.has_preview);
    assert(ui_station_list.selection == SIZE_MAX && !ui_remove_state.open);
    assert(radio_selected_station == SIZE_MAX && radio_playing_station == SIZE_MAX);
    assert(!plays && !toggles && !stops && playback.muted && playback.buffering);
    assert(!memcmp(&playback.station, &original, sizeof(original)));
    nav(BSP_INPUT_NAVIGATION_KEY_BACKSPACE); /* Preview is not a saved row. */
    assert(!ui_remove_state.open && stations_count() == 1);

    /* Search Backspace remains text editing, including UTF-8 boundaries. */
    atomic_store(&ui_services_state, UI_SERVICES_STARTING);
    nav(BSP_INPUT_NAVIGATION_KEY_F2); assert(!ui_search_open);
    ui_handle_keyboard('Z', "Z", 0); assert(!ui_search_query[0]);
    atomic_store(&ui_services_state, UI_SERVICES_READY);
    nav(BSP_INPUT_NAVIGATION_KEY_F2); assert(ui_search_open);
    strcpy(ui_search_query, "Xé"); nav(BSP_INPUT_NAVIGATION_KEY_BACKSPACE);
    assert(!strcmp(ui_search_query, "X") && !ui_remove_state.open);
    ui_handle_keyboard('\b', "\b", 0); assert(!strcmp(ui_search_query, "X"));
    ui_exit_state = UI_EXIT_CONFIRM;
    ui_handle_keyboard('Z', "Z", 0); assert(!strcmp(ui_search_query, "X"));
    ui_exit_state = UI_EXIT_IDLE;
    ui_about_open = true;
    ui_handle_keyboard('Z', "Z", 0); assert(!strcmp(ui_search_query, "X"));
    ui_about_open = false;
    ui_handle_keyboard(' ', " ", 0); assert(!strcmp(ui_search_query, "X "));
    nav(BSP_INPUT_NAVIGATION_KEY_RETURN); assert(searches == 1);
    assert(last_search_mode == BROWSER_SEARCH_NAME);
    unsigned before_switch = cancellations;
    ui_search_selection = 4;
    nav(BSP_INPUT_NAVIGATION_KEY_TAB);
    assert(ui_search_mode == BROWSER_SEARCH_COUNTRY && !ui_search_query[0]);
    assert(!ui_search_selection && cancellations == before_switch + 1);
    nav(BSP_INPUT_NAVIGATION_KEY_RETURN);
    assert(searches == 1 && strstr(ui_search_notice, "country"));
    ui_handle_keyboard('b', "bg", 0);
    nav(BSP_INPUT_NAVIGATION_KEY_RETURN);
    assert(searches == 2 && last_search_mode == BROWSER_SEARCH_COUNTRY);
    nav(BSP_INPUT_NAVIGATION_KEY_TAB);
    assert(ui_search_mode == BROWSER_SEARCH_NAME && !ui_search_query[0]);
    nav(BSP_INPUT_NAVIGATION_KEY_F2); assert(!ui_search_open);

    /* Remove the last saved row while the unsaved active preview survives. */
    ui_station_list.selection = 0;
    nav(BSP_INPUT_NAVIGATION_KEY_BACKSPACE); confirm();
    assert(stations_count() == 0 && ui_station_list.selection == SIZE_MAX);
    assert(ui_station_list.has_preview && !plays && !toggles && !stops);
    assert(stations_load_file("stations.txt") == ESP_ERR_NOT_FOUND);
    /* The removed active station can be saved again from search. */
    result = original; nav(BSP_INPUT_NAVIGATION_KEY_F2); nav(BSP_INPUT_NAVIGATION_KEY_F5);
    assert(stations_count() == 1 && !ui_station_list.has_preview);
    nav(BSP_INPUT_NAVIGATION_KEY_F2);
    assert(ui_station_list.selection == 0 && stations_same(stations_get(0), &original));
    assert(!plays && !toggles && !stops);
    nav(BSP_INPUT_NAVIGATION_KEY_BACKSPACE);
    nav(BSP_INPUT_NAVIGATION_KEY_F6); assert(lights == 1 && ui_remove_state.open);
    nav(BSP_INPUT_NAVIGATION_KEY_F3); assert(stops == 1 && ui_remove_state.open);
    nav(BSP_INPUT_NAVIGATION_KEY_F2);
    /* Empty, stopped state also ignores Backspace safely. */
    assert(__real_stations_remove("stations.txt", 0) == ESP_OK);
    playback.station = (radio_station_t){0};
    nav(BSP_INPUT_NAVIGATION_KEY_BACKSPACE);
    assert(!ui_remove_state.open && !ui_station_list.has_preview);
    puts("PASS: removal confirmation/cancel/retry, modal controls, saved/preview/empty rows, owned playback, index rebasing, search Backspace and resaving");
}
'''


def main():
    ui = ROOT / "main/ui/ui.c"
    text = ui.read_text()
    state_end = text.index("} ui_remove_state;") + len("} ui_remove_state;")
    state_start = text.rindex("static struct {", 0, state_end)
    code = HEADER + text[state_start:state_end]
    code += function(ROOT / "main/radio/radio.c", "void radio_catalog_changed(void)")
    for name in ["static void ui_refresh_preview(void)", "static bool ui_main_selected_station(",
                 "static void ui_focus_current_station(void)", "static bool ui_remove_navigation(",
                 "static bool ui_search_navigation(", "void ui_handle_navigation(",
                 "void ui_handle_keyboard("]:
        code += function(ui, name)
    with tempfile.TemporaryDirectory(prefix="tanradio-removal-") as tmp:
        work = Path(tmp)
        for name in ["esp_err.h", "esp_log.h", "esp_heap_caps.h", "freertos/FreeRTOS.h", "freertos/queue.h"]:
            path = work / name
            path.parent.mkdir(exist_ok=True)
            path.write_text(STUBS)
        (work / "check.c").write_text(code + TEST)
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-Wno-sign-compare", "-g", "-fsanitize=address,undefined",
                        "-I" + str(work), "-I" + str(ROOT / "main"),
                        "-I" + str(ROOT / "local-components/badgeteam__badge-bsp"),
                        str(work / "check.c"), str(ROOT / "main/ui/controls.c"),
                        str(ROOT / "main/input/keys.c"), str(ROOT / "main/radio/stations.c"),
                        "-Wl,--wrap=stations_remove", "-Wl,--wrap=stations_save",
                        "-o", str(work / "check")], check=True)
        subprocess.run([str(work / "check")], check=True, cwd=work,
                       env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))


if __name__ == "__main__":
    main()

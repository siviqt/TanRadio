#!/usr/bin/env python3
"""Exercise production playback decisions and held-key handling on the host."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
STUBS = r'''
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_INVALID_ARG 2
#define ESP_ERR_INVALID_STATE 3
#define ESP_ERR_NOT_FOUND 4
#define ESP_ERR_INVALID_SIZE 5
#define ESP_LOGE(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGI(tag, ...) ((void)(tag))
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define heap_caps_calloc(n, s, caps) calloc(n, s)
typedef void *QueueHandle_t;
typedef void *TaskHandle_t;
typedef uint32_t TickType_t;
'''

HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ui/controls.h"
#include "input/keys.h"

static radio_playback_status_t playback;
static unsigned plays, toggles;
void radio_get_playback_status(radio_playback_status_t *out) { *out = playback; }
bool radio_is_muted(void) { return playback.muted; }
bool radio_toggle_mute(void) { toggles++; return playback.muted = !playback.muted; }
bool radio_play_record(const radio_station_t *station) {
    plays++; playback.station = *station; playback.stopped = false;
    playback.buffering = true; return true;
}

static void check_keys(void) {
    input_keys_t keys; bsp_input_navigation_key_t repeated;
    input_keys_init(&keys, 500, 100);
    assert(input_keys_wait(&keys, 0) == UINT32_MAX);
    assert(!input_keys_due(&keys, 1000, &repeated));
    assert(input_keys_event(&keys, BSP_INPUT_NAVIGATION_KEY_UP, true, 0));
    assert(!input_keys_event(&keys, BSP_INPUT_NAVIGATION_KEY_UP, true, 400));
    assert(!input_keys_due(&keys, 499, &repeated));
    assert(input_keys_due(&keys, 500, &repeated) && repeated == BSP_INPUT_NAVIGATION_KEY_UP);
    assert(!input_keys_due(&keys, 599, &repeated));
    assert(input_keys_due(&keys, 600, &repeated));
    assert(!input_keys_event(&keys, BSP_INPUT_NAVIGATION_KEY_UP, false, 601));
    assert(!input_keys_due(&keys, 2000, &repeated));

    const bsp_input_navigation_key_t repeatable[] = {BSP_INPUT_NAVIGATION_KEY_UP,
        BSP_INPUT_NAVIGATION_KEY_DOWN, BSP_INPUT_NAVIGATION_KEY_BACKSPACE,
        BSP_INPUT_NAVIGATION_KEY_VOLUME_UP, BSP_INPUT_NAVIGATION_KEY_VOLUME_DOWN};
    for (unsigned i = 0; i < sizeof(repeatable) / sizeof(*repeatable); i++) {
        input_keys_init(&keys, 500, 100);
        assert(input_keys_event(&keys, repeatable[i], true, 100));
        assert(input_keys_due(&keys, 600, &repeated) && repeated == repeatable[i]);
        assert(input_keys_due(&keys, 20000, &repeated));
        assert(!input_keys_due(&keys, 20000, &repeated)); /* No catch-up bursts. */
        input_keys_cancel_repeat(&keys);
        assert(!input_keys_due(&keys, 30000, &repeated));
    }
    /* Function keys, Enter and Esc must never repeat or dispatch on release. */
    const bsp_input_navigation_key_t commands[] = {BSP_INPUT_NAVIGATION_KEY_F1,
        BSP_INPUT_NAVIGATION_KEY_F2, BSP_INPUT_NAVIGATION_KEY_F3, BSP_INPUT_NAVIGATION_KEY_F4,
        BSP_INPUT_NAVIGATION_KEY_F5, BSP_INPUT_NAVIGATION_KEY_F6,
        BSP_INPUT_NAVIGATION_KEY_RETURN, BSP_INPUT_NAVIGATION_KEY_ESC,
        BSP_INPUT_NAVIGATION_KEY_SUPER};
    for (unsigned i = 0; i < sizeof(commands) / sizeof(*commands); i++) {
        input_keys_init(&keys, 500, 100);
        assert(input_keys_event(&keys, commands[i], true, 1));
        assert(!input_keys_event(&keys, commands[i], true, 2));
        assert(!input_keys_due(&keys, 50000, &repeated));
        assert(!input_keys_event(&keys, commands[i], false, 50001));
        assert(input_keys_event(&keys, commands[i], true, 50002));
    }
    /* Opening/closing search must not carry an old arrow repeat into it. */
    input_keys_init(&keys, 500, 100);
    assert(input_keys_event(&keys, BSP_INPUT_NAVIGATION_KEY_UP, true, 10));
    assert(input_keys_event(&keys, BSP_INPUT_NAVIGATION_KEY_F2, true, 20));
    assert(!input_keys_due(&keys, 1000, &repeated));
    assert(!input_keys_event(&keys, BSP_INPUT_NAVIGATION_KEY_UP, true, 1001));
    input_keys_event(&keys, BSP_INPUT_NAVIGATION_KEY_UP, false, 1002);
    assert(input_keys_event(&keys, BSP_INPUT_NAVIGATION_KEY_UP, true, 1003));
    assert(input_keys_due(&keys, 1503, &repeated));
    /* Tick counter rollover preserves the initial delay. */
    input_keys_init(&keys, 500, 100);
    input_keys_event(&keys, BSP_INPUT_NAVIGATION_KEY_BACKSPACE, true, UINT32_MAX - 250);
    assert(!input_keys_due(&keys, 248, &repeated));
    assert(input_keys_due(&keys, 249, &repeated));
}

int main(void) {
    check_keys();
    radio_station_t a = {.name="Same name", .url="https://example.org/a", .country="BG", .codec="MP3",
        .stationuuid="01234567-89ab-cdef-0123-456789abcdef"};
    radio_station_t b = {.name="Same name", .url="https://example.org/b", .country="BG", .codec="MP3",
        .stationuuid="11234567-89ab-cdef-0123-456789abcdef"};
    playback = (radio_playback_status_t){.station=a, .stopped=true};
    assert(ui_station_action(NULL, &playback) == UI_STATION_DISABLED);
    assert(!ui_activate_station(NULL, true) && !plays && !toggles);
    assert(ui_station_action(&a, &playback) == UI_STATION_PLAY);
    assert(ui_activate_station(&a, true) && plays == 1 && !toggles);
    assert(playback.buffering && ui_station_action(&a, &playback) == UI_STATION_MUTE);
    assert(ui_activate_station(&a, true) && plays == 1 && toggles == 1 && playback.muted);
    assert(ui_station_action(&a, &playback) == UI_STATION_UNMUTE);
    assert(ui_activate_station(&a, true) && plays == 1 && toggles == 2 && !playback.muted);
    assert(ui_activate_station(&a, false) && plays == 1 && toggles == 2); /* Enter never mutes. */
    playback.muted = true;
    assert(ui_activate_station(&a, false) && plays == 1 && toggles == 3 && !playback.muted);

    playback.muted = true;
    assert(ui_station_action(&b, &playback) == UI_STATION_PLAY); /* Names aren't identity. */
    assert(ui_activate_station(&b, true) && plays == 2 && toggles == 4 && !playback.muted);
    assert(ui_station_action(&b, &playback) == UI_STATION_MUTE); /* Immediate pending target. */
    assert(ui_activate_station(&b, true) && plays == 2 && toggles == 5);
    radio_station_t alias = b;
    strcpy(alias.url, "https://example.org/resolved-differently");
    assert(stations_same(&alias, &b));
    assert(ui_station_action(&alias, &playback) == UI_STATION_UNMUTE);
    alias = b; alias.stationuuid[0] = 0;
    assert(stations_same(&alias, &b));
    assert(ui_activate_station(&alias, true) && plays == 2 && toggles == 6);
    playback.stopped = true; playback.muted = true;
    assert(ui_station_action(&b, &playback) == UI_STATION_PLAY);
    assert(ui_activate_station(&b, true) && plays == 3 && !playback.muted);
    assert(!strcmp(ui_station_action_label(UI_STATION_PLAY), "Play"));
    assert(!strcmp(ui_station_action_label(UI_STATION_MUTE), "Mute"));
    assert(!strcmp(ui_station_action_label(UI_STATION_UNMUTE), "Unmute"));

    FILE *catalog = fopen("stations.txt", "wb"); assert(catalog);
    fprintf(catalog, "#RADIOBROWSERUUID:%s\n%s:::%s:::%s:::%s:::128\n",
            a.stationuuid, a.name, a.url, a.country, a.codec);
    assert(fclose(catalog) == 0 && stations_load_file("stations.txt") == ESP_OK);
    ui_station_list_t list = {0};
    ui_station_list_refresh(&list, &a);
    assert(!list.has_preview && list.selection == 0);
    ui_station_list_focus(&list, &b);
    assert(list.has_preview && list.selection == SIZE_MAX);
    radio_station_t selected;
    assert(ui_station_list_selected(&list, &selected) && stations_same(&selected, &b));
    ui_station_list_move(&list, 1);
    assert(list.selection == 0 && ui_station_list_selected(&list, &selected) && stations_same(&selected, &a));
    ui_station_list_move(&list, -1); assert(list.selection == SIZE_MAX);
    size_t saved_index; bool duplicate;
    assert(stations_save("stations.txt", &b, &saved_index, &duplicate) == ESP_OK);
    assert(!duplicate && saved_index == 1);
    ui_station_list_refresh(&list, &b);
    assert(!list.has_preview && list.selection == 1); /* Saved preview becomes its saved row. */
    assert(ui_station_list_selected(&list, &selected) && stations_same(&selected, &playback.station));
    unsigned previous_plays = plays;
    assert(ui_activate_station(&selected, true) && plays == previous_plays); /* Saving doesn't reconnect. */
    ui_station_list_focus(&list, &a); assert(list.selection == 0);
    alias = a; strcpy(alias.url, "https://example.org/new-a");
    assert(stations_find(&alias, &saved_index) && saved_index == 0);
    ui_station_list_focus(&list, &alias); assert(!list.has_preview && list.selection == 0);
    catalog = fopen("empty.txt", "wb"); assert(catalog); fclose(catalog);
    assert(stations_load_file("empty.txt") == ESP_ERR_NOT_FOUND);
    ui_station_list_refresh(&list, &b);
    assert(list.has_preview && list.selection == SIZE_MAX);
    ui_station_list_move(&list, 1); assert(list.selection == SIZE_MAX);
    ui_station_list_move(&list, -1); assert(list.selection == SIZE_MAX);
    radio_station_t empty = {0};
    ui_station_list_refresh(&list, &empty);
    assert(!list.has_preview && !ui_station_list_selected(&list, &selected));
    ui_station_list_move(&list, 1);
    puts("PASS: Play/Mute/Unmute decisions, stopped/buffering states, station identity, Enter, repeat timing, duplicate presses, releases and rollover");
    puts("PASS: preview focus, catalog navigation, saving an active preview without reconnecting, aliases and empty catalogs");
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix="tanradio-controls-") as directory:
        work = Path(directory)
        for name in ["esp_err.h", "esp_log.h", "esp_heap_caps.h", "freertos/FreeRTOS.h", "freertos/queue.h"]:
            target = work / name
            target.parent.mkdir(exist_ok=True)
            target.write_text(STUBS)
        (work / "check.c").write_text(HARNESS)
        command = ["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-sign-compare",
                   "-g", "-fsanitize=address,undefined", "-I", str(work), "-I", str(ROOT / "main"),
                   "-I", str(ROOT / "local-components/badgeteam__badge-bsp"),
                   str(work / "check.c"), str(ROOT / "main/ui/controls.c"),
                   str(ROOT / "main/input/keys.c"), str(ROOT / "main/radio/stations.c"),
                   "-o", str(work / "check")]
        subprocess.run(command, check=True)
        subprocess.run([str(work / "check")], check=True, cwd=work,
                       env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))


if __name__ == "__main__":
    main()

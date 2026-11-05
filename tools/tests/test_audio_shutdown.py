#!/usr/bin/env python3
"""Compile the real audio module against a blocking host I2S mock.

Run with python3 tools/tests/test_audio_shutdown.py (requires cc and pthreads).
This checks lock independence and exit ordering; it does not emulate FreeRTOS.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
STUBS = r'''
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 1
#define ESP_ERR_INVALID_ARG 2
#define ESP_ERR_TIMEOUT 3
#define ESP_ERR_NOT_SUPPORTED 4
#define ESP_FAIL 5
#define pdPASS 1
#define pdMS_TO_TICKS(ms) (ms)
typedef uint32_t TickType_t;
static TickType_t ticks;
static TickType_t xTaskGetTickCount(void) { return ticks; }
static void vTaskDelay(TickType_t t) { (void)t; }
static void vTaskDelete(void *p) { (void)p; }
static int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg, unsigned priority, void *h) { return pdPASS; }
#define NVS_READONLY 0
#define NVS_READWRITE 1
typedef int nvs_handle_t;
static bool nvs_present, jack;
static uint8_t saved_speaker = 50, saved_headphones = 50;
static int saves, fail_volume, fail_amp, fail_read, fail_commit, fail_rate, fail_enable;
static esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *h) { *h = 1; return mode == NVS_READONLY && !nvs_present ? ESP_FAIL : ESP_OK; }
static esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *v) { *v = key[0] == 's' ? saved_speaker : saved_headphones; return ESP_OK; }
static esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t v) { if(key[0] == 's') saved_speaker = v; else saved_headphones = v; return ESP_OK; }
static esp_err_t nvs_commit(nvs_handle_t h) { if (fail_commit) { fail_commit--; return ESP_FAIL; } nvs_present = true; saves++; return ESP_OK; }
static void nvs_close(nvs_handle_t h) {}
#define BSP_INPUT_ACTION_TYPE_AUDIO_JACK 1
static esp_err_t bsp_input_read_action(int type, bool *v) { if (fail_read) { fail_read--; return ESP_FAIL; } *v = jack; return ESP_OK; }
#define portMAX_DELAY UINT32_MAX
#define ESP_LOGE(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGI(tag, ...) ((void)(tag))
typedef pthread_mutex_t *SemaphoreHandle_t;
static SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    SemaphoreHandle_t m = malloc(sizeof(*m));
    assert(m && pthread_mutex_init(m, NULL) == 0);
    return m;
}
static int xSemaphoreTake(SemaphoreHandle_t m, uint32_t timeout) {
    (void)timeout; assert(pthread_mutex_lock(m) == 0); return 1;
}
static int xSemaphoreGive(SemaphoreHandle_t m) {
    assert(pthread_mutex_unlock(m) == 0); return 1;
}
typedef void *i2s_chan_handle_t;
typedef struct { uint32_t total_dma_buf_size; } i2s_chan_info_t;
static atomic_bool writing, release_write, control_done, amp, force, disabled;
static atomic_int volume, writes;
static atomic_size_t silence_bytes;
static bool format_mode;
static unsigned configured_rate = 44100;
static void pause_thread(void) {
    struct timespec duration = {0, 1000000}; nanosleep(&duration, NULL);
}
static void wait_flag(atomic_bool *flag) {
    for (int i = 0; i < 2000 && !atomic_load(flag); i++) pause_thread();
    assert(atomic_load(flag));
}
static esp_err_t bsp_audio_get_i2s_handle(i2s_chan_handle_t *h) { *h = (void *)1; return ESP_OK; }
static esp_err_t bsp_audio_set_volume(float v) { if(fail_volume) { fail_volume--; return ESP_FAIL; } atomic_store(&volume, (int)v); return ESP_OK; }
static esp_err_t bsp_audio_set_amplifier(bool v) { if(fail_amp) { fail_amp--; return ESP_FAIL; } atomic_store(&amp, v); return ESP_OK; }
static esp_err_t bsp_audio_set_rate(uint32_t r) { assert(atomic_load(&disabled)); if (fail_rate) { fail_rate--; return ESP_FAIL; } configured_rate = r; return ESP_OK; }
static esp_err_t i2s_channel_enable(i2s_chan_handle_t h) { if (fail_enable) { fail_enable--; return ESP_FAIL; } atomic_store(&disabled, false); return ESP_OK; }
static esp_err_t bsp_audio_set_amplifier_force(bool v) { atomic_store(&force, v); return ESP_OK; }
static esp_err_t i2s_channel_get_info(i2s_chan_handle_t h, i2s_chan_info_t *info) {
    assert(h); info->total_dma_buf_size = 5760; return ESP_OK;
}
static esp_err_t i2s_channel_write(i2s_chan_handle_t h, const void *data, size_t size,
                                  size_t *written, uint32_t timeout) {
    assert(h && timeout != portMAX_DELAY && !atomic_load(&disabled));
    if (!format_mode && atomic_fetch_add(&writes, 1) == 0) {
        atomic_store(&writing, true);
        wait_flag(&release_write);
    } else {
        assert(atomic_load(&volume) == 0 && (format_mode || !atomic_load(&amp)) && !atomic_load(&force));
        const uint8_t *bytes = data;
        for (size_t i = 0; i < size; i++) assert(bytes[i] == 0);
        atomic_fetch_add(&silence_bytes, size);
    }
    *written = size;
    return ESP_OK;
}
static esp_err_t i2s_channel_disable(i2s_chan_handle_t h) {
    assert(h && atomic_load(&silence_bytes) % 11520 == 0);
    atomic_store(&disabled, true); return ESP_OK;
}
'''
TEST = r'''
#include "main/audio/audio.c"
static void *play(void *arg) {
    (void)arg; const uint8_t pcm[32] = {1};
    assert(audio_write_pcm(pcm, sizeof(pcm)) == ESP_OK); return NULL;
}
static void *control(void *arg) {
    (void)arg;
    assert(audio_volume_up() == ESP_OK);
    assert(audio_mute() == ESP_OK);
    assert(audio_unmute() == ESP_OK);
    atomic_store(&control_done, true); return NULL;
}
static void *shutdown_output(void *arg) { (void)arg; audio_shutdown(); return NULL; }
int main(int argc, char **argv) {
    (void)argv;
    if (argc > 1) {
        jack = true; nvs_present = true; saved_speaker = 200; saved_headphones = 35;
        audio_init();
        assert(audio_get_volume() == 35 && audio_volumes[0] == 50);
        assert(!atomic_load(&amp) && !atomic_load(&force) && atomic_load(&volume) == 0);
        puts("PASS: headphones at startup, independent NVS restore and invalid-setting fallback");
        return 0;
    }
    pthread_t playback, keys, shutdown;
    audio_init();
    assert(audio_get_volume() == 50 && !atomic_load(&force));
    assert(atomic_load(&volume) == 0); /* Startup stays silent until buffered. */
    assert(audio_unmute() == ESP_OK);
    /* Independent saved levels and route changes, including while muted. */
    assert(audio_volume_up() == ESP_OK); /* speaker 51 */
    jack = true; assert(audio_refresh_output() == ESP_OK);
    assert(audio_get_volume() == 50 && !atomic_load(&amp));
    assert(audio_volume_down() == ESP_OK && audio_get_volume() == 49);
    assert(audio_mute() == ESP_OK);
    jack = false; assert(audio_refresh_output() == ESP_OK);
    assert(audio_get_volume() == 51 && atomic_load(&volume) == 0);
    assert(audio_volume_down() == ESP_OK);
    assert(audio_unmute() == ESP_OK && atomic_load(&volume) == 50);
    ticks = 1499; audio_save_settings(false); assert(!saves);
    ticks = 1500; audio_save_settings(false);
    assert(saves == 1 && saved_speaker == 50 && saved_headphones == 49);
    audio_volumes[0] = audio_volumes[1] = 1;
    audio_load_settings(); assert(audio_volumes[0] == 50 && audio_volumes[1] == 49);
    /* Failed route application stays silent and retries without losing levels. */
    jack = true; fail_volume = 1;
    assert(audio_refresh_output() == ESP_FAIL && !audio_output_ready);
    assert(!atomic_load(&amp) && atomic_load(&volume) == 0);
    assert(audio_refresh_output() == ESP_OK && audio_get_volume() == 49);
    jack = false; fail_amp = 1;
    assert(audio_refresh_output() == ESP_FAIL && !audio_output_ready);
    assert(audio_refresh_output() == ESP_OK && atomic_load(&amp));
    fail_read = 1;
    assert(audio_refresh_output() == ESP_FAIL && !audio_output_ready && !atomic_load(&amp));
    assert(audio_refresh_output() == ESP_OK && atomic_load(&amp));
    format_mode = true;
    assert(audio_set_sample_rate(32000) == ESP_ERR_NOT_SUPPORTED);
    assert(audio_set_sample_rate(48000) == ESP_OK && configured_rate == 48000);
    assert(audio_muted && atomic_load(&volume) == 0);
    assert(audio_set_sample_rate(44100) == ESP_OK && configured_rate == 44100);
    fail_rate = 1;
    assert(audio_set_sample_rate(48000) == ESP_FAIL && configured_rate == 44100);
    assert(audio_i2s_running && audio_sample_rate == 44100);
    fail_enable = 1;
    assert(audio_set_sample_rate(48000) == ESP_FAIL && !audio_i2s_running);
    assert(audio_set_sample_rate(48000) == ESP_OK && audio_i2s_running);
    format_mode = false; atomic_store(&silence_bytes, 0);
    assert(audio_unmute() == ESP_OK);
    assert(audio_volume_up() == ESP_OK && audio_get_volume() == 51);
    assert(audio_volume_down() == ESP_OK && audio_get_volume() == 50);
    for (unsigned n = 50; n < 100; n++) {
        assert(audio_volume_up() == ESP_OK && audio_get_volume() == n + 1);
    }
    assert(audio_volume_up() == ESP_OK && audio_get_volume() == 100);
    for (unsigned n = 100; n > 0; n--) {
        assert(audio_volume_down() == ESP_OK && audio_get_volume() == n - 1);
    }
    assert(audio_volume_down() == ESP_OK && audio_get_volume() == 0);
    assert(audio_mute() == ESP_OK);
    assert(audio_volume_up() == ESP_OK && audio_get_volume() == 1);
    assert(atomic_load(&volume) == 0);
    assert(audio_unmute() == ESP_OK && atomic_load(&volume) == 1);
    for (unsigned n = 1; n < 50; n++) assert(audio_volume_up() == ESP_OK);
    fail_commit = 1;
    audio_save_settings(true);
    assert(audio_saved_generation != audio_settings_generation);
    ticks += 1500; audio_save_settings(false);
    assert(audio_saved_generation == audio_settings_generation);
    assert(pthread_create(&playback, NULL, play, NULL) == 0);
    wait_flag(&writing);
    assert(pthread_create(&keys, NULL, control, NULL) == 0);
    /* Key controls must finish while the PCM write is still blocked. */
    wait_flag(&control_done);
    assert(!atomic_load(&release_write) && atomic_load(&volume) == 51);
    pthread_join(keys, NULL);
    assert(pthread_create(&shutdown, NULL, shutdown_output, NULL) == 0);
    wait_flag(&audio_shutdown_started);
    const uint8_t pcm[4] = {1};
    assert(audio_write_pcm(pcm, sizeof(pcm)) == ESP_ERR_INVALID_STATE);
    assert(atomic_load(&writes) == 1);
    atomic_store(&release_write, true);
    pthread_join(playback, NULL);
    pthread_join(shutdown, NULL);
    assert(atomic_load(&disabled));
    assert(audio_unmute() == ESP_ERR_INVALID_STATE);
    assert(audio_volume_up() == ESP_ERR_INVALID_STATE);
    assert(audio_refresh_output() == ESP_ERR_INVALID_STATE);
    assert(audio_set_sample_rate(48000) == ESP_ERR_INVALID_STATE);
    int before = atomic_load(&writes);
    audio_shutdown();
    assert(atomic_load(&writes) == before && atomic_load(&volume) == 0);
    puts("PASS: controls stay responsive during PCM writes; shutdown blocks new PCM/unmute, flushes silence, and can repeat");
}
'''
with tempfile.TemporaryDirectory(prefix="tanradio-audio-test-") as directory:
    tmp = Path(directory)
    (tmp / "stubs.h").write_text(STUBS)
    for name in ("esp_err.h", "esp_log.h", "freertos/FreeRTOS.h",
                 "freertos/semphr.h", "freertos/task.h", "nvs.h", "bsp/input.h",
                 "driver/i2s_std.h", "bsp/audio.h"):
        path = tmp / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#include "stubs.h"\n')
    (tmp / "test.c").write_text(TEST)
    binary = tmp / "test"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Wno-unused-parameter",
                    "-pthread", "-I", str(tmp), "-I", str(ROOT),
                    str(tmp / "test.c"), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
    subprocess.run([str(binary), "headphones"], check=True, timeout=10)

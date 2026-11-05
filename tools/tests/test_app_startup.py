#!/usr/bin/env python3
"""Verify UI/input precede background transport and catalog initialization."""
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
#include <stdio.h>
typedef int esp_err_t;
typedef void *QueueHandle_t;
#define ESP_OK 0
#define pdPASS 1
#define ESP_LOGE(tag, ...) ((void)(tag), (void)sizeof(printf(__VA_ARGS__)))
#define ESP_LOGI(tag, ...) ((void)(tag), (void)sizeof(printf(__VA_ARGS__)))
int64_t esp_timer_get_time(void);
int xTaskCreate(void (*)(void *), const char *, unsigned, void *, unsigned, void *);
void vTaskDelete(void *);
'''
HARNESS = r'''
#include "main/app/app.c"
static unsigned phase;
static bool welcome = true, task_fail;
static ui_services_state_t state = UI_SERVICES_STARTING;
static void (*background)(void *);
int64_t esp_timer_get_time(void) { return 0; }
void util_init(void) { assert(++phase == 1); }
void ui_init(void) { assert(++phase == 2); }
bool ui_show_welcome(void) { assert(++phase == 3); return welcome; }
void audio_init(void) { assert(++phase == 4); }
void radio_browser_init(void) { assert(++phase == 5); }
void ui_start(void) { assert(++phase == 6); }
void input_init(void) { assert(++phase == 7); }
int xTaskCreate(void (*task)(void *), const char *name, unsigned stack,
                void *arg, unsigned priority, void *handle) {
    assert(++phase == 8 && name && stack == 8192 && !arg && priority && !handle);
    background = task_fail ? NULL : task;
    return task_fail ? 0 : pdPASS;
}
void network_init(void) { assert(++phase == 9); }
void radio_init(void) { assert(++phase == 10); }
void ui_set_services_state(ui_services_state_t next) {
    assert((task_fail && phase == 8 && next == UI_SERVICES_FAILED) ||
           (!task_fail && ++phase == 11 && next == UI_SERVICES_READY));
    state = next;
}
void vTaskDelete(void *task) { assert(!task && ++phase == 12); }
int main(void) {
    app_init();
    assert(phase == 8 && background && state == UI_SERVICES_STARTING);
    /* Main screen, volume and F1 input are active before any slow work runs. */
    background(NULL); assert(phase == 12 && state == UI_SERVICES_READY);
    phase = 0; task_fail = true; app_init();
    assert(phase == 8 && !background && state == UI_SERVICES_FAILED);
    phase = 0; welcome = false; app_init(); assert(phase == 3);
    puts("PASS: immediate UI/input, background transport-before-catalog ordering, startup failure and welcome exit");
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix='tanradio-app-') as directory:
        work = Path(directory)
        (work / 'runtime.h').write_text(STUBS)
        for name in ['esp_err.h', 'esp_log.h', 'esp_timer.h', 'freertos/FreeRTOS.h',
                     'freertos/task.h', 'freertos/queue.h']:
            target = work / name
            target.parent.mkdir(exist_ok=True)
            target.write_text('#include "runtime.h"\n')
        (work / 'check.c').write_text(HARNESS)
        subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                        '-I', str(work), '-I', str(ROOT), '-I', str(ROOT / 'main'),
                        '-I', str(ROOT / 'local-components/badgeteam__badge-bsp'),
                        str(work / 'check.c'), '-o', str(work / 'check')], check=True)
        subprocess.run([str(work / 'check')], check=True)


if __name__ == '__main__':
    main()

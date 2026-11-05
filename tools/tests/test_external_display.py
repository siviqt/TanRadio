#!/usr/bin/env python3
"""Verify driver lifecycle, latest-frame delivery, and explicit retry."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "external_display/driver.h"

typedef void *QueueHandle_t;
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define portTICK_PERIOD_MS 1
typedef uint32_t TickType_t;
#define pdMS_TO_TICKS(ms) (ms)

static unsigned opens, presents, closes, ticks, step;
static jmp_buf done;
static bool pending, fail_present, fail_open, worker_created;
static uint8_t expected_contrast = 60;
static unsigned char mail[1024];
static size_t mail_size;
static void (*run_worker)(void *);
static int xQueueReceive(QueueHandle_t queue, void *out, unsigned wait);

static QueueHandle_t xQueueCreate(unsigned count, size_t size) {
    assert(count == 1 && size <= sizeof(mail));
    mail_size = size;
    return mail;
}
static int xQueueOverwrite(QueueHandle_t queue, const void *item) {
    assert(queue == mail);
    memcpy(mail, item, mail_size);
    pending = true;
    return pdTRUE;
}
static int xTaskCreate(void (*worker)(void *), const char *name, unsigned stack,
                       void *arg, unsigned priority, void *out) {
    (void)name; (void)stack; (void)arg; (void)priority; (void)out;
    assert(!worker_created);
    worker_created = true;
    run_worker = worker;
    return pdPASS;
}
static void vQueueDelete(QueueHandle_t queue) { (void)queue; assert(false); }
static TickType_t xTaskGetTickCount(void) { return ticks; }
static void vTaskDelay(unsigned count) { ticks += count; }

static bool open(uint8_t contrast) {
    assert(contrast == expected_contrast);
    opens++;
    return !fail_open;
}
static bool present(const external_frame_t *frame, uint32_t now_ms, uint32_t *refresh_ms) {
    assert(strcmp(frame->title, "Dropped frame"));
    assert(*refresh_ms == 0 && now_ms == ticks);
    presents++;
    if (presents == 1) *refresh_ms = 2200;
    if (presents == 2) assert(now_ms == 2200 && !strcmp(frame->title, "Current title"));
    return !fail_present;
}
static void close(void) { closes++; }
const external_driver_t external_nokia5110 = {open, present, close};

#include "external_display/external_display.c"

static int xQueueReceive(QueueHandle_t queue, void *out, unsigned wait) {
    assert(queue == mail);
    if (!pending) {
        external_frame_t frame = {.station = "Radio", .title = "Current title"};
        switch (step++) {
        case 0:
            assert(wait == 2200);
            assert(opens == 1 && presents == 1 && closes == 0);
            ticks += wait;
            return 0; /* A driver timer can redraw without a new UI message. */
        case 1:
            assert(wait == portMAX_DELAY && presents == 2);
            assert(external_display_status() == EXTERNAL_INITIALIZED);
            external_display_submit(EXTERNAL_DISPLAY_NOKIA5110, 60, &frame, false);
            assert(!pending); /* Identical rendered input. */
            fail_present = true;
            strcpy(frame.title, "Update");
            external_display_submit(EXTERNAL_DISPLAY_NOKIA5110, 60, &frame, false);
            break;
        case 2:
            assert(opens == 1 && presents == 3 && closes == 1);
            assert(external_display_status() == EXTERNAL_FAILED);
            external_display_submit(EXTERNAL_DISPLAY_NOKIA5110, 60, &frame, false);
            break;
        case 3:
            assert(opens == 1 && presents == 3 && closes == 1);
            fail_open = true;
            external_display_submit(EXTERNAL_DISPLAY_NOKIA5110, 60, &frame, true);
            break;
        case 4:
            assert(opens == 2 && presents == 3 && closes == 2);
            assert(external_display_status() == EXTERNAL_FAILED);
            fail_open = fail_present = false;
            expected_contrast = 61;
            external_display_submit(EXTERNAL_DISPLAY_NOKIA5110, 61, &frame, true);
            break;
        case 5:
            assert(opens == 3 && presents == 4 && closes == 2);
            assert(external_display_status() == EXTERNAL_INITIALIZED);
            external_display_submit(EXTERNAL_DISPLAY_OFF, 61, &frame, false);
            break;
        default:
            assert(wait == portMAX_DELAY && opens == 3 && presents == 4 && closes == 3);
            assert(external_display_status() == EXTERNAL_DISABLED);
            longjmp(done, 1);
        }
    }
    assert(pending);
    pending = false;
    memcpy(out, mail, mail_size);
    return pdTRUE;
}

int main(void) {
    external_frame_t frame = {.station = "Radio", .title = "Dropped frame"};
    external_display_submit(EXTERNAL_DISPLAY_OFF, 60, &frame, false);
    assert(!worker_created && external_display_status() == EXTERNAL_DISABLED);
    external_display_stop();
    assert(!ticks && !pending);
    external_display_submit(EXTERNAL_DISPLAY_NOKIA5110, 60, &frame, false);
    strcpy(frame.title, "Current title");
    external_display_submit(EXTERNAL_DISPLAY_NOKIA5110, 60, &frame, false);
    if (!setjmp(done)) run_worker(NULL);
    puts("PASS: disabled startup, latest frame, timed redraw, open/present/close, fault isolation, retry and pin release request");
}
'''

with tempfile.TemporaryDirectory(prefix="tanradio-external-") as directory:
    work = Path(directory)
    (work / "freertos").mkdir()
    for name in ("FreeRTOS", "queue", "task"):
        (work / "freertos" / (name + ".h")).touch()
    (work / "check.c").write_text(HARNESS)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "main"),
                    "-I", str(work), str(work / "check.c"), "-o", str(work / "check")], check=True)
    subprocess.run([str(work / "check")], check=True)

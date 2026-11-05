#!/usr/bin/env python3
"""Exercise production redraw scheduling and display transfer backpressure.

The deterministic host clock models bursts, idle periods and tick rollover;
the panel mock models native page flips, delayed retirement and timeouts.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(path, signature):
    source = path.read_text()
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <setjmp.h>
#include <stdio.h>
typedef uint32_t TickType_t;
typedef int esp_err_t;
#define pdTRUE 1
#define ESP_OK 0
#define ESP_ERR_TIMEOUT 1
#define ESP_FAIL 2
#define ESP_ERR_INVALID_ARG 3
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) ((ms) / 10)
#define UI_FRAME_INTERVAL_MS 100
#define UI_NOW_PLAYING_INTERVAL_MS 250
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
static void *ui_mutex = (void *)1, *flush_semaphore = (void *)2;
static void *present_mutex = (void *)5;
static void *ui_status_task_handle = (void *)4;
static unsigned retry_requests;
static void xTaskNotifyGive(void *task) { assert(task == ui_status_task_handle); retry_requests++; }
static void advance_display(void);
static bool ui_search_open = true, ui_display_enabled = true;
#define UI_EXIT_COMMITTED 2
static int ui_exit_state;
static void ui_complete_exit(void) {}
static uint32_t ui_search_revision;
typedef struct { uint32_t revision; } radio_browser_status_t;
static TickType_t ticks;
static unsigned elapsed, pending, latest, drawn, frames, polls, previous_frame;
static unsigned frame_times[100];
static jmp_buf finished;
static void advance(unsigned count) {
    for (unsigned i = 0; i < count; i++) {
        ticks++; elapsed++;
        /* 200 rapid updates, then idle, then an isolated update. */
        if (elapsed <= 200 || elapsed == 350) { pending++; latest++; }
    }
}
static TickType_t xTaskGetTickCount(void) { return ticks; }
static unsigned ulTaskNotifyTake(int clear, TickType_t wait) {
    assert(clear);
    while (!pending && wait--) {
        advance(1);
        if (elapsed >= 500) longjmp(finished, 1);
    }
    unsigned result = pending; pending = 0; return result;
}
static void vTaskDelay(TickType_t delay) { while (delay--) { advance(1); advance_display(); } }
static bool ui_status_changed(void) { polls++; return false; }
static bool ui_sync_now_playing(void) { return false; }
static void radio_browser_status(radio_browser_status_t *out) { out->revision = latest; }
static void ui_render(void) {
    assert(frames < 100);
    if (frames) assert(elapsed - previous_frame >= 10);
    previous_frame = elapsed; frame_times[frames++] = elapsed;
    drawn = latest;
    /* A new request may arrive during drawing; it must remain pending. */
    advance(2);
}
static int flush_token = 1, present_token = 1;
static int xSemaphoreTake(void *sem, TickType_t timeout) {
    if (sem == ui_mutex) return pdTRUE;
    assert(!timeout);
    int *token = sem == present_mutex ? &present_token : &flush_token;
    assert(sem == present_mutex || sem == flush_semaphore);
    if (!*token) return 0;
    *token = 0; return pdTRUE;
}
static void xSemaphoreGive(void *sem) {
    if (sem == ui_mutex) return;
    assert(sem == present_mutex || sem == flush_semaphore);
    if (sem == present_mutex) present_token = 1; else flush_token = 1;
}
static uint8_t framebuffer[8], native_pages[2][8];
static void *display_framebuffers[2] = {native_pages[0], native_pages[1]};
static size_t display_width = 2, display_height = 2, display_pixel_bytes = 2;
static unsigned display_front_index, hardware_front_index;
static uint32_t display_refresh_count, display_flip_start;
static bool display_flip_pending;
static unsigned refresh_budget, draw_calls;
static const void *in_flight;
static esp_err_t submission_result;
static void advance_display(void) {
    if (!refresh_budget) return;
    refresh_budget--;
    if (in_flight) hardware_front_index = in_flight == native_pages[1];
    display_refresh_count++;
}
static void *st7701_get_panel(void) { return (void *)3; }
static esp_err_t esp_lcd_panel_draw_bitmap(void *panel, size_t x, size_t y,
                                          size_t w, size_t h, const void *pixels) {
    assert(panel && !x && !y && w == 2 && h == 2);
    /* Native pages select the driver's flip path. An application snapshot
     * would select the unsafe copy-into-current-scanout path. */
    assert(pixels == native_pages[hardware_front_index ^ 1]);
    draw_calls++;
    if (submission_result == ESP_OK) in_flight = pixels;
    return submission_result;
}
static size_t display_h_res = 2, display_v_res = 2;
static const void *pax_buf_get_pixels(const void *buf) { return buf; }

'''

TEST = r'''
int main(void) {
    /* The active page stays intact while a new complete image is prepared.
     * Two refresh completions retire the old page before it can be reused. */
    memset(framebuffer, 1, sizeof(framebuffer)); refresh_budget = 2;
    assert(ui_blit());
    assert(draw_calls == 1 && display_front_index == 1 && hardware_front_index == 1);
    assert(!native_pages[0][0] && native_pages[1][0] == 1 && !display_flip_pending);

    /* Only one completion: the driver has accepted the new page but the
     * previous page must remain protected, including on repeated retries. */
    memset(framebuffer, 2, sizeof(framebuffer)); refresh_budget = 1;
    assert(!ui_blit() && display_flip_pending && present_token);
    assert(display_front_index == 1 && native_pages[0][0] == 2 && native_pages[1][0] == 1);
    unsigned before = draw_calls, retries_before = retry_requests;
    memset(framebuffer, 3, sizeof(framebuffer));
    assert(!ui_blit() && draw_calls == before);
    assert(native_pages[0][0] == 2 && native_pages[1][0] == 1);
    assert(retry_requests == retries_before + 1);
    refresh_budget = 3; /* Retire pending flip, then present the latest image. */
    assert(ui_blit() && !display_flip_pending);
    assert(native_pages[0][0] == 2 && native_pages[1][0] == 3);
    assert(display_front_index == 1 && hardware_front_index == 1);

    /* Rejected submission leaves the front page and page ownership intact. */
    submission_result = ESP_FAIL;
    assert(!ui_blit() && !display_flip_pending && display_front_index == 1);
    submission_result = ESP_OK;
    refresh_budget = 2; display_refresh_count = UINT32_MAX - 1;
    memset(framebuffer, 4, sizeof(framebuffer)); assert(ui_blit());
    assert(display_refresh_count == 0 && display_front_index == 0);
    assert(native_pages[0][0] == 4 && native_pages[1][0] == 3);

    /* A busy presenter skips safely; a partial update preserves other pixels. */
    present_token = 0; before = draw_calls;
    assert(!ui_blit() && draw_calls == before); present_token = 1;
    uint8_t partial[2] = {9, 9}; refresh_budget = 2;
    assert(bsp_display_blit(1, 1, 2, 2, partial) == ESP_OK);
    assert(native_pages[1][0] == 4 && native_pages[1][6] == 9);
    assert(bsp_display_blit(0, 0, 3, 2, partial) == ESP_ERR_INVALID_ARG);

    for (unsigned rollover = 0; rollover < 2; rollover++) {
        ticks = rollover ? UINT32_MAX - 15 : 0;
        elapsed = pending = latest = drawn = frames = polls = previous_frame = 0;
        ui_search_revision = 0;
        if (!setjmp(finished)) ui_status_task(NULL);
        assert(latest == 201 && drawn == latest);
        assert(frames >= 20 && frames <= 23);
        assert(polls == 4);
        /* Idle does not delay a lone update until the next status poll. */
        assert(frame_times[frames - 1] == 350);
    }
    puts("PASS: redraw bursts, latest-state delivery, idle wakeups, tick rollover, native page flips, two-frame retirement, timeout ownership, retry delivery and partial updates");
}
'''


def main():
    ui = ROOT / "main/ui/ui.c"
    bsp = ROOT / "local-components/badgeteam__badge-bsp/targets/tanmatsu/badge_bsp_display.c"
    source = HARNESS + function(bsp, "static esp_err_t bsp_display_retire_flip(")
    source += function(bsp, "static esp_err_t bsp_display_present(")
    source += function(bsp, "esp_err_t bsp_display_blit(")
    source += function(ui, "static bool ui_blit(void)")
    source += function(ui, "static void ui_status_task(void *arg)") + TEST
    with tempfile.TemporaryDirectory(prefix="tanradio-display-") as tmp:
        path = Path(tmp)
        (path / "check.c").write_text(source)
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        str(path / "check.c"), "-o", str(path / "check")], check=True)
        subprocess.run([str(path / "check")], check=True)


if __name__ == "__main__":
    main()

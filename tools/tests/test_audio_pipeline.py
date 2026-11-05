#!/usr/bin/env python3
"""Exercise the production queue/format/consumer boundary with deterministic I/O."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


source = (ROOT / 'main/radio/radio.c').read_text()
production = '\n'.join(function(source, signature) for signature in (
    'static bool radio_stream_interrupted(', 'static bool radio_take_title(', 'static void radio_publish_pcm_title(', 'static void radio_audio_step(', 'static bool radio_queue_stereo(',
    'static bool radio_queue_pcm(', 'static void radio_discard_pcm(',
    'static esp_err_t radio_prepare_output(', 'static bool radio_toggle_mute_locked('))
stubs = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "audio/pcm.h"
#include "radio/now_playing.h"
#define pdTRUE 1
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL 1
#define portMAX_DELAY 0
#define pdMS_TO_TICKS(ms) (ms)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define RADIO_AUDIO_WRITE_CHUNK_SIZE 16
#define RADIO_PCM_HIGH_WATERMARK 64
#define RADIO_PCM_LOW_WATERMARK 32
static void *radio_pcm_stream = (void *)1, *radio_output_mutex = (void *)2;
typedef void *QueueHandle_t;
typedef struct {
    uint64_t position;
    uint32_t connection;
    char title[RADIO_TITLE_SIZE];
} radio_title_marker_t;
static uint64_t radio_pcm_position;
static uint32_t radio_pcm_connection, radio_connection = 7, radio_metadata_revision;
static radio_metadata_t radio_metadata;
static radio_title_marker_t markers[4];
static unsigned marker_count;
static void *radio_pcm_titles = (void *)4, *radio_station_mutex = (void *)5;
static void *radio_task_handle = (void *)3;
static bool radio_stop_requested, radio_exit_requested, radio_switch_requested, radio_resume_requested;
static bool radio_output_ready, radio_buffering, radio_muted;
static bool locked, muted = true, fail_rate;
static unsigned rate, writes;
static uint8_t queue[2048], last_write[16];
static size_t queued;
static const char *TAG;
static void xSemaphoreTake(void *m, int t) {
    (void)t; assert(m);
    if (m != radio_station_mutex) { assert(!locked); locked=true; }
}
static void xSemaphoreGive(void *m) {
    assert(m);
    if (m != radio_station_mutex) { assert(locked); locked=false; }
}
static int xQueuePeek(void *q, radio_title_marker_t *marker, unsigned wait) {
    assert(q == radio_pcm_titles && !wait);
    if (!marker_count) return 0;
    *marker = markers[0];
    return pdTRUE;
}
static int xQueueReceive(void *q, radio_title_marker_t *marker, unsigned wait) {
    if (!xQueuePeek(q, marker, wait)) return 0;
    --marker_count;
    memmove(markers, markers + 1, marker_count * sizeof(*markers));
    return pdTRUE;
}
static void xQueueReset(void *q) { assert(locked && q == radio_pcm_titles); marker_count = 0; }
static void vTaskDelay(unsigned t) { (void)t; }
static void radio_set_buffering(bool b) { radio_buffering = b; }
static int audio_mute(void) { muted=true; return ESP_OK; }
static int audio_unmute(void) { muted=false; return ESP_OK; }
static const char *esp_err_to_name(int e) { (void)e; return "error"; }
static int audio_set_sample_rate(uint32_t r) {
    assert(locked && !radio_output_ready && !queued && radio_buffering);
    muted=true; if(fail_rate) return ESP_FAIL; rate=r; return ESP_OK;
}
static void xStreamBufferReset(void *s) { assert(s && locked); queued=0; }
static size_t xStreamBufferBytesAvailable(void *s) { assert(s && locked); return queued; }
static size_t xStreamBufferSend(void *s, const void *data, size_t size, unsigned t) {
    (void)t; assert(s && !locked && queued+size <= sizeof(queue));
    memcpy(queue+queued,data,size); queued+=size; return size;
}
static size_t xStreamBufferReceive(void *s, void *data, size_t size, unsigned t) {
    (void)t; assert(s && locked && queued >= size);
    memcpy(data,queue,size); queued-=size; memmove(queue,queue+size,queued); return size;
}
static int audio_write_pcm(const void *data, size_t size) {
    assert(locked && size == sizeof(last_write));
    memcpy(last_write,data,size); writes++; return ESP_OK;
}
'''
checks = r'''
static void step(void) {
    uint8_t chunk[16];
    xSemaphoreTake(radio_output_mutex,0); radio_audio_step(chunk); xSemaphoreGive(radio_output_mutex);
}
int main(void) {
    (void)TAG; (void)esp_err_to_name;
    queued=100; step(); /* Not configured: clock silence, leave queued data alone. */
    assert(queued==100 && !memcmp(last_write,(uint8_t[16]){0},16));
    assert(radio_prepare_output(48000, 7)==ESP_OK && rate==48000 && !queued && muted);
    int16_t mono[]={-32768,0,32767};
    assert(radio_queue_pcm((const uint8_t *)mono,sizeof(mono),1));
    assert(queued==12);
    int16_t expected[]={-32768,-32768,0,0,32767,32767};
    assert(!memcmp(queue,expected,sizeof(expected)));
    assert(!radio_queue_pcm((const uint8_t *)mono,5,1));
    assert(!radio_queue_pcm((const uint8_t *)mono,6,2));
    step(); assert(muted && queued==12); /* Wait for high watermark. */
    markers[0] = (radio_title_marker_t){.position = 0, .connection = 6, .title = "Old station"};
    markers[1] = (radio_title_marker_t){.position = 16, .connection = 7, .title = "Current title"};
    marker_count = 2;
    memset(queue,1,64); queued=64; step();
    assert(!radio_buffering && !muted && queued==48 && last_write[0]==1);
    assert(radio_pcm_position == 16 && marker_count == 1 && !radio_metadata.title[0]);
    step();
    assert(radio_pcm_position == 32 && !marker_count && radio_metadata_revision == 1);
    assert(!strcmp(radio_metadata.title, "Current title"));
    /* A new format discards old PCM before changing clocks, and stays silent. */
    assert(radio_prepare_output(44100, 7)==ESP_OK && !queued && muted && radio_buffering);
    radio_muted=true; memset(queue,2,64); queued=64; step();
    assert(muted && !radio_buffering); /* User mute survives buffer refill. */
    radio_discard_pcm(); assert(!queued && !radio_output_ready && muted);
    fail_rate=true;
    assert(radio_prepare_output(48000, 7)==ESP_FAIL && !radio_output_ready && muted);
    unsigned before=writes; radio_switch_requested=true; step(); assert(writes==before);
    radio_switch_requested=false; radio_stop_requested=true; step(); assert(writes==before);
    radio_stop_requested=false;
    radio_resume_requested=true; step(); assert(writes==before);
    radio_resume_requested=false; radio_buffering=true;
    assert(!radio_toggle_mute_locked() && !radio_muted && muted);
    puts("PASS: queue retirement before rate changes, mono frames, buffering/user mute, failed configuration and switch/stop admission");
}
'''
with tempfile.TemporaryDirectory(prefix='tanradio-pipeline-') as directory:
    path = Path(directory) / 'pipeline.c'
    path.write_text(stubs + production + checks)
    binary = Path(directory) / 'pipeline'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'main'),
                    str(path), str(ROOT / 'main/audio/pcm.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

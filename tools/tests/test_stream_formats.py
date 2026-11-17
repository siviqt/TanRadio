#!/usr/bin/env python3
"""Exercise the production byte probe, HLS parser and segment transport."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]

HTTP = r'''
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK 0
typedef struct response *esp_http_client_handle_t;
typedef void *http_event_handle_cb;
esp_err_t esp_http_client_get_url(esp_http_client_handle_t client, char *url, int capacity);
int esp_http_client_read(esp_http_client_handle_t client, char *buffer, int size);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t client);
bool esp_http_client_is_chunked_response(esp_http_client_handle_t client);
int64_t esp_http_client_get_content_length(esp_http_client_handle_t client);
void esp_http_client_close(esp_http_client_handle_t client);
'''

HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "radio/stream_format.h"
#include "radio/hls_manifest.h"
#include "radio/hls_stream.h"

static void check_probe(void)
{
    uint8_t mp3[421] = {0xff, 0xfb, 0x90, 0x64};
    memcpy(mp3 + 417, mp3, 4);
    assert(stream_format_probe(mp3, 417) == STREAM_UNKNOWN);
    assert(stream_format_probe(mp3, sizeof(mp3)) == STREAM_MP3);
    uint8_t tagged[433] = {'I','D','3', 4, 0, 0, 0, 0, 0, 2};
    memcpy(tagged + 12, mp3, sizeof(mp3));
    assert(stream_id3_size(tagged, 10) == 12 && stream_format_probe(tagged, sizeof(tagged)) == STREAM_MP3);
    uint8_t aac[22] = {0xff, 0xf1, 0x50, 0x80, 1, 0x7f, 0xfc};
    memcpy(aac + 11, aac, 7);
    assert(stream_format_probe(aac, sizeof(aac)) == STREAM_AAC);
    uint8_t ogg[60] = {'O', 'g', 'g', 'S', 0, 2};
    ogg[26] = 1; ogg[27] = 19;
    memcpy(ogg + 28, "OpusHead", 8);
    assert(stream_format_probe(ogg, 34) == STREAM_UNKNOWN);
    assert(stream_format_probe(ogg, sizeof(ogg)) == STREAM_OPUS);
    memcpy(ogg + 28, "\001vorbis", 8);
    assert(stream_format_probe(ogg, sizeof(ogg)) == STREAM_VORBIS);
    memset(ogg + 28, 0, 8);
    assert(stream_format_probe(ogg, sizeof(ogg)) == STREAM_UNSUPPORTED);
    uint8_t ts[377] = {0x47};
    ts[188] = ts[376] = 0x47;
    assert(stream_format_probe(ts, sizeof(ts)) == STREAM_TS);
    assert(stream_format_probe((const uint8_t *)"fLaC", 4) == STREAM_FLAC);
    assert(stream_format_probe((const uint8_t *)" \r\n<html>", 9) == STREAM_UNSUPPORTED);
    assert(stream_format_probe((const uint8_t *)"\xef\xbb\xbf#EXTM3U\n", 11) == STREAM_PLAYLIST);
    assert(stream_format_from_mime("audio/aacp; charset=UTF-8") == STREAM_AAC);
    assert(stream_format_from_codec("HE-AACv2") == STREAM_AAC);
}

static void check_manifest(void)
{
    hls_manifest_t manifest;
    char master[] = "#EXTM3U\r\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=1200000,RESOLUTION=640x360,CODECS=\"avc1.1,mp4a.40.2\"\r\nvideo.m3u8\r\n"
        "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"radio\",URI=\"other.m3u8\"\r\n"
        "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"radio\",DEFAULT=YES,URI=\"../audio/live.m3u8\"\r\n";
    assert(hls_parse_manifest(master, &manifest) == HLS_MANIFEST_MASTER);
    assert(!strcmp(manifest.variant, "../audio/live.m3u8"));
    char url[1024];
    const char *base = "https://example.org:8443/live/master.m3u8?first=1";
    assert(hls_resolve_uri(base, manifest.variant, url, sizeof(url)));
    assert(!strcmp(url, "https://example.org:8443/audio/live.m3u8"));
    assert(hls_resolve_uri(base, "//cdn.example.net/live", url, sizeof(url)));
    assert(!strcmp(url, "https://cdn.example.net/live"));
    assert(hls_resolve_uri(base, "./.././audio.ts?x=1#fragment", url, sizeof(url)));
    assert(!strcmp(url, "https://example.org:8443/audio.ts?x=1"));
    assert(hls_resolve_uri(base, "?second=2", url, sizeof(url)));
    assert(!strcmp(url, "https://example.org:8443/live/master.m3u8?second=2"));
    assert(!hls_resolve_uri(base, "file:///tmp/track", url, sizeof(url)));
    assert(!hls_resolve_uri(base, "audio.ts", url, 8));

    char live[] = "#EXTM3U\n#EXT-X-TARGETDURATION:4\n#EXT-X-MEDIA-SEQUENCE:17\n"
        "#EXTINF:4,\na.ts\n#EXT-X-DISCONTINUITY\n#EXTINF:3.9,\nb.ts\n#EXTINF:4,\nc.ts\n";
    assert(hls_parse_manifest(live, &manifest) == HLS_MANIFEST_MEDIA);
    assert(manifest.sequence == 17 && manifest.target_seconds == 4 && manifest.count == 3);
    assert(hls_start_segment(&manifest) == 1 && !strcmp(manifest.segments[1].uri, "b.ts"));
    char encrypted[] = "#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI=\"key.bin\"\n";
    char fmp4[] = "#EXTM3U\n#EXT-X-MAP:URI=\"init.mp4\"\n";
    char short_list[] = "#EXTM3U\n#EXT-X-TARGETDURATION:4\n#EXTINF:1,\n";
    assert(hls_parse_manifest(encrypted, &manifest) == HLS_MANIFEST_UNSUPPORTED);
    assert(hls_parse_manifest(fmp4, &manifest) == HLS_MANIFEST_UNSUPPORTED);
    assert(hls_parse_manifest(short_list, &manifest) == HLS_MANIFEST_INVALID);
}

struct response {
    const char *url;
    const uint8_t *data;
    size_t length, used;
    bool open;
};

static const char master[] = "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=32000,CODECS=\"mp4a.40.2\"\nlow/live.m3u8\n";
static const char first_manifest[] = "#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:10\n"
    "#EXTINF:2,\na.aac\n#EXTINF:2,\nb.aac\n";
static const char second_manifest[] = "#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:11\n"
    "#EXTINF:2,\nb.aac\n#EXTINF:2,\n../c.aac\n#EXT-X-ENDLIST\n";
static const uint8_t segment[] = {'I','D','3',4,0,0,0,0,0,2,0,0,0xff,0xf1,0x50,0x80,1,0x7f,0xfc,1,2,3,4};
static const char *const expected[] = {
    "https://radio.example/low/live.m3u8", "https://cdn.example/live/a.aac",
    "https://cdn.example/live/b.aac", "https://radio.example/low/live.m3u8",
    "https://cdn.example/c.aac",
};
static unsigned requests, sent;
static int64_t clock_us;
static struct response current;

esp_err_t esp_http_client_get_url(esp_http_client_handle_t client, char *url, int capacity)
{
    return snprintf(url, capacity, "%s", client->url) < capacity ? ESP_OK : 1;
}

int esp_http_client_read(esp_http_client_handle_t client, char *buffer, int size)
{
    assert(client->open && size > 0);
    size_t count = client->length - client->used;
    if (count > 5) count = 5;
    assert(count <= (size_t)size);
    memcpy(buffer, client->data + client->used, count);
    client->used += count;
    return (int)count;
}

bool esp_http_client_is_complete_data_received(esp_http_client_handle_t client)
{
    return client->length == client->used;
}

bool esp_http_client_is_chunked_response(esp_http_client_handle_t client) { (void)client; return false; }
int64_t esp_http_client_get_content_length(esp_http_client_handle_t client) { return client->length; }
void esp_http_client_close(esp_http_client_handle_t client) { client->open = false; }
int64_t esp_timer_get_time(void) { return clock_us; }
void vTaskDelay(unsigned ms) { clock_us += 1000 * ms; }

stream_http_result_t stream_http_open(esp_http_client_handle_t *client, const char *url,
                                      http_event_handle_cb events, void *headers, bool icy,
                                      stream_active_fn active, void *context)
{
    assert(!events && !headers && !icy && active(context) && !current.open);
    assert(requests < sizeof(expected) / sizeof(*expected) && !strcmp(url, expected[requests]));
    current = (struct response){.data = segment, .length = sizeof(segment), .url = url, .open = true};
    if (requests == 0 || requests == 3) {
        const char *text = requests == 0 ? first_manifest : second_manifest;
        current = (struct response){.data = (const uint8_t *)text, .length = strlen(text),
                                    .url = "https://cdn.example/live/redirect.m3u8", .open = true};
    }
    requests++;
    *client = &current;
    return STREAM_HTTP_OK;
}

void stream_http_close(esp_http_client_handle_t *client)
{
    if (*client) esp_http_client_close(*client);
    *client = NULL;
}

static bool active(void *context) { (void)context; return true; }
static bool sink(void *context, const uint8_t *bytes, size_t size)
{
    (void)context;
    for (size_t i = 0; i < size; i++)
        assert(bytes[i] == segment[12 + sent++ % (sizeof(segment) - 12)]);
    return true;
}

int main(void)
{
    check_probe();
    check_manifest();
    struct response initial = {.url = "https://radio.example/master.m3u8",
        .data = (const uint8_t *)master, .length = strlen(master), .used = 8, .open = true};
    uint8_t buffer[STREAM_HTTP_CHUNK];
    memcpy(buffer, master, initial.used);
    hls_stream_t *stream = hls_stream_create(); assert(stream);
    assert(hls_stream_run(stream, &initial, buffer, initial.used, active, sink, NULL) == HLS_STREAM_COMPLETE);
    assert(requests == 5 && sent == 3 * (sizeof(segment) - 12) && clock_us >= 1000000);
    hls_stream_destroy(stream);
    puts("PASS: stream probes, HLS URI resolution, bounded playlists, refresh, segment deduplication and split ID3 tags");
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix="tanradio-formats-") as directory:
        work = Path(directory)
        (work / "esp_http_client.h").write_text(HTTP)
        (work / "esp_heap_caps.h").write_text(
            "#include <stdlib.h>\n#define heap_caps_calloc(n,s,c) calloc(n,s)\n")
        (work / "esp_timer.h").write_text("#include <stdint.h>\nint64_t esp_timer_get_time(void);\n")
        (work / "freertos").mkdir()
        (work / "freertos/FreeRTOS.h").write_text("#define pdMS_TO_TICKS(ms) (ms)\n")
        (work / "freertos/task.h").write_text("void vTaskDelay(unsigned);\n")
        (work / "check.c").write_text(HARNESS)
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-I", str(work), "-I", str(ROOT / "main"),
                        str(work / "check.c"), str(ROOT / "main/radio/stream_format.c"),
                        str(ROOT / "main/radio/hls_manifest.c"), str(ROOT / "main/radio/hls_stream.c"),
                        "-o", str(work / "check")], check=True)
        subprocess.run([str(work / "check")], check=True, env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))


if __name__ == "__main__":
    main()

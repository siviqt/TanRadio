#!/usr/bin/env python3
"""Verify byte preservation and title handling across arbitrary read boundaries."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
CHECKS = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "radio/icy.h"

typedef struct {
    uint8_t audio[128];
    size_t size, title_count;
    uint64_t offsets[4];
    char titles[4][RADIO_TITLE_SIZE];
} received_t;

static bool audio(void *arg, const uint8_t *data, size_t size) {
    received_t *received = arg;
    assert(received->size + size <= sizeof(received->audio));
    memcpy(received->audio + received->size, data, size);
    received->size += size;
    return true;
}

static void title(void *arg, uint64_t position, const char *text) {
    received_t *received = arg;
    assert(received->title_count < 4);
    assert(position == received->size);
    size_t i = received->title_count++;
    received->offsets[i] = position;
    strcpy(received->titles[i], text);
}

static size_t append(uint8_t *out, const uint8_t *bytes, const char *metadata, unsigned padded) {
    memcpy(out, bytes, 7);
    out[7] = padded / 16;
    if (padded) {
        memset(out + 8, 0, padded);
        memcpy(out + 8, metadata, strlen(metadata));
    }
    return 8 + padded;
}

int main(void) {
    uint32_t interval = 0;
    assert(icy_parse_uint(" 16000\t", &interval) && interval == 16000);
    assert(icy_parse_uint("4294967295", &interval) && interval == UINT32_MAX);
    assert(!icy_parse_uint("4294967296", &interval));
    assert(!icy_parse_uint("-1", &interval) && icy_parse_uint("0", &interval) && interval == 0);
    assert(!icy_parse_uint("123;456", &interval) && !icy_parse_uint("", &interval));

    uint8_t expected[42], stream[4352];
    for (unsigned i = 0; i < sizeof(expected); i++) expected[i] = 255 - i * 7;
    size_t length = 0;
    length += append(stream + length, expected + 0, "StreamTitle='Guns N' Roses; Live';StreamUrl='foo';", 64);
    length += append(stream + length, expected + 7, "", 0);
    length += append(stream + length, expected + 14, "StreamTitle='missing closing quote", 48);
    length += append(stream + length, expected + 21, "StreamTitle='';", 16);
    length += append(stream + length, expected + 28, "StreamUrl='ignored';StreamTitle='Caf\xe9 \\'Live\\'';", ICY_BLOCK_SIZE);
    memcpy(stream + length, expected + 35, 7);
    length += 7;

    icy_filter_t filter;
    for (size_t chunk = 1; chunk <= 256; chunk++) {
        received_t received = {0};
        icy_filter_init(&filter, 7);
        for (size_t offset = 0; offset < length;) {
            size_t n = length - offset < chunk ? length - offset : chunk;
            assert(icy_filter_feed(&filter, stream + offset, n, audio, title, &received));
            offset += n;
        }
        assert(received.size == sizeof(expected) && !memcmp(expected, received.audio, sizeof(expected)));
        assert(received.title_count == 3);
        assert(received.offsets[0] == 7 && received.offsets[1] == 28 && received.offsets[2] == 35);
        assert(!strcmp(received.titles[0], "Guns N' Roses; Live"));
        assert(!strcmp(received.titles[1], ""));
        assert(!strcmp(received.titles[2], "Caf\xc3\xa9 'Live'"));
    }
    received_t plain = {0};
    assert(icy_filter_feed(NULL, expected, sizeof(expected), audio, title, &plain));
    assert(plain.size == sizeof(expected) && !memcmp(expected, plain.audio, plain.size) && !plain.title_count);

    char text[7];
    const uint8_t utf8[] = " \t\xd0\xa0\n\xd0\xbe\xd0\xba";
    icy_copy_text(text, sizeof(text), utf8, sizeof(utf8) - 1);
    assert(!strcmp(text, "\xd0\xa0 \xd0\xbe"));
    puts("PASS: ICY interval validation, byte-for-byte audio, split/max/empty/malformed blocks and text encoding");
}
'''

with tempfile.TemporaryDirectory(prefix="tanradio-icy-") as directory:
    work = Path(directory)
    (work / "check.c").write_text(CHECKS)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    "-I", str(ROOT / "main"), str(work / "check.c"),
                    str(ROOT / "main/radio/icy.c"), "-o", str(work / "check")], check=True)
    subprocess.run([str(work / "check")], check=True, env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))

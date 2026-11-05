#!/usr/bin/env python3
"""Exercise production BSP/codec gain functions against a register mock."""
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


driver = (ROOT / 'local-components/nicolaielectronics__es8156/es8156.c').read_text()
bsp = (ROOT / 'local-components/badgeteam__badge-bsp/targets/tanmatsu/badge_bsp_audio.c').read_text()
prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <stdio.h>
typedef int esp_err_t;
typedef void *es8156_handle_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_FAIL 2
#define portMAX_DELAY 0
static void *codec_handle = (void *)1, *codec_i2c_bus_semaphore = (void *)2;
static bool locked, fail;
static uint8_t reg;
static void xSemaphoreTake(void *s, int t) { (void)t; assert(s && !locked); locked = true; }
static void xSemaphoreGive(void *s) { assert(s && locked); locked = false; }
static int es8156_write_volume_control(void *h, uint8_t v) {
    assert(h && locked); if (fail) return ESP_FAIL; reg = v; return ESP_OK;
}
static int es8156_read_volume_control(void *h, uint8_t *v) {
    assert(h && locked); if (fail) return ESP_FAIL; *v = reg; return ESP_OK;
}
'''
defines = '\n'.join(line for source in (driver, bsp) for line in source.splitlines()
                    if line.startswith(('#define ES8156_VOLUME_DB_', '#define VOLUME_SCALING_FACTOR')))
production = '\n'.join(function(source, signature) for source, signature in (
    (driver, 'esp_err_t es8156_set_volume_percentage('),
    (driver, 'esp_err_t es8156_get_volume_percentage('),
    (bsp, 'esp_err_t bsp_audio_set_volume('),
    (bsp, 'esp_err_t bsp_audio_get_volume(')))
checks = r'''
int main(void) {
    unsigned previous = 0;
    for (unsigned p = 0; p <= 100; p++) {
        assert(bsp_audio_set_volume(p) == ESP_OK && !locked);
        assert(reg >= previous && reg <= 0xB3);
        previous = reg;
        float readback = -1;
        assert(bsp_audio_get_volume(&readback) == ESP_OK && !locked);
        assert(fabsf(readback - p) < 0.30f); /* half a register step */
    }
    assert(reg == 0xB3);
    assert(bsp_audio_set_volume(1000) == ESP_OK && reg == 0xB3);
    assert(bsp_audio_set_volume(-10) == ESP_OK && reg == 0);
    assert(bsp_audio_set_volume(NAN) == ESP_ERR_INVALID_ARG && !locked);
    assert(bsp_audio_set_volume(INFINITY) == ESP_ERR_INVALID_ARG && !locked);
    assert(bsp_audio_get_volume(NULL) == ESP_ERR_INVALID_ARG && !locked);
    fail = true;
    assert(bsp_audio_set_volume(50) == ESP_FAIL && !locked);
    float result = 123;
    assert(bsp_audio_get_volume(&result) == ESP_FAIL && result == 123 && !locked);
    puts("PASS: real BSP/driver gain mapping, maximum 0xB3, monotonicity, readback, bounds and I2C failures");
}
'''
with tempfile.TemporaryDirectory(prefix='tanradio-gain-') as directory:
    source = Path(directory) / 'gain.c'
    source.write_text(prefix + defines + '\n' + production + checks)
    binary = Path(directory) / 'gain'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(source),
                    '-lm', '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

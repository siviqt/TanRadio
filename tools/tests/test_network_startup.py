#!/usr/bin/env python3
"""Check real Wi-Fi startup ordering without blocking on saved-network retries."""
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
#define ESP_OK 0
#define ESP_FAIL -1
#define pdPASS 1
#define ESP_LOGE(tag, ...) ((void)(tag), (void)sizeof(printf(__VA_ARGS__)))
#define ESP_LOGW(tag, ...) ((void)(tag), (void)sizeof(printf(__VA_ARGS__)))
#define ESP_LOGI(tag, ...) ((void)(tag), (void)sizeof(printf(__VA_ARGS__)))
typedef struct { uint8_t ssid[32]; } wifi_ap_record_t;
static unsigned phase, connection_calls, disconnected, deleted;
static int failure;
static bool connected;
static int64_t ticks;
static void (*background)(void *);
static const char *esp_err_to_name(int error) { (void)error; return "mock error"; }
static int64_t esp_timer_get_time(void) { return ticks; }
static esp_err_t wifi_remote_initialize(void) {
    assert(phase++ == 0); ticks += 100;
    return failure == 1 ? ESP_FAIL : ESP_OK;
}
static esp_err_t wifi_connection_init_stack(void) {
    assert(phase++ == 1); ticks += 100;
    return failure == 2 ? ESP_FAIL : ESP_OK;
}
static int xTaskCreate(void (*task)(void *), const char *name, unsigned stack,
                       void *arg, unsigned priority, void *handle) {
    assert(phase++ == 2 && name && stack >= 4096 && priority && !arg && !handle);
    if (failure == 3) return 0;
    background = task; return pdPASS;
}
static esp_err_t wifi_connect_try_all(void) {
    connection_calls++; ticks += 30000000;
    connected = failure != 4;
    return connected ? ESP_OK : ESP_FAIL;
}
static bool wifi_connection_is_connected(void) { assert(phase >= 2); return connected; }
static void wifi_connection_disconnect(void) { disconnected++; connected = false; }
static esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *out) {
    *out = (wifi_ap_record_t){.ssid="Test network"}; return ESP_OK;
}
static void vTaskDelete(void *task) { assert(!task); deleted++; }
'''
HARNESS = r'''
#include "main/network/network.c"
int main(void) {
    char ssid[32];
    assert(!network_is_connected() && !network_get_ssid(ssid, sizeof(ssid)));
    network_shutdown(); assert(!disconnected);
    for (failure = 0; failure <= 4; failure++) {
        phase = connection_calls = disconnected = deleted = 0;
        ticks = 0; background = NULL; connected = false;
        atomic_store(&network_stack_ready, false);
        network_init();
        /* The simulated 30-second connection never runs on startup's caller. */
        assert(ticks <= 200 && !connection_calls && !network_is_connected());
        if (failure == 1 || failure == 2 || failure == 3) {
            assert(!background); continue;
        }
        assert(background && phase == 3);
        background(NULL);
        assert(connection_calls == 1 && deleted == 1 && ticks == 30000200);
        assert(network_is_connected() == (failure != 4));
        if (connected) {
            assert(network_get_ssid(ssid, sizeof(ssid)) && !strcmp(ssid, "Test network"));
            char small[5]; assert(network_get_ssid(small, sizeof(small)) && !strcmp(small, "Test"));
        } else assert(!network_get_ssid(ssid, sizeof(ssid)));
        network_shutdown(); assert(disconnected == 1 && !network_is_connected());
    }
    puts("PASS: transport-before-SD preparation, nonblocking connection task, readiness guards, failure paths and SSID bounds");
}
'''


def main():
    # A warm slave can retain the previous host's SDIO counters and cause an
    # impossible internal-DMA allocation before the RPC handshake completes.
    for name in ['sdkconfig_tanmatsu', 'sdkconfigs/tanmatsu']:
        config = (ROOT / name).read_text().splitlines()
        assert 'CONFIG_ESP_HOSTED_SLAVE_RESET_ON_EVERY_HOST_BOOTUP=y' in config
        assert 'CONFIG_ESP_HOSTED_SLAVE_RESET_ONLY_IF_NECESSARY=y' not in config
    with tempfile.TemporaryDirectory(prefix='tanradio-network-') as directory:
        work = Path(directory)
        (work / 'runtime.h').write_text(STUBS)
        for name in ['esp_log.h', 'esp_wifi.h', 'esp_timer.h', 'wifi_connection.h',
                     'wifi_remote.h', 'freertos/FreeRTOS.h', 'freertos/task.h']:
            target = work / name
            target.parent.mkdir(exist_ok=True)
            target.write_text('#include "runtime.h"\n')
        (work / 'check.c').write_text(HARNESS)
        subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                        '-I', str(work), '-I', str(ROOT),
                        str(work / 'check.c'), '-o', str(work / 'check')], check=True)
        subprocess.run([str(work / 'check')], check=True)


if __name__ == '__main__':
    main()

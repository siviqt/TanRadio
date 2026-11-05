#!/usr/bin/env python3
"""Exercise the production SD adapter, shared-host cleanup, VCOM and DSI reads."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

ROOT = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[2]
BSP = ROOT / "local-components/badgeteam__badge-bsp/targets/tanmatsu"


def function(path, signature):
    source = path.read_text()
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL 1
#define ESP_ERR_INVALID_ARG 2
#define ESP_ERR_NOT_FOUND 3
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_RETURN_ON_ERROR(expr, ...) do { int err = (expr); if (err) return err; } while (0)
#define BSP_STORAGE_TYPE_SDCARD 1
static bool mounted, auto_format;
static unsigned mount_calls;
static int query_result, mount_result;
static int bsp_storage_get_mounted(int type, bool *out) {
    assert(type == BSP_STORAGE_TYPE_SDCARD);
    if (!query_result) *out = mounted;
    return query_result;
}
static int bsp_storage_mount_advanced(int type, const char *point, int files, bool format) {
    assert(type == BSP_STORAGE_TYPE_SDCARD && !strcmp(point, TANRADIO_SD_MOUNT_POINT));
    assert(files == 4); auto_format = format; mount_calls++;
    if (!mount_result) mounted = true;
    return mount_result;
}
#define CONFIG_ESP_HOSTED_SDIO_SLOT 1
#define SD_IO_RW_DIRECT 52
#define SCF_CMD_AC 1
#define SCF_RSP_R5 2
typedef struct { unsigned opcode, arg, flags, timeout_ms; } sdmmc_command_t;
static unsigned transactions, deinitializations;
static int transaction_result, deinit_result;
static int sdmmc_host_do_transaction(int slot, sdmmc_command_t *cmd) {
    assert(slot == CONFIG_ESP_HOSTED_SDIO_SLOT);
    assert(cmd->opcode == 52 && cmd->arg == 0 && cmd->timeout_ms == 1000);
    assert(cmd->flags == (SCF_CMD_AC | SCF_RSP_R5));
    transactions++;
    return transaction_result;
}
static int sdmmc_host_deinit_slot(int slot) {
    assert(slot == 0 || slot == CONFIG_ESP_HOSTED_SDIO_SLOT);
    deinitializations++;
    return deinit_result;
}
typedef int nvs_handle_t;
#define NVS_READONLY 0
static const char *NVS_NAMESPACE = "system";
static int open_result, get_result, set_result, flush_result;
static unsigned closes, sets, flushes;
static uint8_t saved_vcom = 0x3c, applied_vcom;
static int nvs_open(const char *space, int mode, nvs_handle_t *handle) {
    assert(!strcmp(space, "system") && mode == NVS_READONLY);
    *handle = 7; return open_result;
}
static int nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *value) {
    assert(handle == 7 && !strcmp(key, "lcd_vcom"));
    if (!get_result) *value = saved_vcom;
    return get_result;
}
static void nvs_close(nvs_handle_t handle) { assert(handle == 7); closes++; }
static int bsp_display_get_vcom_default(uint8_t *out) { *out = 0x20; return ESP_OK; }
static int st7701_set_vcom(uint8_t value) { sets++; applied_vcom = value; return set_result; }
typedef struct { unsigned num_fbs; } bsp_display_configuration_t;
static bool bsp_display_initialized;
static const bsp_display_configuration_t *expected_configuration;
static int bsp_display_enable_dsi_phy_power(void) { return ESP_OK; }
static int bsp_display_initialize_panel(const bsp_display_configuration_t *configuration) {
    assert(configuration == expected_configuration); return ESP_OK;
}
static int bsp_display_initialize_flush(const bsp_display_configuration_t *configuration) {
    assert(configuration == expected_configuration && configuration->num_fbs == 2);
    flushes++; return flush_result;
}
static int bsp_display_initialize_te(void) { return ESP_OK; }
'''

definitions = "\n".join(line for line in (ROOT / "main/storage/sdcard.h").read_text().splitlines()
                         if line.startswith("#define TANRADIO_SD_MOUNT_POINT"))
production = "\n".join(function(path, signature) for path, signature in (
    (ROOT / "main/storage/sdcard.c", "esp_err_t tanradio_sd_mount("),
    (BSP / "badge_bsp_storage.c", "static esp_err_t sdmmc_host_deinit_slot_hosted("),
    (BSP / "badge_bsp_display.c", "static esp_err_t bsp_display_nvs_settings_get_u8("),
    (BSP / "badge_bsp_display.c", "esp_err_t bsp_display_initialize("),
))

checks = r'''
int main(void) {
    query_result = ESP_FAIL;
    assert(tanradio_sd_mount() == ESP_FAIL && mount_calls == 0);
    query_result = ESP_OK; mount_result = ESP_FAIL;
    assert(tanradio_sd_mount() == ESP_FAIL && mount_calls == 1 && !mounted && !auto_format);
    mount_result = ESP_OK;
    assert(tanradio_sd_mount() == ESP_OK && mount_calls == 2 && mounted && !auto_format);
    assert(tanradio_sd_mount() == ESP_OK && mount_calls == 2);

    assert(sdmmc_host_deinit_slot_hosted(0) == ESP_OK);
    assert(transactions == 1 && deinitializations == 1);
    transaction_result = ESP_FAIL; deinit_result = ESP_ERR_INVALID_ARG;
    assert(sdmmc_host_deinit_slot_hosted(0) == ESP_ERR_INVALID_ARG);
    assert(transactions == 2 && deinitializations == 2);
    assert(sdmmc_host_deinit_slot_hosted(CONFIG_ESP_HOSTED_SDIO_SLOT) == ESP_ERR_INVALID_ARG);
    assert(transactions == 2 && deinitializations == 3);

    const bsp_display_configuration_t configuration = { .num_fbs = 2 };
    expected_configuration = &configuration;
    assert(bsp_display_nvs_settings_get_u8(NULL, 0, &applied_vcom) == ESP_ERR_INVALID_ARG);
    assert(bsp_display_nvs_settings_get_u8("lcd_vcom", 0, NULL) == ESP_ERR_INVALID_ARG);
    open_result = ESP_ERR_NOT_FOUND;
    assert(bsp_display_initialize(&configuration) == ESP_OK && sets == 0 && closes == 0);
    bsp_display_initialized = false; open_result = ESP_OK; get_result = ESP_ERR_NOT_FOUND;
    assert(bsp_display_initialize(&configuration) == ESP_OK && sets == 0 && closes == 1);
    bsp_display_initialized = false; get_result = ESP_OK;
    assert(bsp_display_initialize(&configuration) == ESP_OK && sets == 1 && closes == 2);
    assert(applied_vcom == saved_vcom && flushes == 3);
    assert(bsp_display_initialize(&configuration) == ESP_OK && sets == 1 && flushes == 3);
    bsp_display_initialized = false; flush_result = ESP_FAIL;
    assert(bsp_display_initialize(&configuration) == ESP_FAIL && !bsp_display_initialized);
    flush_result = ESP_OK; set_result = ESP_FAIL;
    assert(bsp_display_initialize(&configuration) == ESP_OK && bsp_display_initialized);
    puts("PASS: BSP SD mount errors/retry/idempotence/no formatting; shared-host cleanup; saved/missing VCOM and native flush setup");
}
'''

with tempfile.TemporaryDirectory(prefix="tanradio-bsp-integration-") as directory:
    source = Path(directory) / "integration.c"
    source.write_text(definitions + "\n" + prefix + production + checks)
    binary = source.with_suffix("")
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    str(source), "-o", str(binary)], check=True)
    # LeakSanitizer cannot inspect processes in the restricted build sandbox;
    # keep AddressSanitizer and UndefinedBehaviorSanitizer active, as the
    # existing TanRadio harnesses do. These extracted functions do not allocate.
    subprocess.run([str(binary)], check=True,
                   env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))

hal = ROOT / "local-components/esp_hal_lcd/mipi_dsi_hal.c"
hal_prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#define MIPI_DSI_READ_TIMEOUT_US 100000U
#define MIPI_DSI_DT_SET_MAXIMUM_RETURN_PKT 0x37
typedef unsigned mipi_dsi_data_type_t;
typedef struct { void *host; } mipi_dsi_hal_context_t;
static bool mipi_dsi_read_timed_out;
static unsigned mode, elapsed, headers, words;
static void esp_rom_delay_us(unsigned us) { assert(us == 1); elapsed++; }
static bool mipi_dsi_host_ll_gen_is_cmd_fifo_full(void *host) { assert(host); return mode == 1; }
static bool mipi_dsi_host_ll_gen_is_read_cmd_busy(void *host) {
    assert(host); return mode == 2 || (mode == 5 && elapsed < 7);
}
static bool mipi_dsi_host_ll_gen_is_read_fifo_empty(void *host) {
    assert(host); return mode == 3 || (mode == 5 && elapsed < 12) || (mode != 4 && words == 0);
}
static void mipi_dsi_host_ll_gen_set_packet_header(void *host, unsigned vc, unsigned dt,
                                                 unsigned msb, unsigned lsb) {
    assert(host && vc == 0 && msb == 0);
    if (headers++ == 0) assert(dt == 0x37 && (lsb == 1 || lsb == 7));
    else assert(headers == 2 && dt == 0x06 && lsb == 0xb1);
}
static void mipi_dsi_host_ll_enable_video_mode(void *host, bool enable) { assert(host && !enable); }
static void mipi_dsi_host_ll_enable_bta(void *host, bool enable) { assert(host && enable); }
static void mipi_dsi_host_ll_gen_set_rx_vcid(void *host, unsigned vc) { assert(host && vc == 0); }
static uint32_t mipi_dsi_host_ll_gen_read_payload_fifo(void *host) {
    assert(host); if (mode == 4) return 0;
    assert(words); return words-- == 2 ? 0x44332211 : 0x88776655;
}
'''
hal_production = "\n".join(function(hal, signature) for signature in (
    "bool mipi_dsi_hal_host_gen_read_timed_out(",
    "static bool mipi_dsi_hal_read_poll(",
    "void mipi_dsi_hal_host_gen_read_short_packet(",
))
hal_checks = r'''
int main(void) {
    mipi_dsi_hal_context_t context = { .host = (void *)1 };
    uint8_t output[9]; memset(output, 0xaa, sizeof(output));
    words = 2;
    mipi_dsi_hal_host_gen_read_short_packet(&context, 0, 0x06, 0xb1, output, 7);
    const uint8_t expected[] = {0x11,0x22,0x33,0x44,0x55,0x66,0x77,0xaa,0xaa};
    assert(!memcmp(output, expected, sizeof(output)) && !mipi_dsi_hal_host_gen_read_timed_out());
    for (mode = 1; mode <= 4; mode++) {
        elapsed = headers = 0; words = 1; memset(output, 0xaa, sizeof(output));
        mipi_dsi_hal_host_gen_read_short_packet(&context, 0, 0x06, 0xb1, output, 1);
        assert(mipi_dsi_hal_host_gen_read_timed_out() && elapsed == MIPI_DSI_READ_TIMEOUT_US);
        assert(output[0] == 0 && output[1] == 0xaa);
    }
    mode = 5; elapsed = headers = 0; words = 1;
    mipi_dsi_hal_host_gen_read_short_packet(&context, 0, 0x06, 0xb1, output, 1);
    assert(!mipi_dsi_hal_host_gen_read_timed_out() && output[0] == 0x55 && elapsed == 13);
    mipi_dsi_hal_host_gen_read_short_packet(&context, 0, 0x06, 0xb1, NULL, 0);
    assert(!mipi_dsi_hal_host_gen_read_timed_out());
    puts("PASS: actual DSI HAL read payload/bounds, command/BTA/FIFO timeouts and successful read after timeout");
}
'''
with tempfile.TemporaryDirectory(prefix="tanradio-dsi-timeout-") as directory:
    source = Path(directory) / "timeout.c"
    source.write_text(hal_prefix + hal_production + hal_checks)
    binary = source.with_suffix("")
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True,
                   env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))

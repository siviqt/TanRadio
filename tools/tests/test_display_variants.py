#!/usr/bin/env python3
"""Run the real LCD driver and BSP restoration against a panel/NVS mock."""
from pathlib import Path
import json
import os
import subprocess
import tempfile

from test_display_updates import function

ROOT = Path(__file__).resolve().parents[2]
LCD = ROOT / "local-components/nicolaielectronics__mipi_dsi_abstraction"
BSP = ROOT / "local-components/badgeteam__badge-bsp/targets/tanmatsu/badge_bsp_display.c"

HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int esp_err_t;
typedef int gpio_num_t;
typedef void *esp_lcd_panel_handle_t;
typedef void *esp_lcd_panel_io_handle_t;
typedef void *esp_lcd_dsi_bus_handle_t;
typedef int lcd_color_format_t;
#define ESP_OK 0
#define ESP_FAIL 1
#define ESP_ERR_INVALID_ARG 2
#define ESP_ERR_INVALID_STATE 3
#define ESP_ERR_NOT_FOUND 4
#define LCD_COLOR_FMT_RGB565 16
#define LCD_COLOR_FMT_RGB888 24
#define MIPI_DSI_DPI_CLK_SRC_DEFAULT 0
#define LCD_RGB_ELEMENT_ORDER_RGB 0
#define LCD_CMD_NORON 0x13
#define LCD_CMD_SLPOUT 0x11
#define LCD_CMD_DISPON 0x29
#define LCD_CMD_MADCTL 0x36
#define LCD_CMD_RDDID 0x04
#define LCD_CMD_COLMOD 0x3a
#define CONFIG_BSP_TARGET_TANMATSU 1
#define CONFIG_MIPI_DSI_ABSTRACTION_USE_VENDORED_ESP_HAL_LCD 1
#define ESP_RETURN_ON_ERROR(expr, ...) do { int err = (expr); if (err) return err; } while (0)
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))
#define pdMS_TO_TICKS(ms) (ms)
static void vTaskDelay(unsigned ticks) { (void)ticks; }
typedef struct { unsigned bus_id, num_data_lanes, lane_bit_rate_mbps; } esp_lcd_dsi_bus_config_t;
typedef struct { unsigned virtual_channel, lcd_cmd_bits, lcd_param_bits; } esp_lcd_dbi_io_config_t;
typedef struct {
    unsigned virtual_channel, dpi_clk_src, dpi_clock_freq_mhz, in_color_format, num_fbs;
    struct { unsigned h_size, v_size, hsync_back_porch, hsync_pulse_width, hsync_front_porch,
             vsync_back_porch, vsync_pulse_width, vsync_front_porch; } video_timing;
} esp_lcd_dpi_panel_config_t;
typedef struct { int cmd; const void *data; size_t data_bytes; unsigned delay_ms; } st7701_lcd_init_cmd_t;
typedef struct {
    const st7701_lcd_init_cmd_t *init_cmds;
    size_t init_cmds_size;
    struct { void *dsi_bus; const esp_lcd_dpi_panel_config_t *dpi_config; } mipi_config;
    struct { bool use_mipi_interface; } flags;
} st7701_vendor_config_t;
typedef struct {
    int reset_gpio_num, rgb_ele_order, bits_per_pixel;
    const st7701_vendor_config_t *vendor_config;
} esp_lcd_panel_dev_config_t;
#include "dsi_panel_nicolaielectronics_st7701.h"

static const char *fault;
static uint8_t panel_id[3] = {0x77, 0x01, 0x00}, hardware_vcom, saved_vcom, command_bank;
static const uint8_t factory_vcom = 0x21;
static bool saved, timed_out;
static unsigned id_reads, otp_reads, vcom_writes, nvs_reads, panel_creations, panel_inits, flushes;
static unsigned sequence_hash = 2166136261u, tx_hash = 2166136261u, tx_count;
static const st7701_lcd_init_cmd_t *selected_sequence;
static size_t selected_count;
static bool fails(const char *name) { return !strcmp(fault, name); }
static void hash_byte(unsigned *hash, unsigned value) { *hash = (*hash ^ (uint8_t)value) * 16777619u; }
static bool mipi_dsi_hal_host_gen_read_timed_out(void) { return timed_out; }
static int esp_lcd_new_dsi_bus(const esp_lcd_dsi_bus_config_t *config, void **handle) {
    assert(config->bus_id == 0 && config->num_data_lanes == 2 && config->lane_bit_rate_mbps == 500);
    *handle = (void *)1; return ESP_OK;
}
static int esp_lcd_new_panel_io_dbi(void *bus, const esp_lcd_dbi_io_config_t *config, void **handle) {
    assert(bus && config->lcd_cmd_bits == 8 && config->lcd_param_bits == 8);
    *handle = (void *)2; return ESP_OK;
}
static int esp_lcd_panel_io_rx_param(void *io, int cmd, void *output, size_t size) {
    assert(io); timed_out = false;
    if (cmd == LCD_CMD_RDDID) {
        assert(size == 3); id_reads++;
        if (fails("id-error")) return ESP_FAIL;
        timed_out = fails("id-timeout");
        memcpy(output, panel_id, size);
    } else {
        assert(cmd == 0xb1 && command_bank == 0x11 && size == 1); otp_reads++;
        if (fails("otp-error")) return ESP_FAIL;
        timed_out = fails("otp-timeout");
        *(uint8_t *)output = factory_vcom;
    }
    return ESP_OK;
}
static int esp_lcd_panel_io_tx_param(void *io, int cmd, const void *data, size_t size) {
    assert(io); const uint8_t *bytes = data;
    tx_count++; hash_byte(&tx_hash, cmd); hash_byte(&tx_hash, size);
    for (size_t i = 0; i < size; i++) hash_byte(&tx_hash, bytes[i]);
    if (cmd == 0xff && size == 5) command_bank = bytes[4];
    if (cmd == 0xb1 && command_bank == 0x11 && size == 1) {
        if (fails("set-error") && panel_inits) return ESP_FAIL;
        hardware_vcom = bytes[0]; vcom_writes++;
    }
    return ESP_OK;
}
static int esp_lcd_new_panel_st7701(void *io, const esp_lcd_panel_dev_config_t *config, void **panel) {
    assert(io && config->bits_per_pixel == 16);
    const st7701_vendor_config_t *vendor = config->vendor_config;
    const esp_lcd_dpi_panel_config_t *dpi = vendor->mipi_config.dpi_config;
    assert(vendor->flags.use_mipi_interface && vendor->mipi_config.dsi_bus);
    assert(dpi->num_fbs == 2 && dpi->in_color_format == LCD_COLOR_FMT_RGB565);
    assert(dpi->dpi_clock_freq_mhz == 30 && dpi->video_timing.h_size == 480 && dpi->video_timing.v_size == 800);
    selected_sequence = vendor->init_cmds; selected_count = vendor->init_cmds_size;
    *panel = (void *)3; panel_creations++; return ESP_OK;
}
static int esp_lcd_dpi_panel_enable_dma2d(void *panel) { assert(panel); return ESP_OK; }
static int esp_lcd_dpi_panel_disable_dma2d(void *panel) { assert(panel); return ESP_OK; }
static int esp_lcd_panel_del(void *panel) { assert(panel); return ESP_OK; }
static int esp_lcd_panel_reset(void *panel) {
    assert(panel); command_bank = 0; hardware_vcom = factory_vcom; return ESP_OK;
}
static int esp_lcd_panel_init(void *panel) {
    assert(panel);
    if (fails("panel-error")) return ESP_FAIL;
    for (size_t i = 0; i < selected_count; i++) {
        const st7701_lcd_init_cmd_t *cmd = &selected_sequence[i];
        const uint8_t *data = cmd->data;
        hash_byte(&sequence_hash, cmd->cmd); hash_byte(&sequence_hash, cmd->data_bytes);
        hash_byte(&sequence_hash, cmd->delay_ms);
        for (size_t j = 0; j < cmd->data_bytes; j++) hash_byte(&sequence_hash, data[j]);
        assert(esp_lcd_panel_io_tx_param((void *)2, cmd->cmd, cmd->data, cmd->data_bytes) == ESP_OK);
    }
    panel_inits++; return ESP_OK;
}

typedef int nvs_handle_t;
#define NVS_READONLY 0
static const char *NVS_NAMESPACE = "system";
static int nvs_open(const char *space, int mode, nvs_handle_t *handle) {
    assert(!strcmp(space, "system") && mode == NVS_READONLY); nvs_reads++;
    *handle = 7; return saved && !fails("nvs-open-error") ? ESP_OK : ESP_ERR_NOT_FOUND;
}
static int nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *value) {
    assert(handle == 7 && !strcmp(key, "lcd_vcom"));
    if (fails("nvs-read-error")) return ESP_FAIL;
    *value = saved_vcom; return ESP_OK;
}
static void nvs_close(nvs_handle_t handle) { assert(handle == 7); }
typedef struct { unsigned num_fbs; } bsp_display_configuration_t;
static bool bsp_display_initialized;
static int bsp_display_enable_dsi_phy_power(void) { return ESP_OK; }
static int bsp_display_initialize_panel(const bsp_display_configuration_t *config) {
    const st7701_configuration_t lcd = { .reset_pin = 0, .num_fbs = config->num_fbs };
    return st7701_initialize(&lcd);
}
static int bsp_display_initialize_flush(const bsp_display_configuration_t *config) {
    assert(config->num_fbs == 2); flushes++; return ESP_OK;
}
static int bsp_display_initialize_te(void) { return ESP_OK; }
'''

CHECKS = r'''
int main(int argc, char **argv) {
    assert(argc == 4); fault = argv[3];
    bool variant_2 = !strcmp(argv[1], "v2");
    if (variant_2) memcpy(panel_id, (uint8_t[]){0x42, 0x00, 0x01}, 3);
    if (!strcmp(argv[1], "near-id0")) memcpy(panel_id, (uint8_t[]){0x43, 0x00, 0x01}, 3);
    if (!strcmp(argv[1], "near-id1")) memcpy(panel_id, (uint8_t[]){0x42, 0x01, 0x01}, 3);
    if (!strcmp(argv[1], "near-id2")) memcpy(panel_id, (uint8_t[]){0x42, 0x00, 0x00}, 3);
    saved = strcmp(argv[2], "none") != 0;
    saved_vcom = saved ? (uint8_t)strtoul(argv[2], NULL, 0) : 0;
    uint8_t id[3] = {0xaa, 0xaa, 0xaa}, value;
    assert(st7701_get_id(NULL) == ESP_ERR_INVALID_ARG);
    assert(st7701_get_id(id) == ESP_ERR_INVALID_STATE && id[0] == 0xaa);
    assert(st7701_get_vcom(NULL) == ESP_ERR_INVALID_ARG);
    assert(st7701_get_vcom_otp(&value) == ESP_ERR_INVALID_STATE);
    assert(st7701_set_vcom(0x33) == ESP_ERR_INVALID_STATE);
    char label[40]; about_display_value(label, sizeof(label));
    assert(!strcmp(label, "Unknown"));

    const bsp_display_configuration_t config = { .num_fbs = 2 };
    int result = bsp_display_initialize(&config);
    assert(id_reads == 1);
    if (fails("id-timeout") || fails("otp-timeout") || fails("panel-error")) {
        assert(result == (fails("panel-error") ? ESP_FAIL : ESP_ERR_NOT_FOUND));
        assert(!bsp_display_initialized && !flushes && !nvs_reads && !vcom_writes);
        if (fails("id-timeout")) assert(!otp_reads && !panel_creations);
        else assert(otp_reads == 1 && panel_creations == 1);
        puts("PASS: failed panel initialization propagates without calibration or flush setup");
        return 0;
    }
    assert(result == ESP_OK && bsp_display_initialized && flushes == 1);
    bool selected_2 = variant_2 && !fails("id-error");
    assert(st7701_is_display_variant_2() == selected_2);
    unsigned default_value = selected_2 ? 0x2e : 0x2d;
    bool restored = saved && !fails("otp-error") && !fails("nvs-open-error") &&
                    !fails("nvs-read-error") && !fails("set-error");
    unsigned expected = restored ? saved_vcom : default_value;
    assert(hardware_vcom == expected);
    assert(st7701_get_vcom(&value) == ESP_OK && value == expected);
    assert(vcom_writes == (restored ? 2u : 1u));
    assert(otp_reads == 1 && panel_inits == 1 && panel_creations == 1);
    assert(nvs_reads == (fails("otp-error") ? 0u : 1u));
    if (fails("otp-error")) assert(st7701_get_vcom_otp(&value) == ESP_ERR_INVALID_STATE);
    else assert(st7701_get_vcom_otp(&value) == ESP_OK && value == factory_vcom);
    about_display_value(label, sizeof(label));
    if (fails("id-error")) {
        assert(st7701_get_id(id) == ESP_ERR_INVALID_STATE && !strcmp(label, "Unknown"));
    } else {
        assert(st7701_get_id(id) == ESP_OK && !memcmp(id, panel_id, sizeof(id)));
        char expected_label[40];
        snprintf(expected_label, sizeof(expected_label), "v%u (ID %02X %02X %02X)",
                 selected_2 ? 2u : 1u, (unsigned)panel_id[0], (unsigned)panel_id[1], (unsigned)panel_id[2]);
        assert(!strcmp(label, expected_label));
    }
    unsigned writes = vcom_writes, previous_tx = tx_count;
    assert(bsp_display_initialize(&config) == ESP_OK && vcom_writes == writes && tx_count == previous_tx);
    assert(id_reads == 1 && flushes == 1);
    printf("sequence=%08x tx=%08x commands=%u VCOM=%02x writes=%u\n",
           sequence_hash, tx_hash, tx_count, hardware_vcom, vcom_writes);
}
'''


def main():
    metadata = json.loads((ROOT / "metadata.json").read_text())
    assert metadata["application"][0]["bsp_version"] == 2
    for config in (ROOT / "sdkconfig_tanmatsu", ROOT / "sdkconfigs/tanmatsu"):
        assert "CONFIG_MIPI_DSI_ABSTRACTION_USE_VENDORED_ESP_HAL_LCD=y" in config.read_text().splitlines()
    restoration = "\n".join(function(BSP, name) for name in (
        "esp_err_t bsp_display_get_vcom_default(",
        "static esp_err_t bsp_display_nvs_settings_get_u8(",
        "esp_err_t bsp_display_initialize(",
    ))
    diagnostics = function(ROOT / "main/ui/about_view.c", "static void about_display_value(")
    includes = ("driver/gpio.h", "esp_check.h", "esp_err.h", "esp_lcd_mipi_dsi.h",
                "esp_lcd_panel_commands.h", "esp_lcd_panel_io.h", "esp_lcd_panel_ops.h",
                "esp_lcd_st7701.h", "esp_ldo_regulator.h", "esp_log.h", "esp_timer.h",
                "freertos/FreeRTOS.h", "freertos/semphr.h", "freertos/task.h", "sdkconfig.h",
                "hal/mipi_dsi_hal.h")
    with tempfile.TemporaryDirectory(prefix="tanradio-display-variants-") as directory:
        work = Path(directory)
        for name in includes:
            path = work / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("#pragma once\n")
        source = work / "variants.c"
        driver = (LCD / "dsi_panel_nicolaielectronics_st7701.c").read_text()
        source.write_text(HARNESS + driver + restoration + diagnostics + CHECKS)
        binary = work / "variants"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                        "-I", str(work), "-I", str(LCD / "include"),
                        str(source), "-o", str(binary)], check=True)
        cases = [(variant, saved, "ok") for variant in ("v1", "v2")
                 for saved in ("none", "0x35", "0x2d", "0x2e", "0", "255")]
        cases += [(variant, "0x35", fault) for variant in ("v1", "v2")
                  for fault in ("id-error", "id-timeout", "otp-error", "otp-timeout",
                                "panel-error", "nvs-open-error", "nvs-read-error", "set-error")]
        cases += [(variant, "none", "ok") for variant in ("near-id0", "near-id1", "near-id2")]
        for case in cases:
            result = subprocess.run([str(binary), *case], check=True, capture_output=True, text=True,
                                    env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))
            assert result.stdout.strip()
    print(f"PASS: {len(cases)} real-driver/BSP cases: v1/v2 defaults and saved values, exact ID selection, "
          "ID/OTP errors and timeouts, init failure, read-only calibration and About diagnostics")


if __name__ == "__main__":
    main()

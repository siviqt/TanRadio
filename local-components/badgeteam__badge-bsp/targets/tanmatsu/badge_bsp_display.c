// Board support package API: Tanmatsu implementation
// SPDX-FileCopyrightText: 2024-2025 Nicolai Electronics
// SPDX-FileCopyrightText: 2024 Orange-Murker
// SPDX-License-Identifier: MIT

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "bsp/device.h"
#include "bsp/display.h"
#include "bsp/tanmatsu.h"
#include "driver/gpio.h"
#include "dsi_panel_nicolaielectronics_st7701.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"
#include "hal/gpio_types.h"
#include "hal/lcd_types.h"
#include "tanmatsu_coprocessor.h"
#include "tanmatsu_hardware.h"
#include "nvs.h"

static char const* TAG = "BSP display";
static char const* NVS_NAMESPACE = "system";

static esp_ldo_channel_handle_t ldo_mipi_phy            = NULL;
static bool                     bsp_display_initialized = false;
static bsp_display_te_mode_t    display_te_mode         = BSP_DISPLAY_TE_DISABLED;
static SemaphoreHandle_t        te_semaphore            = NULL;
static SemaphoreHandle_t        flush_semaphore         = NULL;
static SemaphoreHandle_t        present_mutex           = NULL;
static void*                    display_framebuffers[2] = {0};
static size_t                   display_width, display_height, display_pixel_bytes;
static unsigned                 display_front_index;
static uint32_t                 display_refresh_count, display_flip_start;
static bool                     display_flip_pending;

IRAM_ATTR static bool bsp_display_refresh_ready(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t* edata,
                                                void* user_ctx) {
    __atomic_add_fetch(&display_refresh_count, 1, __ATOMIC_RELEASE);
    return false;
}

IRAM_ATTR static bool bsp_display_flush_ready(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t* edata,
                                              void* user_ctx) {
    xSemaphoreGiveFromISR(flush_semaphore, NULL);
    return false;
}

static esp_err_t bsp_display_enable_dsi_phy_power(void) {
    if (ldo_mipi_phy != NULL) {
        return ESP_OK;
    }
    esp_ldo_channel_config_t ldo_mipi_phy_config = {
        .chan_id    = BSP_DSI_LDO_CHAN,
        .voltage_mv = BSP_DSI_LDO_VOLTAGE_MV,
    };
    return esp_ldo_acquire_channel(&ldo_mipi_phy_config, &ldo_mipi_phy);
}

static esp_err_t bsp_display_initialize_panel(const bsp_display_configuration_t* configuration) {
    st7701_configuration_t config = {
        .reset_pin = BSP_LCD_RESET_PIN,
        .use_24_bit_color =
            configuration ? (configuration->requested_color_format == BSP_DISPLAY_COLOR_FORMAT_24_888RGB) : false,
        .num_fbs = configuration ? configuration->num_fbs : 1,
    };

    return st7701_initialize(&config);
}

IRAM_ATTR static void te_gpio_interrupt_handler(void* pvParameters) {
    xSemaphoreGiveFromISR(te_semaphore, NULL);
}

static esp_err_t bsp_display_initialize_flush(const bsp_display_configuration_t* configuration) {
    if (configuration && configuration->num_fbs >= 2) {
        lcd_color_format_t format;
        ESP_RETURN_ON_ERROR(st7701_get_parameters(&display_width, &display_height, &format), TAG, "Failed to get display size");
        display_pixel_bytes = format == LCD_COLOR_FMT_RGB565 ? 2 : 3;
        ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(st7701_get_panel(), 2,
            &display_framebuffers[0], &display_framebuffers[1]), TAG, "Failed to get native framebuffers");
        present_mutex = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(present_mutex, ESP_ERR_NO_MEM, TAG, "Failed to create presentation mutex");
        esp_lcd_dpi_panel_event_callbacks_t callbacks = {
            .on_refresh_done = bsp_display_refresh_ready,
        };
        ESP_LOGI(TAG, "Native double buffering enabled; waiting for scanout retirement");
        return esp_lcd_dpi_panel_register_event_callbacks(st7701_get_panel(), &callbacks, NULL);
    }
    flush_semaphore = xSemaphoreCreateBinary();
    xSemaphoreGive(flush_semaphore);
    esp_lcd_dpi_panel_event_callbacks_t callbacks = {
        .on_color_trans_done = bsp_display_flush_ready,
    };
    return esp_lcd_dpi_panel_register_event_callbacks(st7701_get_panel(), &callbacks, NULL);
}

static esp_err_t bsp_display_initialize_te(void) {
    te_semaphore = xSemaphoreCreateBinary();
    xSemaphoreGive(te_semaphore);

    gpio_config_t te_pin_cfg = {
        .pin_bit_mask = BIT64(BSP_LCD_TE_PIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = false,
        .pull_down_en = false,
        .intr_type    = GPIO_INTR_POSEDGE,
    };
    esp_err_t res = gpio_config(&te_pin_cfg);
    if (res != ESP_OK) {
        return res;
    }
    res = gpio_isr_handler_add(BSP_LCD_TE_PIN, te_gpio_interrupt_handler, NULL);
    return res;
}

static esp_err_t bsp_display_nvs_settings_get_u8(const char* key, uint8_t default_value, uint8_t* out_value) {
    if (key == NULL || out_value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs_handle;
    esp_err_t    res = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (res != ESP_OK) {
        *out_value = default_value;
        return res;
    }
    uint8_t value;
    res = nvs_get_u8(nvs_handle, key, &value);
    if (res != ESP_OK) {
        *out_value = default_value;
        nvs_close(nvs_handle);
        return res;
    }
    nvs_close(nvs_handle);

    *out_value = value;
    return res;
}

// Public functions

esp_err_t bsp_display_initialize(const bsp_display_configuration_t* configuration) {
    if (bsp_display_initialized) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(bsp_display_enable_dsi_phy_power(), TAG, "Failed to enable DSI PHY power");
    ESP_RETURN_ON_ERROR(bsp_display_initialize_panel(configuration), TAG, "Failed to initialize panel");

    uint8_t vcom = 0;
    if (bsp_display_get_vcom_default(&vcom) == ESP_OK && bsp_display_nvs_settings_get_u8("lcd_vcom", vcom, &vcom) == ESP_OK) {
        if (st7701_set_vcom(vcom) == ESP_OK) {
            ESP_LOGI(TAG, "Applied LCD VCOM from NVS: 0x%02X", vcom);
        } else {
            ESP_LOGE(TAG, "Failed to apply LCD VCOM from NVS");
        }
    } else {
        ESP_LOGI(TAG, "No LCD VCOM value stored in NVS");
    }

    ESP_RETURN_ON_ERROR(bsp_display_initialize_flush(configuration), TAG, "Failed to initialize flush callback");
    ESP_RETURN_ON_ERROR(bsp_display_initialize_te(), TAG, "Failed to tearing effect callback");
    bsp_display_initialized = true;
    return ESP_OK;
}

esp_err_t bsp_display_get_parameters(size_t* h_res, size_t* v_res, bsp_display_color_format_t* color_fmt,
                                     bsp_display_endianness_t* data_endian) {
    if (!bsp_display_initialized) {
        return ESP_FAIL;
    }

    lcd_color_format_t color_fmt_driver;
    st7701_get_parameters(h_res, v_res, &color_fmt_driver);

    switch (color_fmt_driver) {
        case LCD_COLOR_FMT_RGB565:
            if (color_fmt) {
                *color_fmt = BSP_DISPLAY_COLOR_FORMAT_16_565RGB;
            }
            break;
        case LCD_COLOR_FMT_RGB888:
            if (color_fmt) {
                *color_fmt = BSP_DISPLAY_COLOR_FORMAT_24_888RGB;
            }
            break;
        default:
            return ESP_FAIL;
    };

    if (data_endian) {
        *data_endian = BSP_DISPLAY_ENDIAN_LITTLE;
    }
    return ESP_OK;
}

esp_err_t bsp_display_get_panel(esp_lcd_panel_handle_t* panel) {
    if (!bsp_display_initialized) {
        return ESP_FAIL;
    }
    *panel = st7701_get_panel();
    return ESP_OK;
}

esp_err_t bsp_display_get_panel_io(esp_lcd_panel_io_handle_t* panel_io) {
    if (!bsp_display_initialized) {
        return ESP_FAIL;
    }

    *panel_io = st7701_get_panel_io();
    return ESP_OK;
}

bsp_display_rotation_t bsp_display_get_default_rotation() {
    return BSP_DISPLAY_ROTATION_270;
}

esp_err_t bsp_display_get_backlight_brightness(uint8_t* out_percentage) {
    ESP_RETURN_ON_FALSE(out_percentage, ESP_ERR_INVALID_ARG, TAG, "Percentage output argument is NULL");
    uint8_t                       raw_value;
    tanmatsu_coprocessor_handle_t handle = NULL;
    ESP_RETURN_ON_ERROR(bsp_tanmatsu_coprocessor_get_handle(&handle), TAG, "Failed to get coprocessor handle");
    ESP_RETURN_ON_ERROR(tanmatsu_coprocessor_get_display_backlight(handle, &raw_value), TAG,
                        "Failed to get display backlight brightness");
    *out_percentage = (raw_value * 100) / 255;
    return ESP_OK;
}

esp_err_t bsp_display_set_backlight_brightness(uint8_t percentage) {
    tanmatsu_coprocessor_handle_t handle = NULL;
    ESP_RETURN_ON_ERROR(bsp_tanmatsu_coprocessor_get_handle(&handle), TAG, "Failed to get coprocessor handle");
    ESP_RETURN_ON_ERROR(tanmatsu_coprocessor_set_display_backlight(handle, (percentage * 255) / 100), TAG,
                        "Failed to configure display backlight brightness");
    return ESP_OK;
}

esp_err_t bsp_display_set_tearing_effect_mode(bsp_display_te_mode_t mode) {
    if (!bsp_display_initialized) {
        return ESP_FAIL;
    }

    esp_lcd_panel_io_handle_t panel_io = st7701_get_panel_io();

    esp_err_t res = ESP_OK;

    if (mode == BSP_DISPLAY_TE_DISABLED) {
        res = esp_lcd_panel_io_tx_param(panel_io, LCD_CMD_TEOFF, (uint8_t[]){0}, 0);
    } else {
        res = esp_lcd_panel_io_tx_param(panel_io, LCD_CMD_TEON,
                                        (uint8_t[]){(mode == BSP_DISPLAY_TE_V_AND_H_BLANKING) ? 1 : 0}, 1);
    }

    if (res != ESP_OK) {
        return res;
    }

    display_te_mode = mode;
    return ESP_OK;
}

esp_err_t bsp_display_get_tearing_effect_mode(bsp_display_te_mode_t* mode) {
    if (mode == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *mode = display_te_mode;
    return ESP_OK;
}

esp_err_t bsp_display_get_tearing_effect_semaphore(SemaphoreHandle_t* semaphore) {
    if (semaphore == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *semaphore = te_semaphore;
    return ESP_OK;
}

static esp_err_t bsp_display_retire_flip(void) {
    TickType_t start = xTaskGetTickCount();
    /* The first completion can belong to a frame whose DMA was already
     * queued when draw_bitmap changed cur_fb_index. A second completion
     * guarantees that old scanout has retired on this ESP-IDF driver. */
    while (display_flip_pending &&
           (uint32_t)(__atomic_load_n(&display_refresh_count, __ATOMIC_ACQUIRE) - display_flip_start) < 2) {
        if (xTaskGetTickCount() - start >= pdMS_TO_TICKS(100)) return ESP_ERR_TIMEOUT;
        vTaskDelay(1);
    }
    if (display_flip_pending) {
        display_front_index ^= 1;
        display_flip_pending = false;
    }
    return ESP_OK;
}

static esp_err_t bsp_display_present(size_t x_start, size_t y_start, size_t x_end, size_t y_end, const void* buffer) {
    /* Never modify either page until an earlier timed-out flip has retired. */
    esp_err_t res = bsp_display_retire_flip();
    if (res != ESP_OK) return res;
    if (!buffer || x_start >= x_end || y_start >= y_end ||
        x_end > display_width || y_end > display_height) return ESP_ERR_INVALID_ARG;

    unsigned back_index = display_front_index ^ 1;
    uint8_t* back = display_framebuffers[back_index];
    size_t stride = display_width * display_pixel_bytes;
    if (x_start || y_start || x_end != display_width || y_end != display_height) {
        memcpy(back, display_framebuffers[display_front_index], stride * display_height);
    }
    const uint8_t* source = buffer;
    size_t row_bytes = (x_end - x_start) * display_pixel_bytes;
    for (size_t y = y_start; y < y_end; y++) {
        memcpy(back + y * stride + x_start * display_pixel_bytes, source, row_bytes);
        source += row_bytes;
    }
    /* Passing the native page selects the driver's page-flip path: cache
     * writeback and a pointer switch at the next DMA frame boundary. Passing
     * an external snapshot instead copies into the ACTIVE page via DMA2D. */
    res = esp_lcd_panel_draw_bitmap(st7701_get_panel(), 0, 0, display_width, display_height, back);
    if (res != ESP_OK) return res;
    display_flip_start = __atomic_load_n(&display_refresh_count, __ATOMIC_ACQUIRE);
    display_flip_pending = true;
    return bsp_display_retire_flip();
}

esp_err_t bsp_display_blit(size_t x_start, size_t y_start, size_t x_end, size_t y_end, const void* buffer) {
    if (present_mutex) {
        if (xSemaphoreTake(present_mutex, 0) != pdTRUE) return ESP_ERR_TIMEOUT;
        esp_err_t res = bsp_display_present(x_start, y_start, x_end, y_end, buffer);
        xSemaphoreGive(present_mutex);
        return res;
    }
    /* Legacy single-page mode still needs asynchronous source ownership. */
    if (xSemaphoreTake(flush_semaphore, 0) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t res = esp_lcd_panel_draw_bitmap(st7701_get_panel(), x_start, y_start, x_end, y_end, buffer);
    if (res != ESP_OK) {
        /* A rejected submission will not signal transfer completion. */
        xSemaphoreGive(flush_semaphore);
    }
    return res;
}

esp_err_t bsp_display_get_vcom(uint8_t* out_vcom) {
    return st7701_get_vcom(out_vcom);
}

esp_err_t bsp_display_get_vcom_default(uint8_t* out_vcom) {
    return st7701_get_vcom_otp(out_vcom);
}

esp_err_t bsp_display_set_vcom(uint8_t vcom) {
    return st7701_set_vcom(vcom);
}

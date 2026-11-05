#include "bsp/device.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "app/app.h"

static const char *TAG = "tanradio";

void app_main(void)
{
    esp_err_t res;

    // Initialize the Non Volatile Storage partition.
    res = nvs_flash_init();
    if (res == ESP_ERR_NVS_NO_FREE_PAGES ||
        res == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        res = nvs_flash_erase();
        if (res != ESP_OK) {
            ESP_LOGE(TAG, "Failed to erase NVS flash: %s",
                     esp_err_to_name(res));
            return;
        }

        res = nvs_flash_init();
    }

    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NVS: %s",
                 esp_err_to_name(res));
        return;
    }

    // Initialize the Board Support Package.
    const bsp_configuration_t bsp_configuration = {
        .display = {
            /* RGB565 reduces scanout and redraw PSRAM bandwidth by a third.
             * DSI underruns show up as blue frames during stream changes. */
            .requested_color_format = BSP_DISPLAY_COLOR_FORMAT_16_565RGB,
            .num_fbs = 2,
        },
    };

    res = bsp_device_initialize(&bsp_configuration);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize BSP: %s",
                 esp_err_to_name(res));
        return;
    }

    // Transfer control to the TanRadio application layer.
    app_init();
}

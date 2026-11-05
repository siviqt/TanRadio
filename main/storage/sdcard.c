/* BSP 1.6.2 owns the SD power controller and shared SDMMC host in this build. */
#include "storage/sdcard.h"

#include <stdbool.h>
#include "bsp/storage.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "tanradio.sd";

esp_err_t tanradio_sd_mount(void)
{
    bool mounted = false;
    ESP_RETURN_ON_ERROR(bsp_storage_get_mounted(BSP_STORAGE_TYPE_SDCARD, &mounted),
                        TAG, "Unable to query SD-card mount state");
    if (mounted) {
        return ESP_OK;
    }

    /* Preserve the production four-file limit and never format a user's card. */
    ESP_RETURN_ON_ERROR(bsp_storage_mount_advanced(BSP_STORAGE_TYPE_SDCARD,
                        TANRADIO_SD_MOUNT_POINT, 4, false),
                        TAG, "Unable to mount SD card through BSP");
    ESP_LOGI(TAG, "SD card mounted at %s through BSP 1.6.2", TANRADIO_SD_MOUNT_POINT);
    return ESP_OK;
}

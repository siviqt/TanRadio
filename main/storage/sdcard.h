/*
 * TanRadio SD-card mount service.
 */
#ifndef TANRADIO_MAIN_STORAGE_SDCARD_H
#define TANRADIO_MAIN_STORAGE_SDCARD_H

#include "esp_err.h"

#define TANRADIO_SD_MOUNT_POINT "/sd"

/** Mount the Tanmatsu microSD FAT filesystem at /sd. */
esp_err_t tanradio_sd_mount(void);

#endif /* TANRADIO_MAIN_STORAGE_SDCARD_H */

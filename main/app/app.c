/*
 * Project : TanRadio
 * Module  : app
 * File    : app.c
 *
 * Purpose :
 *   Application initialization coordinator.
 */

/* Local header */
#include "app.h"

/* Application modules */
#include "audio/audio.h"
#include "input/input.h"
#include "network/network.h"
#include "radio/radio.h"
#include "radio/browser.h"
#include "ui/ui.h"
#include "util/util.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static void app_services_task(void *arg)
{
    (void)arg;
    int64_t started = esp_timer_get_time();
    /* Both SDMMC slots share the host: transport must precede SD setup. */
    network_init();
    radio_init();
    ui_set_services_state(UI_SERVICES_READY);
    ESP_LOGI("tanradio.app", "Radio services ready after %lld ms in background",
             (long long)((esp_timer_get_time() - started) / 1000));
    vTaskDelete(NULL);
}

/*******************************************************************************
 * Public Functions
 ******************************************************************************/

/**
 * @brief Initialize the TanRadio application.
 */
void app_init(void)
{
    util_init();
    ui_init();
    if (!ui_show_welcome()) {
        return;
    }
    int64_t started = esp_timer_get_time();
    audio_init();
    radio_browser_init();
    ui_start();
    input_init();
    if (xTaskCreate(app_services_task, "tanradio_startup", 8192, NULL, 3, NULL) != pdPASS) {
        ui_set_services_state(UI_SERVICES_FAILED);
        ESP_LOGE("tanradio.app", "Unable to start radio services task");
    }
    ESP_LOGI("tanradio.app", "Main screen ready %lld ms after Space",
             (long long)((esp_timer_get_time() - started) / 1000));
}

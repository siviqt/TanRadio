/*
 * Project : TanRadio
 * Module  : network
 * File    : network.c
 *
 * Purpose :
 *   TanRadio network subsystem.
 */

/* ESP-IDF */
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Standard C library */
#include <string.h>
#include <stdatomic.h>

/* Tanmatsu Wi-Fi */
#include "wifi_connection.h"
#include "wifi_remote.h"

/* Local header */
#include "network.h"

static const char *TAG = "tanradio.network";
static atomic_bool network_stack_ready;

static void network_connect_task(void *arg)
{
    (void)arg;
    int64_t started = esp_timer_get_time();
    esp_err_t result = wifi_connect_try_all();
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "Connected after %lld ms in background",
                 (long long)((esp_timer_get_time() - started) / 1000));
    } else {
        ESP_LOGW(TAG, "Saved-network connection failed: %s", esp_err_to_name(result));
    }
    vTaskDelete(NULL);
}

/*******************************************************************************
 * Public Functions
 ******************************************************************************/

/**
 * @brief Initialize the network subsystem.
 */
void network_init(void)
{
    int64_t started = esp_timer_get_time();
    esp_err_t res;

    /*
     * Initialize the Tanmatsu remote Wi-Fi interface.
     *
     * This is the official Tanmatsu Wi-Fi initialization path.
     */
    res = wifi_remote_initialize();
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi radio unavailable: %s",
                 esp_err_to_name(res));
        return;
    }

    /*
     * Initialize the Espressif Wi-Fi stack.
     */
    res = wifi_connection_init_stack();
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize Wi-Fi stack: %s",
                 esp_err_to_name(res));
        return;
    }

    atomic_store(&network_stack_ready, true);
    /* SD-card setup still follows transport/stack initialization because both
     * SDMMC slots share the host. Saved-network retries must not block the UI. */
    if (xTaskCreate(network_connect_task, "tanradio_wifi", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Unable to start Wi-Fi connection task");
    }
    ESP_LOGI(TAG, "Network stack ready after %lld ms; connecting in background",
             (long long)((esp_timer_get_time() - started) / 1000));
}

/**
 * @brief Shut down the TanRadio network subsystem.
 *
 * The Wi-Fi manager owns the ESP-IDF Wi-Fi stop operation.
 * TanRadio therefore uses the manager's disconnect API rather
 * than stopping the Wi-Fi driver directly.
 */
void network_shutdown(void)
{
    ESP_LOGI(TAG, "Shutting down network subsystem");

    if (atomic_load(&network_stack_ready)) wifi_connection_disconnect();

    ESP_LOGI(TAG, "Network subsystem shut down");
}

/**
 * @brief Return whether Wi-Fi is currently connected.
 */
bool network_is_connected(void)
{
    return atomic_load(&network_stack_ready) && wifi_connection_is_connected();
}

/**
 * @brief Get the SSID of the currently connected Wi-Fi network.
 */
bool network_get_ssid(char *out_ssid, size_t out_size)
{
    if (out_ssid == NULL || out_size == 0) {
        return false;
    }

    out_ssid[0] = '\0';

    if (!network_is_connected()) {
        return false;
    }

    wifi_ap_record_t ap_info = {0};

    esp_err_t res = esp_wifi_sta_get_ap_info(&ap_info);
    if (res != ESP_OK) {
        return false;
    }

    size_t ssid_length = strnlen(
        (const char *)ap_info.ssid,
        sizeof(ap_info.ssid));

    if (ssid_length == 0) {
        return false;
    }

    if (ssid_length >= out_size) {
        ssid_length = out_size - 1;
    }

    memcpy(out_ssid, ap_info.ssid, ssid_length);
    out_ssid[ssid_length] = '\0';

    return true;
}

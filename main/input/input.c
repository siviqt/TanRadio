/*
 * Project : TanRadio
 * Module  : input
 * File    : input.c
 *
 * Purpose :
 *   TanRadio input subsystem.
 */

/* ESP-IDF */
#include "esp_log.h"

/* BSP */
#include "bsp/input.h"

/* Local modules */
#include "ui/ui.h"
#include "radio/radio.h"
#include "audio/audio.h"

/* Local header */
#include "input.h"
#include "input/keys.h"

static const char *TAG = "tanradio.input";

static QueueHandle_t input_event_queue = NULL;
static TaskHandle_t input_task_handle = NULL;

#define KEY_REPEAT_INITIAL_DELAY_MS 500
#define KEY_REPEAT_INTERVAL_MS 100

static void input_dispatch_navigation(bsp_input_navigation_key_t key)
{
    if (ui_handle_exit_navigation(key)) return;
    if (key == BSP_INPUT_NAVIGATION_KEY_VOLUME_UP) (void)audio_volume_up();
    else if (key == BSP_INPUT_NAVIGATION_KEY_VOLUME_DOWN) (void)audio_volume_down();
    else ui_handle_navigation(key, true);
}

static void input_task(void *arg)
{
    (void)arg;

    input_keys_t keys;
    input_keys_init(&keys, pdMS_TO_TICKS(KEY_REPEAT_INITIAL_DELAY_MS),
                     pdMS_TO_TICKS(KEY_REPEAT_INTERVAL_MS));

    while (true) {
        bsp_input_event_t event;
        TickType_t wait_ticks = portMAX_DELAY;

        uint32_t until_repeat = input_keys_wait(&keys, xTaskGetTickCount());
        if (until_repeat != UINT32_MAX) wait_ticks = until_repeat;
        /* Poll held keys too: a dropped release must never leave volume or
         * editing repeating forever, or leave a function key stuck down. */
        for (size_t i = 0; i < sizeof(keys.held) / sizeof(keys.held[0]); i++) {
            if (keys.held[i] && wait_ticks > pdMS_TO_TICKS(50)) {
                wait_ticks = pdMS_TO_TICKS(50);
            }
        }

        if (xQueueReceive(
                input_event_queue,
                &event,
                wait_ticks) == pdTRUE) {
            switch (event.type) {
                case INPUT_EVENT_TYPE_NAVIGATION:
                    if (input_keys_event(&keys, event.args_navigation.key,
                                         event.args_navigation.state, xTaskGetTickCount())) {
                        input_dispatch_navigation(event.args_navigation.key);
                    }
                    break;

                case INPUT_EVENT_TYPE_ACTION:
                    if (event.args_action.type ==
                            BSP_INPUT_ACTION_TYPE_POWER_BUTTON &&
                        event.args_action.state) {

                        input_keys_cancel_repeat(&keys);
                        ui_toggle_display();
                    } else if (event.args_action.type ==
                        BSP_INPUT_ACTION_TYPE_AUDIO_JACK) {

                        /* Audio owns route, gain, mute and amplifier ordering.
                         * Re-read current state so a queued stale event is harmless. */
                        esp_err_t res = audio_refresh_output();
                        if (res != ESP_OK)
                            ESP_LOGW(TAG, "Audio output update failed: %s", esp_err_to_name(res));
                    }
                    break;

                case INPUT_EVENT_TYPE_KEYBOARD:
                    ESP_LOGD(
                        TAG,
                        "Keyboard event: ASCII=0x%02x UTF-8=\"%s\" modifiers=0x%08lx",
                        (unsigned int)(unsigned char)event.args_keyboard.ascii,
                        event.args_keyboard.utf8,
                        (unsigned long)event.args_keyboard.modifiers);

                    ui_handle_keyboard(
                        event.args_keyboard.ascii,
                        event.args_keyboard.utf8,
                        event.args_keyboard.modifiers);
                    break;

                case INPUT_EVENT_TYPE_SCANCODE:
                    ESP_LOGD(
                        TAG,
                        "Scancode event: 0x%02x",
                        (unsigned int)event.args_scancode.scancode);

                    ui_handle_scancode(
                        event.args_scancode.scancode);
                    break;

                default:
                    /*
                     * Unknown or unused BSP input event.
                     */
                    break;
            }
        }

        for (size_t i = 0; i < sizeof(keys.held) / sizeof(keys.held[0]); i++) {
            if (!keys.held[i]) continue;
            bool pressed = false;
            esp_err_t result = bsp_input_read_navigation_key((bsp_input_navigation_key_t)i, &pressed);
            if (result == ESP_OK && !pressed) {
                input_keys_event(&keys, (bsp_input_navigation_key_t)i, false, xTaskGetTickCount());
            } else if (result != ESP_OK) {
                /* A failed sample is not a release: it must never re-arm F1. */
                input_keys_cancel_repeat(&keys);
            }
        }
        bsp_input_navigation_key_t repeat;
        if (input_keys_due(&keys, xTaskGetTickCount(), &repeat)) input_dispatch_navigation(repeat);
    }
}

/*******************************************************************************
 * Public Functions
 ******************************************************************************/

/**
 * @brief Initialize the input subsystem.
 */
void input_init(void)
{
    esp_err_t res;

    res = bsp_input_get_queue(&input_event_queue);
    if (res != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to get BSP input event queue: %s",
            esp_err_to_name(res));
        return;
    }

    if (input_event_queue == NULL) {
        ESP_LOGE(TAG, "BSP returned a NULL input event queue");
        return;
    }

    BaseType_t task_res = xTaskCreate(
        input_task,
        "tanradio_input",
        8192,
        NULL,
        5,
        &input_task_handle);

    if (task_res != pdPASS) {
        input_task_handle = NULL;
        ESP_LOGE(TAG, "Failed to create input task");
        return;
    }

    ESP_LOGI(TAG, "Input subsystem initialized");
}

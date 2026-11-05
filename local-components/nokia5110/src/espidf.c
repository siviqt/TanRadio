// SPDX-License-Identifier: MIT
#include "nokia5110_espidf.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static portMUX_TYPE owner_lock = portMUX_INITIALIZER_UNLOCKED;
static nokia5110_espidf_t *owner;
static void pin_list(nokia5110_espidf_t *d, int *p) {
    p[0]=d->pins.ce; p[1]=d->pins.rst; p[2]=d->pins.clk; p[3]=d->pins.din; p[4]=d->pins.dc;
}
static int end(void *context) {
    nokia5110_espidf_t *d = context;
    int pins[5], result = 0; pin_list(d, pins);
    for (unsigned i = 0; i < d->configured_pins; i++) {
        // Disable output without gpio_reset_pin's default pull-up.
        gpio_config_t config = {.pin_bit_mask=1ULL << pins[i], .mode=GPIO_MODE_INPUT,
            .pull_up_en=GPIO_PULLUP_DISABLE, .pull_down_en=GPIO_PULLDOWN_DISABLE,
            .intr_type=GPIO_INTR_DISABLE};
        if (gpio_config(&config) != ESP_OK) result = -1;
    }
    d->configured_pins = 0;
    return result;
}
static int begin(void *context) {
    nokia5110_espidf_t *d = context;
    int pins[5]; pin_list(d, pins);
    for (unsigned i = 0; i < 5; i++) {
        d->configured_pins = i + 1;
        if (gpio_reset_pin(pins[i]) != ESP_OK ||
            gpio_set_level(pins[i], pins[i] == d->pins.ce) != ESP_OK ||
            gpio_set_direction(pins[i], GPIO_MODE_OUTPUT) != ESP_OK) { end(d); return -1; }
    }
    vTaskDelay(pdMS_TO_TICKS(20) + 1);
    if (gpio_set_level(d->pins.rst, 1) != ESP_OK) { end(d); return -1; }
    vTaskDelay(pdMS_TO_TICKS(20) + 1);
    return 0;
}
static int write_bytes(void *context, bool data, const uint8_t *bytes, size_t length) {
    nokia5110_espidf_t *d = context;
    // Keep the per-byte CE and clock timing used by the verified test app.
    for (size_t i = 0; i < length; i++) {
        if (gpio_set_level(d->pins.dc, data) || gpio_set_level(d->pins.ce, 0)) goto fail;
        esp_rom_delay_us(2);
        for (int bit = 7; bit >= 0; bit--) {
            if (gpio_set_level(d->pins.clk, 0) || gpio_set_level(d->pins.din, (bytes[i] >> bit) & 1)) goto fail;
            esp_rom_delay_us(2);
            if (gpio_set_level(d->pins.clk, 1)) goto fail;
            esp_rom_delay_us(2);
        }
        if (gpio_set_level(d->pins.clk, 0) || gpio_set_level(d->pins.ce, 1)) goto fail;
    }
    return 0;
fail:
    gpio_set_level(d->pins.clk, 0);
    gpio_set_level(d->pins.ce, 1);
    return -1;
}
nokia5110_result_t nokia5110_espidf_open(nokia5110_espidf_t *d, nokia5110_pins_t pins, unsigned contrast) {
    if (!d || contrast > 127) return NOKIA5110_INVALID;
    if (d->display.initialized) return NOKIA5110_STATE;
    int list[] = {pins.clk, pins.din, pins.ce, pins.dc, pins.rst};
    for (int i = 0; i < 5; i++) {
        if (!GPIO_IS_VALID_OUTPUT_GPIO(list[i])) return NOKIA5110_INVALID;
        for (int j = 0; j < i; j++) if (list[i] == list[j]) return NOKIA5110_INVALID;
    }
    portENTER_CRITICAL(&owner_lock);
    if (owner) { portEXIT_CRITICAL(&owner_lock); return NOKIA5110_BUSY; }
    owner = d;
    portEXIT_CRITICAL(&owner_lock);
    d->pins = pins;
    nokia5110_transport_t t = {.context=d, .begin=begin, .write=write_bytes, .end=end};
    nokia5110_result_t result = nokia5110_init(&d->display, &t, contrast);
    if (result) { portENTER_CRITICAL(&owner_lock); owner=NULL; portEXIT_CRITICAL(&owner_lock); }
    return result;
}
nokia5110_result_t nokia5110_espidf_close(nokia5110_espidf_t *d) {
    if (!d) return NOKIA5110_INVALID;
    portENTER_CRITICAL(&owner_lock);
    bool is_owner = owner == d;
    portEXIT_CRITICAL(&owner_lock);
    if (!is_owner) return NOKIA5110_OK;
    nokia5110_result_t result = nokia5110_deinit(&d->display);
    portENTER_CRITICAL(&owner_lock); owner=NULL; portEXIT_CRITICAL(&owner_lock);
    return result;
}

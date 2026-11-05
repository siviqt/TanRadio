#ifndef TANRADIO_INPUT_KEYS_H
#define TANRADIO_INPUT_KEYS_H

#include "bsp/input.h"

typedef struct {
    bool held[BSP_INPUT_NAVIGATION_KEY_VOLUME_DOWN + 1];
    bsp_input_navigation_key_t repeating;
    uint32_t started, last, delay, interval;
} input_keys_t;

void input_keys_init(input_keys_t *keys, uint32_t delay, uint32_t interval);
/* Only a fresh key-down is dispatched. Releases and duplicate presses aren't. */
bool input_keys_event(input_keys_t *keys, bsp_input_navigation_key_t key,
                       bool pressed, uint32_t now);
void input_keys_cancel_repeat(input_keys_t *keys);
uint32_t input_keys_wait(const input_keys_t *keys, uint32_t now);
bool input_keys_due(input_keys_t *keys, uint32_t now,
                     bsp_input_navigation_key_t *key);

#endif

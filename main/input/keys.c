#include "input/keys.h"

void input_keys_init(input_keys_t *keys, uint32_t delay, uint32_t interval)
{
    *keys = (input_keys_t){.delay = delay, .interval = interval};
}

static bool repeats(bsp_input_navigation_key_t key)
{
    return key == BSP_INPUT_NAVIGATION_KEY_UP || key == BSP_INPUT_NAVIGATION_KEY_DOWN ||
           key == BSP_INPUT_NAVIGATION_KEY_BACKSPACE ||
           key == BSP_INPUT_NAVIGATION_KEY_VOLUME_UP || key == BSP_INPUT_NAVIGATION_KEY_VOLUME_DOWN;
}

void input_keys_cancel_repeat(input_keys_t *keys)
{
    keys->repeating = BSP_INPUT_NAVIGATION_KEY_NONE;
}

bool input_keys_event(input_keys_t *keys, bsp_input_navigation_key_t key,
                       bool pressed, uint32_t now)
{
    if (key <= BSP_INPUT_NAVIGATION_KEY_NONE || key > BSP_INPUT_NAVIGATION_KEY_VOLUME_DOWN) return false;
    if (!pressed) {
        keys->held[key] = false;
        if (keys->repeating == key) input_keys_cancel_repeat(keys);
        return false;
    }
    if (keys->held[key]) return false;
    keys->held[key] = true;
    /* Any other command cancels an old repeat, including a screen change. */
    keys->repeating = repeats(key) ? key : BSP_INPUT_NAVIGATION_KEY_NONE;
    keys->started = keys->last = now;
    return true;
}

uint32_t input_keys_wait(const input_keys_t *keys, uint32_t now)
{
    if (keys->repeating == BSP_INPUT_NAVIGATION_KEY_NONE) return UINT32_MAX;
    uint32_t initial = now - keys->started;
    if (initial < keys->delay) return keys->delay - initial;
    uint32_t elapsed = now - keys->last;
    return elapsed < keys->interval ? keys->interval - elapsed : 0;
}

bool input_keys_due(input_keys_t *keys, uint32_t now, bsp_input_navigation_key_t *key)
{
    if (keys->repeating == BSP_INPUT_NAVIGATION_KEY_NONE || input_keys_wait(keys, now)) return false;
    *key = keys->repeating;
    keys->last = now; /* One repeat per iteration; never burst after slow I/O. */
    return true;
}

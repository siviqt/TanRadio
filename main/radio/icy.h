#ifndef TANRADIO_ICY_H
#define TANRADIO_ICY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RADIO_TITLE_SIZE 240
#define ICY_BLOCK_SIZE (255 * 16)

typedef struct {
    uint32_t interval, audio_left;
    size_t block_size, block_used;
    uint64_t audio_position;
    uint8_t block[ICY_BLOCK_SIZE];
} icy_filter_t;

typedef bool (*icy_audio_fn)(void *context, const uint8_t *data, size_t size);
typedef void (*icy_title_fn)(void *context, uint64_t position, const char *title);

bool icy_parse_uint(const char *text, uint32_t *value);
void icy_copy_text(char *out, size_t capacity, const uint8_t *text, size_t length);
void icy_filter_init(icy_filter_t *filter, uint32_t interval);
/* A NULL filter passes through unframed audio. A false sink result ends the connection. */
bool icy_filter_feed(icy_filter_t *filter, const uint8_t *data, size_t size,
                     icy_audio_fn audio, icy_title_fn title, void *context);

#endif

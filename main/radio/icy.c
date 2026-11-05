#include "radio/icy.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

bool icy_parse_uint(const char *text, uint32_t *value)
{
    while (*text == ' ' || *text == '\t') text++;
    if (*text < '0' || *text > '9') return false;
    uint32_t result = 0;
    while (*text >= '0' && *text <= '9') {
        unsigned digit = *text++ - '0';
        if (result > (UINT32_MAX - digit) / 10) return false;
        result = result * 10 + digit;
    }
    while (*text == ' ' || *text == '\t') text++;
    if (*text) return false;
    *value = result;
    return true;
}

static size_t utf8_size(const uint8_t *text, size_t length)
{
    uint8_t first = text[0];
    if (first < 0x80) return 1;
    size_t width = first >= 0xc2 && first <= 0xdf ? 2 :
                   first >= 0xe0 && first <= 0xef ? 3 :
                   first >= 0xf0 && first <= 0xf4 ? 4 : 0;
    if (!width || width > length) return 0;
    for (size_t i = 1; i < width; i++) {
        if ((text[i] & 0xc0) != 0x80) return 0;
    }
    if ((first == 0xe0 && text[1] < 0xa0) || (first == 0xed && text[1] >= 0xa0) ||
        (first == 0xf0 && text[1] < 0x90) || (first == 0xf4 && text[1] >= 0x90)) return 0;
    return width;
}

void icy_copy_text(char *out, size_t capacity, const uint8_t *text, size_t length)
{
    size_t used = 0;
    if (!capacity) return;
    while (length && *text) {
        size_t width = utf8_size(text, length);
        if (!width) {
            /* Some ICY sources still send ISO-8859-1. */
            if (*text >= 0xa0) {
                if (capacity - used < 3) break;
                out[used++] = (char)(0xc0 | (*text >> 6));
                out[used++] = (char)(0x80 | (*text & 0x3f));
            }
            width = 1;
        } else if (width == 1 && (*text <= ' ' || *text == 0x7f)) {
            if (used && out[used - 1] != ' ' && capacity - used >= 2) out[used++] = ' ';
        } else {
            if (width >= capacity - used) break;
            memcpy(out + used, text, width);
            used += width;
        }
        text += width;
        length -= width;
    }
    if (used && out[used - 1] == ' ') used--;
    out[used] = '\0';
}

static void icy_emit_title(icy_filter_t *filter, icy_title_fn title, void *context)
{
    const uint8_t *cursor = filter->block;
    const uint8_t *end = memchr(cursor, 0, filter->block_size);
    if (!end) end = cursor + filter->block_size;
    while (cursor < end) {
        while (cursor < end && (isspace(*cursor) || *cursor == ';')) cursor++;
        const uint8_t *key = cursor;
        while (cursor < end && (isalnum(*cursor) || *cursor == '_' || *cursor == '-')) cursor++;
        size_t key_size = cursor - key;
        while (cursor < end && isspace(*cursor)) cursor++;
        if (!key_size || cursor == end || *cursor++ != '=') return;
        while (cursor < end && isspace(*cursor)) cursor++;
        if (cursor == end || (*cursor != '\'' && *cursor != '"')) return;
        uint8_t quote = *cursor++;
        const uint8_t *value = cursor;
        while (cursor < end) {
            if (*cursor == '\\' && cursor + 1 < end && cursor[1] == quote) {
                cursor += 2;
                continue;
            }
            if (*cursor == quote) {
                const uint8_t *after = cursor + 1;
                while (after < end && isspace(*after)) after++;
                if (after == end || *after == ';') break;
            }
            cursor++;
        }
        if (cursor == end) return;
        if (key_size == sizeof("StreamTitle") - 1 &&
            strncasecmp((const char *)key, "StreamTitle", key_size) == 0) {
            char clean[RADIO_TITLE_SIZE];
            /* Strip quote escapes in place before normalizing the text. */
            uint8_t *write = (uint8_t *)value;
            for (const uint8_t *read = value; read < cursor; read++) {
                if (*read == '\\' && read + 1 < cursor && read[1] == quote) read++;
                *write++ = *read;
            }
            icy_copy_text(clean, sizeof(clean), value, write - value);
            title(context, filter->audio_position, clean);
            return;
        }
        cursor++;
    }
}

void icy_filter_init(icy_filter_t *filter, uint32_t interval)
{
    filter->interval = interval;
    filter->audio_left = interval;
    filter->audio_position = 0;
    filter->block_size = filter->block_used = 0;
}

bool icy_filter_feed(icy_filter_t *filter, const uint8_t *data, size_t size,
                     icy_audio_fn audio, icy_title_fn title, void *context)
{
    if (!size) return true;
    if (!filter || !filter->interval) return audio(context, data, size);
    while (size) {
        size_t take = 1;
        if (filter->audio_left) {
            take = size < filter->audio_left ? size : filter->audio_left;
            if (!audio(context, data, take)) return false;
            filter->audio_left -= take;
            filter->audio_position += take;
        } else if (!filter->block_size) {
            filter->block_size = (size_t)*data * 16;
            if (!filter->block_size) filter->audio_left = filter->interval;
        } else {
            take = filter->block_size - filter->block_used;
            if (take > size) take = size;
            memcpy(filter->block + filter->block_used, data, take);
            filter->block_used += take;
            if (filter->block_used == filter->block_size) {
                icy_emit_title(filter, title, context);
                filter->block_size = filter->block_used = 0;
                filter->audio_left = filter->interval;
            }
        }
        data += take;
        size -= take;
    }
    return true;
}

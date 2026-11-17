#ifndef TANRADIO_STREAM_FORMAT_H
#define TANRADIO_STREAM_FORMAT_H

#include <stddef.h>
#include <stdint.h>

#define STREAM_URL_CAPACITY 1024

typedef enum {
    STREAM_UNKNOWN,
    STREAM_MP3,
    STREAM_AAC,
    STREAM_OGG,
    STREAM_OPUS,
    STREAM_VORBIS,
    STREAM_FLAC,
    STREAM_TS,
    STREAM_PLAYLIST,
    STREAM_UNSUPPORTED,
} stream_format_t;

stream_format_t stream_format_from_codec(const char *codec);
stream_format_t stream_format_from_mime(const char *mime);
stream_format_t stream_format_probe(const uint8_t *bytes, size_t size);
/* Total bytes occupied by the first ID3v2 tag, even if only its header is present. */
size_t stream_id3_size(const uint8_t *bytes, size_t size);
const char *stream_format_name(stream_format_t format);

#endif

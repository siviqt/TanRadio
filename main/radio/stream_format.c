#include "radio/stream_format.h"

#include <stdbool.h>
#include <string.h>
#include <strings.h>

stream_format_t stream_format_from_codec(const char *codec)
{
    if (!codec) return STREAM_UNKNOWN;
    if (!strcasecmp(codec, "MP3")) return STREAM_MP3;
    if (!strcasecmp(codec, "AAC") || !strcasecmp(codec, "AAC+") ||
        !strcasecmp(codec, "AAC-LC") || !strcasecmp(codec, "HE-AAC") ||
        !strcasecmp(codec, "HE-AACv2")) return STREAM_AAC;
    if (!strcasecmp(codec, "OGG")) return STREAM_OGG;
    if (!strcasecmp(codec, "OPUS")) return STREAM_OPUS;
    if (!strcasecmp(codec, "VORBIS") || !strcasecmp(codec, "OGG VORBIS")) return STREAM_VORBIS;
    if (!strcasecmp(codec, "FLAC")) return STREAM_FLAC;
    return STREAM_UNKNOWN;
}

static bool matches_mime(const char *mime, const char *expected)
{
    size_t length = strlen(expected);
    return !strncasecmp(mime, expected, length) &&
           (mime[length] == '\0' || mime[length] == ';' || mime[length] == ' ');
}

stream_format_t stream_format_from_mime(const char *mime)
{
    if (!mime) return STREAM_UNKNOWN;
    if (matches_mime(mime, "audio/mpeg") || matches_mime(mime, "audio/mp3")) return STREAM_MP3;
    if (matches_mime(mime, "audio/aac") || matches_mime(mime, "audio/aacp") ||
        matches_mime(mime, "audio/x-aac")) return STREAM_AAC;
    if (matches_mime(mime, "audio/ogg") || matches_mime(mime, "application/ogg") ||
        matches_mime(mime, "audio/opus")) return STREAM_OGG;
    if (matches_mime(mime, "audio/flac") || matches_mime(mime, "audio/x-flac")) return STREAM_FLAC;
    if (matches_mime(mime, "application/vnd.apple.mpegurl") ||
        matches_mime(mime, "application/x-mpegurl") || matches_mime(mime, "audio/mpegurl") ||
        matches_mime(mime, "audio/x-mpegurl")) return STREAM_PLAYLIST;
    if (matches_mime(mime, "text/html") || matches_mime(mime, "application/json") ||
        matches_mime(mime, "audio/mp4") || matches_mime(mime, "video/mp4")) return STREAM_UNSUPPORTED;
    return STREAM_UNKNOWN;
}

size_t stream_id3_size(const uint8_t *bytes, size_t size)
{
    if (size < 10 || memcmp(bytes, "ID3", 3) || bytes[3] < 2 || bytes[3] > 4 ||
        ((bytes[6] | bytes[7] | bytes[8] | bytes[9]) & 0x80)) return 0;
    size_t length = 10;
    if (bytes[3] == 4 && (bytes[5] & 0x10)) length += 10;
    return length + ((size_t)bytes[6] << 21) + ((size_t)bytes[7] << 14) +
           ((size_t)bytes[8] << 7) + bytes[9];
}

static size_t adts_frame_size(const uint8_t *bytes, size_t size)
{
    if (size < 7 || bytes[0] != 0xff || (bytes[1] & 0xf6) != 0xf0 ||
        ((bytes[2] >> 2) & 15) > 12) return 0;
    size_t length = ((bytes[3] & 3) << 11) | (bytes[4] << 3) | (bytes[5] >> 5);
    return length >= ((bytes[1] & 1) ? 7 : 9) ? length : 0;
}

static size_t mpeg_frame_size(const uint8_t *bytes, size_t size)
{
    static const uint16_t bitrate[2][3][14] = {
        {
            {32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448},
            {32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384},
            {32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320},
        }, {
            {32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256},
            {8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},
            {8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},
        },
    };
    static const unsigned rates[] = {44100, 48000, 32000};
    if (size < 4 || bytes[0] != 0xff || (bytes[1] & 0xe0) != 0xe0) return 0;
    unsigned version = (bytes[1] >> 3) & 3;
    unsigned layer = (bytes[1] >> 1) & 3;
    unsigned bit_index = bytes[2] >> 4;
    unsigned rate_index = (bytes[2] >> 2) & 3;
    if (version == 1 || !layer || !bit_index || bit_index == 15 || rate_index == 3) return 0;
    unsigned rate = rates[rate_index] >> (version == 3 ? 0 : version == 2 ? 1 : 2);
    unsigned kbps = bitrate[version != 3][3 - layer][bit_index - 1];
    unsigned padding = (bytes[2] >> 1) & 1;
    if (layer == 3) return (12000 * kbps / rate + padding) * 4;
    return (layer == 1 && version != 3 ? 72000 : 144000) * kbps / rate + padding;
}

static stream_format_t ogg_format(const uint8_t *bytes, size_t size)
{
    if (size < 27) return STREAM_UNKNOWN;
    size_t start = 27 + bytes[26];
    if (size < start + 8) return STREAM_UNKNOWN;
    if (bytes[4] || !(bytes[5] & 2) || !bytes[26] || bytes[27] < 8) return STREAM_UNSUPPORTED;
    if (!memcmp(bytes + start, "OpusHead", 8)) return STREAM_OPUS;
    if (!memcmp(bytes + start, "\001vorbis", 7)) return STREAM_VORBIS;
    return STREAM_UNSUPPORTED;
}

stream_format_t stream_format_probe(const uint8_t *bytes, size_t size)
{
    if (!bytes || !size) return STREAM_UNKNOWN;
    size_t id3;
    while ((id3 = stream_id3_size(bytes, size))) {
        if (id3 >= size) return STREAM_UNKNOWN;
        bytes += id3;
        size -= id3;
    }
    if (size >= 4 && !memcmp(bytes, "OggS", 4)) return ogg_format(bytes, size);
    if (size >= 4 && !memcmp(bytes, "fLaC", 4)) return STREAM_FLAC;
    if (size >= 8 && !memcmp(bytes + 4, "ftyp", 4)) return STREAM_UNSUPPORTED;
    if (size > 376 && bytes[0] == 0x47 && bytes[188] == 0x47 && bytes[376] == 0x47)
        return STREAM_TS;

    size_t text = size >= 3 && !memcmp(bytes, "\xef\xbb\xbf", 3) ? 3 : 0;
    while (text < size && (bytes[text] == ' ' || bytes[text] == '\t' ||
                           bytes[text] == '\r' || bytes[text] == '\n')) text++;
    if (size - text >= 7 && !memcmp(bytes + text, "#EXTM3U", 7)) return STREAM_PLAYLIST;
    const char *start = (const char *)bytes + text;
    if ((size - text >= 5 && !strncasecmp(start, "<html", 5)) ||
        (size - text >= 9 && !strncasecmp(start, "<!doctype", 9))) return STREAM_UNSUPPORTED;

    /* Live joins can start mid-frame. Two consistent headers avoid confusing
     * a sync-like payload with the start of a stream. */
    for (size_t i = 0; i + 4 <= size; i++) {
        const uint8_t *header = bytes + i;
        size_t remaining = size - i;
        if (header[0] != 0xff) continue;
        size_t frame = adts_frame_size(header, remaining);
        if (frame && frame < remaining && adts_frame_size(header + frame, remaining - frame) &&
            (header[2] & 0x3c) == (header[frame + 2] & 0x3c)) return STREAM_AAC;
        frame = mpeg_frame_size(header, remaining);
        if (frame && frame < remaining && mpeg_frame_size(header + frame, remaining - frame) &&
            (header[1] & 0xfe) == (header[frame + 1] & 0xfe) &&
            (header[2] & 0x0c) == (header[frame + 2] & 0x0c)) return STREAM_MP3;
    }
    return STREAM_UNKNOWN;
}

const char *stream_format_name(stream_format_t format)
{
    switch (format) {
        case STREAM_MP3: return "MP3";
        case STREAM_AAC: return "AAC";
        case STREAM_OGG: return "Ogg";
        case STREAM_OPUS: return "Opus";
        case STREAM_VORBIS: return "Vorbis";
        case STREAM_FLAC: return "FLAC";
        case STREAM_TS:
        case STREAM_PLAYLIST: return "HLS";
        default: return "";
    }
}

#include "radio/hls_manifest.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
    const char *value;
    size_t length;
} hls_attribute_t;

static hls_attribute_t attribute(const char *line, const char *name)
{
    size_t name_size = strlen(name);
    while (*line) {
        const char *key = line;
        line += strcspn(line, "=,");
        if (*line != '=') break;
        bool match = (size_t)(line - key) == name_size && !memcmp(key, name, name_size);
        const char *value = ++line;
        if (*line == '"') {
            value = ++line;
            line += strcspn(line, "\"");
            if (*line != '"') break;
        } else {
            line += strcspn(line, ",");
        }
        if (match) return (hls_attribute_t){value, (size_t)(line - value)};
        if (*line == '"') line++;
        if (*line != ',') break;
        line++;
    }
    return (hls_attribute_t){0};
}

static bool attribute_is(hls_attribute_t value, const char *text)
{
    return value.value && value.length == strlen(text) && !memcmp(value.value, text, value.length);
}

static bool audio_variant(const char *line)
{
    if (attribute(line, "RESOLUTION").value || attribute(line, "VIDEO").value) return false;
    hls_attribute_t codecs = attribute(line, "CODECS");
    if (!codecs.value) return true;
    return (codecs.length > 5 && !memcmp(codecs.value, "mp4a.", 5) &&
            !memchr(codecs.value, ',', codecs.length)) || attribute_is(codecs, "mp3");
}

static bool parse_number(const char *text, uint64_t *result)
{
    if (!isdigit((unsigned char)*text)) return false;
    char *end;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno || *end) return false;
    *result = value;
    return true;
}

static char *next_line(char **cursor)
{
    char *line = *cursor;
    if (!line || !*line) return NULL;
    char *end = strchr(line, '\n');
    *cursor = end ? end + 1 : NULL;
    if (end) *end = 0;
    while (*line == ' ' || *line == '\t') line++;
    size_t length = strlen(line);
    while (length && isspace((unsigned char)line[length - 1])) line[--length] = 0;
    return line;
}

hls_manifest_type_t hls_parse_manifest(char *text, hls_manifest_t *manifest)
{
    memset(manifest, 0, sizeof(*manifest));
    if (!strncmp(text, "\xef\xbb\xbf", 3)) text += 3;
    char *line = next_line(&text);
    if (!line || strcmp(line, "#EXTM3U")) return HLS_MANIFEST_INVALID;
    bool master = false, want_variant = false, want_segment = false, discontinuity = false;
    hls_attribute_t alternate = {0};
    const char *variant = NULL;
    bool default_audio = false;
    while ((line = next_line(&text))) {
        if (!*line) continue;
        if (!strncmp(line, "#EXT-X-MEDIA:", 13)) {
            const char *attrs = line + 13;
            bool is_default = attribute_is(attribute(attrs, "DEFAULT"), "YES");
            hls_attribute_t uri = attribute(attrs, "URI");
            if (attribute_is(attribute(attrs, "TYPE"), "AUDIO") && uri.length &&
                (!alternate.value || (is_default && !default_audio))) {
                alternate = uri;
                default_audio = is_default;
            }
            master = true;
        } else if (!strncmp(line, "#EXT-X-STREAM-INF:", 18)) {
            master = true;
            want_variant = !variant && audio_variant(line + 18);
        } else if (!strncmp(line, "#EXT-X-TARGETDURATION:", 22)) {
            uint64_t seconds;
            if (!parse_number(line + 22, &seconds) || !seconds || seconds > UINT_MAX)
                return HLS_MANIFEST_INVALID;
            manifest->target_seconds = (unsigned)seconds;
        } else if (!strncmp(line, "#EXT-X-MEDIA-SEQUENCE:", 22)) {
            if (!parse_number(line + 22, &manifest->sequence)) return HLS_MANIFEST_INVALID;
        } else if (!strncmp(line, "#EXTINF:", 8)) {
            want_segment = true;
        } else if (!strcmp(line, "#EXT-X-DISCONTINUITY")) {
            discontinuity = true;
        } else if (!strcmp(line, "#EXT-X-ENDLIST")) {
            manifest->end_list = true;
        } else if (!strncmp(line, "#EXT-X-MAP:", 11) || !strncmp(line, "#EXT-X-BYTERANGE:", 17) ||
                   !strncmp(line, "#EXT-X-I-FRAMES-ONLY", 20) ||
                   (!strncmp(line, "#EXT-X-KEY:", 11) &&
                    !attribute_is(attribute(line + 11, "METHOD"), "NONE"))) {
            return HLS_MANIFEST_UNSUPPORTED;
        } else if (line[0] != '#') {
            if (want_segment) {
                if (manifest->count == HLS_MAX_SEGMENTS) return HLS_MANIFEST_UNSUPPORTED;
                manifest->segments[manifest->count++] = (hls_segment_t){line, discontinuity};
                want_segment = discontinuity = false;
            } else if (want_variant) {
                variant = line;
                want_variant = false;
            }
        }
    }
    if (master) {
        if (alternate.value) {
            /* Attribute values are slices of the mutable manifest buffer. */
            ((char *)alternate.value)[alternate.length] = 0;
            variant = alternate.value;
        }
        manifest->variant = variant;
        return variant ? HLS_MANIFEST_MASTER : HLS_MANIFEST_UNSUPPORTED;
    }
    if (!manifest->count || !manifest->target_seconds || want_segment ||
        manifest->sequence > UINT64_MAX - manifest->count) return HLS_MANIFEST_INVALID;
    return HLS_MANIFEST_MEDIA;
}

static size_t scheme_size(const char *url)
{
    if (!strncasecmp(url, "https://", 8)) return 8;
    return !strncasecmp(url, "http://", 7) ? 7 : 0;
}

static void normalize_path(char *path)
{
    char *source = path, *destination = path;
    while (*source && *source != '?') {
        if (!strncmp(source, "/./", 3) || !strncmp(source, "/../", 4)) {
            bool parent = source[2] == '.';
            source += parent ? 3 : 2;
            if (parent && destination > path) {
                do { destination--; } while (destination > path && *destination != '/');
            }
        } else if ((!strncmp(source, "/.", 2) && (source[2] == '?' || !source[2])) ||
                   (!strncmp(source, "/..", 3) && (source[3] == '?' || !source[3]))) {
            bool parent = source[2] == '.';
            source += parent ? 3 : 2;
            if (parent && destination > path) {
                do { destination--; } while (destination > path && *destination != '/');
            }
            *destination++ = '/';
        } else {
            *destination++ = *source++;
            while (*source && *source != '/' && *source != '?') *destination++ = *source++;
        }
    }
    memmove(destination, source, strlen(source) + 1);
}

bool hls_resolve_uri(const char *base, const char *reference, char *url, size_t capacity)
{
    size_t reference_size = strcspn(reference, "#");
    size_t scheme = scheme_size(base);
    if (!scheme || !reference_size || reference_size >= capacity ||
        capacity > INT_MAX) return false;
    for (size_t i = 0; i < reference_size; i++) {
        unsigned char character = reference[i];
        if (character <= ' ' || character == 127 || character == '\\') return false;
    }
    const char *origin_end = base + scheme + strcspn(base + scheme, "/?#");
    if (origin_end == base + scheme) return false;
    size_t prefix = 0;
    bool add_slash = false;
    if (!scheme_size(reference)) {
        if (reference[0] == '/' && reference[1] == '/') {
            prefix = scheme - 2;
        } else {
            size_t first_part = strcspn(reference, "/?#");
            if (memchr(reference, ':', first_part)) return false;
            prefix = origin_end - base;
            if (reference[0] != '/') {
                size_t base_path_end = prefix + strcspn(origin_end, "?#");
                if (reference[0] == '?') {
                    prefix = base_path_end;
                } else {
                    while (base_path_end > prefix && base[base_path_end - 1] != '/') base_path_end--;
                    add_slash = base_path_end == prefix;
                    prefix = base_path_end;
                }
            }
        }
    }
    if (prefix >= capacity) return false;
    int length = snprintf(url, capacity, "%.*s%s%.*s", (int)prefix, base,
                          add_slash ? "/" : "", (int)reference_size, reference);
    if (length < 0 || (size_t)length >= capacity) return false;
    scheme = scheme_size(url);
    char *path = url + scheme + strcspn(url + scheme, "/?");
    if (!scheme || path == url + scheme) return false;
    normalize_path(path);
    return true;
}

size_t hls_start_segment(const hls_manifest_t *manifest)
{
    if (manifest->end_list) return 0;
    size_t first = manifest->count > 3 ? manifest->count - 3 : 0;
    for (size_t i = first; i < manifest->count; i++)
        if (manifest->segments[i].discontinuity) first = i;
    return first;
}

#ifndef TANRADIO_HLS_MANIFEST_H
#define TANRADIO_HLS_MANIFEST_H

#include <stdbool.h>
#include "radio/stream_format.h"

#define HLS_MAX_SEGMENTS 64

typedef enum {
    HLS_MANIFEST_INVALID,
    HLS_MANIFEST_UNSUPPORTED,
    HLS_MANIFEST_MASTER,
    HLS_MANIFEST_MEDIA,
} hls_manifest_type_t;

typedef struct {
    const char *uri; /* Points inside the mutable manifest buffer. */
    bool discontinuity;
} hls_segment_t;

typedef struct {
    const char *variant;
    uint64_t sequence;
    unsigned target_seconds;
    size_t count;
    bool end_list;
    hls_segment_t segments[HLS_MAX_SEGMENTS];
} hls_manifest_t;

hls_manifest_type_t hls_parse_manifest(char *text, hls_manifest_t *manifest);
bool hls_resolve_uri(const char *base, const char *reference, char *url, size_t capacity);
size_t hls_start_segment(const hls_manifest_t *manifest);

#endif

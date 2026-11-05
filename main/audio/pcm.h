#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Native rates supported by the initial format-aware playback path. */
bool audio_pcm_supported(uint32_t rate, unsigned bits, unsigned channels);
/* Convert a complete mono/stereo S16 frame block into interleaved stereo.
 * Returns stereo frames produced; rejects partial frames or short output. */
size_t audio_pcm_stereo(const void *input, size_t bytes, unsigned channels,
                        int16_t *output, size_t frame_capacity);

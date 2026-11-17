#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Native rates supported by the initial format-aware playback path. */
bool audio_pcm_supported(uint32_t rate, unsigned bits, unsigned channels);
/* Validated source formats use native 44.1/48 kHz or upsample to 48 kHz. */
uint32_t audio_pcm_output_rate(uint32_t rate, unsigned bits, unsigned channels);
/* Convert a complete mono/stereo S16 frame block into interleaved stereo.
 * Returns stereo frames produced; rejects partial frames or short output. */
size_t audio_pcm_stereo(const void *input, size_t bytes, unsigned channels,
                        int16_t *output, size_t frame_capacity);

typedef struct {
    uint32_t input_rate;
    uint32_t phase;
    int16_t previous[2];
    bool primed;
} audio_pcm_resampler_t;

/* Convert S16 stereo to 48 kHz. Supply at least ceil(48000/rate) output
 * frames per input frame. Keep the state for the duration of the format. */
size_t audio_pcm_resample(audio_pcm_resampler_t *state, const int16_t *input,
                          size_t frames, int16_t *output, size_t capacity);

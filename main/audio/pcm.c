#include "pcm.h"
#include <string.h>

bool audio_pcm_supported(uint32_t rate, unsigned bits, unsigned channels)
{
    return (rate == 44100 || rate == 48000) && bits == 16 &&
           (channels == 1 || channels == 2);
}

uint32_t audio_pcm_output_rate(uint32_t rate, unsigned bits, unsigned channels)
{
    if (audio_pcm_supported(rate, bits, channels)) return rate;
    if (bits == 16 && (channels == 1 || channels == 2) && rate >= 8000 && rate < 48000)
        return 48000;
    return 0;
}

size_t audio_pcm_stereo(const void *input, size_t bytes, unsigned channels,
                        int16_t *output, size_t frame_capacity)
{
    if (!input || !output || (channels != 1 && channels != 2) ||
        bytes % (channels * sizeof(int16_t))) return 0;
    size_t frames = bytes / (channels * sizeof(int16_t));
    if (frames > frame_capacity) return 0;
    if (channels == 2) {
        memcpy(output, input, bytes);
    } else {
        const uint8_t *source = input;
        for (size_t i = 0; i < frames; i++) {
            int16_t sample;
            memcpy(&sample, source + i * sizeof(sample), sizeof(sample));
            output[2 * i] = sample;
            output[2 * i + 1] = sample;
        }
    }
    return frames;
}

size_t audio_pcm_resample(audio_pcm_resampler_t *state, const int16_t *input,
                          size_t frames, int16_t *output, size_t capacity)
{
    const uint32_t output_rate = 48000;
    if (!state || !input || !output || !frames) return 0;
    uint32_t rate = state->input_rate;
    if (rate < 8000 || rate >= output_rate ||
        frames > capacity / ((output_rate + rate - 1) / rate)) return 0;
    size_t written = 0;
    for (size_t i = 0; i < frames; i++, input += 2) {
        if (!state->primed) {
            state->previous[0] = input[0];
            state->previous[1] = input[1];
            state->primed = true;
        }
        while (state->phase < output_rate) {
            int32_t right = (int32_t)state->phase;
            int32_t left = (int32_t)output_rate - right;
            for (unsigned channel = 0; channel < 2; channel++) {
                int32_t sample = left * state->previous[channel] + right * input[channel];
                output[2 * written + channel] = (int16_t)(sample / (int32_t)output_rate);
            }
            written++;
            state->phase += rate;
        }
        state->phase -= output_rate;
        state->previous[0] = input[0];
        state->previous[1] = input[1];
    }
    return written;
}

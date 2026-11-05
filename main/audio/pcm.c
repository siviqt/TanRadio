#include "pcm.h"
#include <string.h>

bool audio_pcm_supported(uint32_t rate, unsigned bits, unsigned channels)
{
    return (rate == 44100 || rate == 48000) && bits == 16 &&
           (channels == 1 || channels == 2);
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

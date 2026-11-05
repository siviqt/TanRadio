/******************************************************************************
 *
 * Project : TanRadio
 * Module  : audio
 * File    : audio.h
 *
 * Purpose :
 *     Public interface for the audio module.
 *
 ******************************************************************************/

#ifndef TANRADIO_MAIN_AUDIO_AUDIO_H
#define TANRADIO_MAIN_AUDIO_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/**
 * @brief Initialize the audio subsystem.
 */
void audio_init(void);

typedef enum { AUDIO_OUTPUT_SPEAKER, AUDIO_OUTPUT_HEADPHONES } audio_output_t;
typedef struct {
    audio_output_t output;
    uint8_t volume;
} audio_status_t;

/** Obtain a consistent output/volume snapshot for the UI. */
void audio_get_status(audio_status_t *out_status);
/** Re-read the jack and apply the corresponding saved volume. */
esp_err_t audio_refresh_output(void);
/** Quiesce I2S and configure a supported PCM sample rate; remains muted. */
esp_err_t audio_set_sample_rate(uint32_t rate);

/**
 * @brief Increase application-tracked audio volume by 1%, up to 100%.
 */
esp_err_t audio_volume_up(void);

/**
 * @brief Decrease application-tracked audio volume by 1%, down to 0%.
 */
esp_err_t audio_volume_down(void);

/**
 * @brief Get the application-tracked audio volume.
 *
 * @return Current volume percentage, 0-100.
 */
uint8_t audio_get_volume(void);

/**
 * @brief Write decoded PCM samples to the BSP-owned audio output.
 *
 * @param data PCM sample buffer.
 * @param size Number of PCM bytes to write.
 * @return ESP_OK on success, otherwise an ESP-IDF error.
 */
esp_err_t audio_write_pcm(const void *data, size_t size);

/**
 * @brief Mute the audio codec without changing the configured volume.
 */
esp_err_t audio_mute(void);

/**
 * @brief Restore the configured volume after a temporary mute.
 */
esp_err_t audio_unmute(void);

/**
 * @brief Permanently silence output before returning to the launcher.
 * Blocks subsequent PCM writes and volume changes until restart.
 */
void audio_shutdown(void);

#endif /* TANRADIO_MAIN_AUDIO_AUDIO_H */

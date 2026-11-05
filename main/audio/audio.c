/* TanRadio audio control. BSP owns the codec and I2S channel. */
#include <stdbool.h>
#include <stdatomic.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "bsp/audio.h"
#include "bsp/input.h"
#include "nvs.h"
#include "audio.h"

static const char *TAG = "tanradio.audio";
static i2s_chan_handle_t i2s_handle;
static uint8_t audio_volumes[2] = {50, 50};
static audio_output_t audio_output = AUDIO_OUTPUT_SPEAKER;
static bool audio_output_ready;
static bool audio_muted = true;
static uint32_t audio_sample_rate = 44100;
static bool audio_i2s_running = true;
static SemaphoreHandle_t audio_control_mutex;
static SemaphoreHandle_t audio_write_mutex;
static SemaphoreHandle_t audio_settings_mutex;
static atomic_bool audio_shutdown_started;
static TickType_t audio_settings_changed;
static unsigned audio_settings_generation, audio_saved_generation;

/* All state and codec operations below are protected by the control mutex.
 * Persistence never holds it during flash operations. */
static void audio_load_settings(void)
{
    nvs_handle_t handle;
    if (nvs_open("tanradio_audio", NVS_READONLY, &handle) != ESP_OK) return;
    const char *keys[] = {"speaker", "headphones"};
    for (unsigned i = 0; i < 2; i++) {
        uint8_t value;
        if (nvs_get_u8(handle, keys[i], &value) == ESP_OK && value <= 100)
            audio_volumes[i] = value;
    }
    nvs_close(handle);
}

static void audio_save_settings(bool force)
{
    if (!audio_settings_mutex || !audio_control_mutex) return;
    xSemaphoreTake(audio_settings_mutex, portMAX_DELAY);
    xSemaphoreTake(audio_control_mutex, portMAX_DELAY);
    unsigned generation = audio_settings_generation;
    bool due = generation != audio_saved_generation &&
        (force || (TickType_t)(xTaskGetTickCount() - audio_settings_changed) >= pdMS_TO_TICKS(1500));
    uint8_t speaker = audio_volumes[0], headphones = audio_volumes[1];
    xSemaphoreGive(audio_control_mutex);
    if (due) {
        nvs_handle_t handle;
        esp_err_t res = nvs_open("tanradio_audio", NVS_READWRITE, &handle);
        if (res == ESP_OK) {
            res = nvs_set_u8(handle, "speaker", speaker);
            if (res == ESP_OK) res = nvs_set_u8(handle, "headphones", headphones);
            if (res == ESP_OK) res = nvs_commit(handle);
            nvs_close(handle);
        }
        xSemaphoreTake(audio_control_mutex, portMAX_DELAY);
        if (res == ESP_OK) audio_saved_generation = generation;
        else audio_settings_changed = xTaskGetTickCount(); /* Retry without a tight flash loop. */
        xSemaphoreGive(audio_control_mutex);
        if (res != ESP_OK) ESP_LOGW(TAG, "Volume settings save failed: %s", esp_err_to_name(res));
    }
    xSemaphoreGive(audio_settings_mutex);
}

esp_err_t audio_refresh_output(void)
{
    if (!audio_control_mutex) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(audio_control_mutex, portMAX_DELAY);
    if (atomic_load(&audio_shutdown_started)) {
        xSemaphoreGive(audio_control_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    bool headphones = false;
    esp_err_t res = bsp_input_read_action(BSP_INPUT_ACTION_TYPE_AUDIO_JACK, &headphones);
    if (res == ESP_OK) {
        audio_output_t next = headphones ? AUDIO_OUTPUT_HEADPHONES : AUDIO_OUTPUT_SPEAKER;
        if (!audio_output_ready || next != audio_output) {
            audio_output_ready = false;
            audio_output = next;
            res = bsp_audio_set_volume(0);
            /* Always attempt amplifier disable, even if the codec mute failed. */
            esp_err_t amp_res = bsp_audio_set_amplifier(false);
            if (res == ESP_OK) res = amp_res;
            if (res == ESP_OK) res = bsp_audio_set_amplifier_force(false);
            if (res == ESP_OK) res = bsp_audio_set_volume(audio_muted ? 0 : audio_volumes[next]);
            if (res == ESP_OK) res = bsp_audio_set_amplifier(!headphones);
            if (res == ESP_OK) {
                audio_output_ready = true;
                ESP_LOGI(TAG, "Output=%s volume=%u%%", headphones ? "headphones" : "speaker", audio_volumes[next]);
            } else {
                (void)bsp_audio_set_volume(0);
                (void)bsp_audio_set_amplifier(false);
            }
        }
    } else {
        /* Unknown jack state must not retain an audible stale route. */
        audio_output_ready = false;
        (void)bsp_audio_set_volume(0);
        (void)bsp_audio_set_amplifier(false);
    }
    xSemaphoreGive(audio_control_mutex);
    return res;
}

static void audio_maintenance_task(void *arg)
{
    (void)arg;
    while (!atomic_load(&audio_shutdown_started)) {
        (void)audio_refresh_output();
        audio_save_settings(false);
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    vTaskDelete(NULL);
}

void audio_init(void)
{
    audio_control_mutex = xSemaphoreCreateMutex();
    audio_write_mutex = xSemaphoreCreateMutex();
    audio_settings_mutex = xSemaphoreCreateMutex();
    if (!audio_control_mutex || !audio_write_mutex || !audio_settings_mutex) {
        ESP_LOGE(TAG, "Failed to allocate audio mutexes");
        return;
    }
    if (bsp_audio_get_i2s_handle(&i2s_handle) != ESP_OK || !i2s_handle) {
        ESP_LOGE(TAG, "BSP audio channel unavailable");
        return;
    }
    audio_load_settings();
    (void)bsp_audio_set_volume(0);
    (void)bsp_audio_set_amplifier(false);
    (void)bsp_audio_set_amplifier_force(false);
    (void)audio_refresh_output();
    if (xTaskCreate(audio_maintenance_task, "tanradio_levels", 4096, NULL, 2, NULL) != pdPASS)
        ESP_LOGE(TAG, "Failed to start audio settings/jack service");
}

static esp_err_t audio_adjust_volume(int delta)
{
    if (!audio_control_mutex) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(audio_control_mutex, portMAX_DELAY);
    esp_err_t res = ESP_ERR_INVALID_STATE;
    if (!atomic_load(&audio_shutdown_started)) {
        int next = audio_volumes[audio_output] + delta;
        if (next < 0) next = 0;
        if (next > 100) next = 100;
        res = audio_muted || !audio_output_ready ? ESP_OK : bsp_audio_set_volume(next);
        if (res == ESP_OK && next != audio_volumes[audio_output]) {
            audio_volumes[audio_output] = next;
            audio_settings_changed = xTaskGetTickCount();
            audio_settings_generation++;
            ESP_LOGI(TAG, "Volume: %u%% output=%s", next,
                     audio_output == AUDIO_OUTPUT_HEADPHONES ? "headphones" : "speaker");
        }
    }
    xSemaphoreGive(audio_control_mutex);
    return res;
}

esp_err_t audio_volume_up(void) { return audio_adjust_volume(1); }
esp_err_t audio_volume_down(void) { return audio_adjust_volume(-1); }

void audio_get_status(audio_status_t *out)
{
    if (!out) return;
    if (audio_control_mutex) xSemaphoreTake(audio_control_mutex, portMAX_DELAY);
    *out = (audio_status_t){audio_output, audio_volumes[audio_output]};
    if (audio_control_mutex) xSemaphoreGive(audio_control_mutex);
}

uint8_t audio_get_volume(void)
{
    audio_status_t status;
    audio_get_status(&status);
    return status.volume;
}

static esp_err_t audio_set_muted(bool muted)
{
    if (!audio_control_mutex) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(audio_control_mutex, portMAX_DELAY);
    esp_err_t res = ESP_ERR_INVALID_STATE;
    if (!atomic_load(&audio_shutdown_started)) {
        res = bsp_audio_set_volume(muted || !audio_output_ready ? 0 : audio_volumes[audio_output]);
        if (res == ESP_OK) audio_muted = muted;
    }
    xSemaphoreGive(audio_control_mutex);
    return res;
}
esp_err_t audio_mute(void) { return audio_set_muted(true); }
esp_err_t audio_unmute(void) { return audio_set_muted(false); }

esp_err_t audio_set_sample_rate(uint32_t rate)
{
    if (rate != 44100 && rate != 48000) return ESP_ERR_NOT_SUPPORTED;
    if (!audio_control_mutex || !audio_write_mutex) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(audio_control_mutex, portMAX_DELAY);
    esp_err_t res = ESP_ERR_INVALID_STATE;
    if (!atomic_load(&audio_shutdown_started) && i2s_handle) {
        res = bsp_audio_set_volume(0);
        if (res == ESP_OK) audio_muted = true;
        xSemaphoreTake(audio_write_mutex, portMAX_DELAY);
        /* A failed enable must remain recoverable on the next play request. */
        if (res == ESP_OK && !audio_i2s_running) {
            res = i2s_channel_enable(i2s_handle);
            audio_i2s_running = res == ESP_OK;
        }
        /* Retire previous-stream samples in both DMA rings before restart. */
        i2s_chan_info_t info = {0};
        if (res == ESP_OK) res = i2s_channel_get_info(i2s_handle, &info);
        static const uint8_t silence[1024] = {0};
        size_t remaining = 2 * info.total_dma_buf_size;
        while (res == ESP_OK && remaining) {
            size_t size = remaining < sizeof(silence) ? remaining : sizeof(silence), written = 0;
            res = i2s_channel_write(i2s_handle, silence, size, &written, 100);
            if (res == ESP_OK && written != size) res = ESP_ERR_TIMEOUT;
            if (res == ESP_OK) remaining -= written;
        }
        if (res == ESP_OK && rate != audio_sample_rate) {
            res = i2s_channel_disable(i2s_handle);
            if (res == ESP_OK) {
                audio_i2s_running = false;
                res = bsp_audio_set_rate(rate);
                if (res == ESP_OK) audio_sample_rate = rate;
                esp_err_t enabled = i2s_channel_enable(i2s_handle);
                audio_i2s_running = enabled == ESP_OK;
                if (res == ESP_OK) res = enabled;
            }
        }
        xSemaphoreGive(audio_write_mutex);
    }
    xSemaphoreGive(audio_control_mutex);
    return res;
}

static esp_err_t audio_write_pcm_locked(const void *data, size_t size)
{
    if (i2s_handle == NULL) {
        ESP_LOGE(TAG, "Audio I2S handle is not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (data == NULL || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t bytes_written = 0;

    esp_err_t res = i2s_channel_write(
        i2s_handle,
        data,
        size,
        &bytes_written,
        1000);

    if (res != ESP_OK) {
        ESP_LOGE(TAG,
                 "Failed to write PCM to I2S: %s",
                 esp_err_to_name(res));
        return res;
    }

    if (bytes_written != size) {
        ESP_LOGW(TAG,
                 "Partial PCM write: %zu of %zu bytes",
                 bytes_written,
                 size);
    }

    return ESP_OK;
}

esp_err_t audio_write_pcm(const void *data, size_t size)
{
    if (audio_write_mutex == NULL || atomic_load(&audio_shutdown_started)) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(audio_write_mutex, portMAX_DELAY);
    esp_err_t res = atomic_load(&audio_shutdown_started)
                        ? ESP_ERR_INVALID_STATE
                        : audio_write_pcm_locked(data, size);
    xSemaphoreGive(audio_write_mutex);
    return res;
}

static void audio_shutdown_check(esp_err_t res, const char *operation)
{
    if (res != ESP_OK) {
        ESP_LOGW(TAG, "Shutdown %s failed: %s", operation, esp_err_to_name(res));
    }
}

void audio_shutdown(void)
{
    /* Close admission before waiting for an in-flight write. Otherwise the
     * higher-priority PCM task could continually reacquire the write lock. */
    atomic_store(&audio_shutdown_started, true);
    if (audio_control_mutex != NULL) {
        xSemaphoreTake(audio_control_mutex, portMAX_DELAY);
    }
    audio_muted = true;
    audio_output_ready = false;

    /* Attempt every safety step even if a preceding hardware call fails.
     * The amplifier is coprocessor-owned and survives the CPU restart. */
    audio_shutdown_check(bsp_audio_set_volume(0.0f), "codec mute");
    audio_shutdown_check(bsp_audio_set_amplifier(false), "amplifier disable");
    audio_shutdown_check(bsp_audio_set_amplifier_force(false), "amplifier force release");

    if (audio_write_mutex != NULL) {
        xSemaphoreTake(audio_write_mutex, portMAX_DELAY);
    }
    if (i2s_handle != NULL) {
        i2s_chan_info_t info = {0};
        esp_err_t res = i2s_channel_get_info(i2s_handle, &info);
        audio_shutdown_check(res, "I2S information");
        if (res == ESP_OK) {
            static const uint8_t silence[1024] = {0};
            /* Two full DMA rings overwrite stale PCM and clock silence out.
             * No playback writer can enter while we hold the write mutex. */
            size_t remaining = 2 * info.total_dma_buf_size;
            while (remaining > 0) {
                size_t size = remaining < sizeof(silence) ? remaining : sizeof(silence);
                size_t written = 0;
                res = i2s_channel_write(i2s_handle, silence, size, &written, 100);
                if (res != ESP_OK || written != size) {
                    audio_shutdown_check(res != ESP_OK ? res : ESP_ERR_TIMEOUT,
                                         "silence flush");
                    break;
                }
                remaining -= written;
            }
        }
        esp_err_t disable_res = i2s_channel_disable(i2s_handle);
        audio_shutdown_check(disable_res, "I2S disable");
        if (disable_res == ESP_OK) {
            /* The BSP owns the channel; do not delete it. */
            audio_i2s_running = false;
            i2s_handle = NULL;
        }
    }
    if (audio_write_mutex != NULL) {
        xSemaphoreGive(audio_write_mutex);
    }
    if (audio_control_mutex != NULL) {
        xSemaphoreGive(audio_control_mutex);
    }
    audio_save_settings(true);
}

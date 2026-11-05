#include "external_display/driver.h"

#include <stdatomic.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

typedef struct {
    external_display_id_t id;
    uint8_t contrast;
    unsigned configuration;
    external_frame_t frame;
} display_request_t;

static const external_driver_t *const drivers[] = {
    [EXTERNAL_DISPLAY_NOKIA5110] = &external_nokia5110,
};

static QueueHandle_t mailbox;
static display_request_t submitted;
static atomic_int status = EXTERNAL_DISABLED;
static atomic_uint completed_configuration;

static void display_worker(void *arg)
{
    (void)arg;
    const external_driver_t *driver = NULL;
    unsigned applied = 0;
    display_request_t request = {0};
    TickType_t wait = portMAX_DELAY;
    for (;;) {
        bool received = xQueueReceive(mailbox, &request, wait) == pdTRUE;
        if (!received && wait == portMAX_DELAY) continue;
        if (received && request.configuration != applied) {
            if (driver) driver->close();
            applied = request.configuration;
            driver = NULL;
            if (request.id == EXTERNAL_DISPLAY_OFF) {
                atomic_store(&status, EXTERNAL_DISABLED);
            } else {
                atomic_store(&status, EXTERNAL_STARTING);
                if ((unsigned)request.id < sizeof(drivers) / sizeof(drivers[0]))
                    driver = drivers[request.id];
                if (driver && !driver->open(request.contrast)) {
                    driver->close();
                    driver = NULL;
                }
                atomic_store(&status, driver ? EXTERNAL_INITIALIZED : EXTERNAL_FAILED);
            }
        }
        uint32_t refresh_ms = 0;
        uint32_t now_ms = (uint32_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (driver && !driver->present(&request.frame, now_ms, &refresh_ms)) {
            driver->close();
            driver = NULL;
            atomic_store(&status, EXTERNAL_FAILED);
        }
        wait = driver && refresh_ms ? pdMS_TO_TICKS(refresh_ms) : portMAX_DELAY;
        if (!wait) wait = 1;
        atomic_store(&completed_configuration, applied);
    }
}

void external_display_submit(external_display_id_t id, uint8_t contrast,
                             const external_frame_t *frame, bool retry)
{
    bool configure = retry || id != submitted.id || contrast != submitted.contrast;
    if (!mailbox) {
        if (id == EXTERNAL_DISPLAY_OFF) {
            submitted.id = id;
            submitted.contrast = contrast;
            atomic_store(&status, EXTERNAL_DISABLED);
            return;
        }
        if (!configure && atomic_load(&status) == EXTERNAL_FAILED) return;
        submitted.id = id;
        submitted.contrast = contrast;
        mailbox = xQueueCreate(1, sizeof(display_request_t));
        if (!mailbox || xTaskCreate(display_worker, "tanradio_display", 4096, NULL, 1, NULL) != pdPASS) {
            if (mailbox) vQueueDelete(mailbox);
            mailbox = NULL;
            atomic_store(&status, EXTERNAL_FAILED);
            return;
        }
        configure = true;
    }
    if (!configure && !memcmp(&submitted.frame, frame, sizeof(*frame))) return;
    if (configure) {
        ++submitted.configuration;
        submitted.id = id;
        submitted.contrast = contrast;
    }
    submitted.frame = *frame;
    xQueueOverwrite(mailbox, &submitted);
}

external_display_status_t external_display_status(void)
{
    return atomic_load(&status);
}

void external_display_stop(void)
{
    if (!mailbox) return;
    const external_frame_t blank = {0};
    external_display_submit(EXTERNAL_DISPLAY_OFF, 0, &blank, true);
    /* A GPIO update is bounded. Let it finish before releasing the port. */
    for (unsigned i = 0; i < 50 && atomic_load(&completed_configuration) != submitted.configuration; i++)
        vTaskDelay(pdMS_TO_TICKS(10));
}

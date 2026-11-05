/*
 * Project : TanRadio
 * Module  : util
 * File    : util.c
 *
 * Purpose :
 *   Common application utilities.
 */

/* ESP-IDF */
#include "esp_log.h"

/* Local header */
#include "util.h"

static const char *TAG = "tanradio.util";

/*******************************************************************************
 * Public Functions
 ******************************************************************************/

/**
 * @brief Initialize the util subsystem.
 */
void util_init(void)
{
    ESP_LOGI(TAG, "Util subsystem initialized");
}

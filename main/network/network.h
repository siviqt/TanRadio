/******************************************************************************
 *
 * Project : TanRadio
 * Module  : network
 * File    : network.h
 *
 * Purpose :
 *     Public interface for the network module.
 *
 ******************************************************************************/

#ifndef TANRADIO_MAIN_NETWORK_NETWORK_H
#define TANRADIO_MAIN_NETWORK_NETWORK_H

#include <stdbool.h>
#include <stddef.h>

/**
 * @brief Prepare the network stack and connect saved networks in background.
 */
void network_init(void);
void network_shutdown(void);

/**
 * @brief Return whether Wi-Fi is currently connected.
 */
bool network_is_connected(void);

/**
 * @brief Get the SSID of the currently connected Wi-Fi network.
 *
 * @param out_ssid Destination buffer.
 * @param out_size Size of the destination buffer.
 *
 * @return true when the connected SSID was retrieved.
 */
bool network_get_ssid(char *out_ssid, size_t out_size);

#endif /* TANRADIO_MAIN_NETWORK_NETWORK_H */

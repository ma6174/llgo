/* Shim for compiling IDF sources (the regulatory tables) against this
 * header tree without the esp_event/esp_netif component headers. The OSAL
 * and blob ABI only need the types. */
#ifndef SHIM_ESP_WIFI_H
#define SHIM_ESP_WIFI_H
#include "esp_wifi_types.h"
#include "esp_err.h"
#endif

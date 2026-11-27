/* Shim: esp_wifi_default.h prototypes reference esp_netif types; nothing in
 * this build calls them (default netif wiring is IDF glue). */
#ifndef SHIM_ESP_NETIF_H
#define SHIM_ESP_NETIF_H
typedef struct esp_netif_t esp_netif_t;
typedef struct esp_netif_inherent_config_t esp_netif_inherent_config_t;
#endif

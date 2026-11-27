/* esp_wifi public-API glue for the S31 blob on llgo.
 *
 * Most public esp_wifi_* functions live in the closed-source libs here
 * (checked against the pinned blob's symbol table: esp_wifi_set_mode,
 * esp_wifi_start/stop, esp_wifi_scan_start, esp_wifi_scan_get_ap_records/
 * ap_num, esp_wifi_set_config, esp_wifi_get_mac, esp_wifi_*_internal).
 * The pieces that IDF ships as open source and the blob therefore expects
 * from its host are provided below:
 *
 *   - esp_wifi_init/deinit: the public init path (IDF's wifi_init.c, minus
 *     the light-sleep/PM/PSRAM branches this OSAL does not have, and minus
 *     esp_supplicant_init - the S31 blob carries no supplicant, so WPA
 *     handshakes are out of P1 scope; init/scan do not need it),
 *   - esp_wifi_connect/disconnect public wrappers (libmesh references them),
 *   - esp_event_handler_register/unregister + WIFI_EVENT/SC_EVENT bases
 *     (the library posts WiFi events through them; the bridge routes into
 *     the Go event dispatcher in osal/events.go),
 *   - esp_mesh_send_event_internal: weak no-op (no mesh in P1),
 *   - regulatory tables: copied verbatim from the pinned IDF
 *     (components/esp_wifi/regulatory/esp_wifi_regulatory.c - the blob
 *     reads regdomain_table/regulatory_data directly),
 *   - the wifi_init_config_t default (WIFI_INIT_CONFIG_DEFAULT shape) and
 *     scan/sta-config builders so Go callers never see C struct layouts.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Console lines go through the device's retargeted write (fd 1); see
 * osi_osal.c for why the stdio FILE* path stays unused. */
static void osal_print_line(const char *s) {
    write(1, s, strlen(s));
}

#include "esp_private/wifi_os_adapter.h"
#include "esp_wifi_types.h"
/* The real esp_wifi.h (wifi_init_config_t + the CONFIG defaults this file
 * mirrors) lives next to the shimmed flat copy; quoted relative include so
 * the shim esp_wifi.h does not shadow it here. */
#include "../../idfparts/include/esp_wifi/esp_wifi.h"

/* Go-side bridge (bridge.go / wifi.go) */
extern int32_t llgo_osal_event_handler_register(const char *base, int32_t id,
                                                void *fn, void *arg);
extern int32_t llgo_osal_event_handler_unregister(const char *base, int32_t id,
                                                  void *fn, void *arg);

/* Closed-source entries the wrappers call (verified blob symbols). */
extern int esp_wifi_init_internal(const wifi_init_config_t *config);
extern int esp_wifi_deinit_internal(void);
extern int esp_wifi_connect_internal(void);
extern int esp_wifi_disconnect_internal(void);
extern int esp_wifi_set_mode(uint32_t mode);
extern int esp_wifi_start(void);
extern int esp_wifi_stop(void);
extern int esp_wifi_scan_start(const wifi_scan_config_t *config, bool block);
extern int esp_wifi_scan_get_ap_num(uint16_t *number);
extern int esp_wifi_scan_get_ap_records(uint16_t *number, wifi_ap_record_t *records);
extern int esp_wifi_set_config(uint32_t ifx, wifi_config_t *conf);
extern int esp_wifi_get_mac(uint32_t ifx, uint8_t mac[6]);
extern int esp_wifi_set_storage(uint32_t storage);

/* Regulatory tables from the pinned IDF (kept verbatim; see header). */
extern const wifi_regdomain_t regdomain_table[];
extern const wifi_regulatory_t regulatory_data[];

/* ---- event bases ---- */

/* esp_event bases: pointer VARIABLES whose value is a unique string address
 * (the IDF ESP_EVENT_DEFINE_BASE shape). The blob's compiled code reads the
 * symbol and compares base values, so the array form would hand it garbage -
 * this must stay a variable, not an array. */
#include "esp_event_base.h"
esp_event_base_t const WIFI_EVENT = "WIFI_EVENT";
esp_event_base_t const SC_EVENT = "SC_EVENT";

/* Base values for the Go side (event registration compares pointers). */
uintptr_t llgo_wifi_event_base(void) { return (uintptr_t)WIFI_EVENT; }

int esp_event_handler_register(const char *event_base, int32_t event_id,
                               void *handler, void *arg) {
    return llgo_osal_event_handler_register(event_base, event_id, handler, arg);
}

int esp_event_handler_unregister(const char *event_base, int32_t event_id,
                                 void *handler, void *arg) {
    return llgo_osal_event_handler_unregister(event_base, event_id, handler, arg);
}

void esp_mesh_send_event_internal(int evt_id, void *data) {
    /* mesh is not part of P1; libmesh references this symbol but a link that
     * never runs mesh code never reaches it. */
    (void)evt_id;
    (void)data;
}

/* ---- init config ---- */

/* g_wifi_default_wpa_crypto_funcs stand-in: correct size/version, NULL
 * vectors. The blob stores the table at init; without a supplicant (P1) it
 * is never called - a connect attempt would fail at the 4-way handshake,
 * which is exactly the documented P1 boundary. */
/* NULL crypto vectors: correct size/version but no implementations. The
 * blob stores the table at init; without a supplicant (P1) it is never
 * called - a connect attempt would fail at the 4-way handshake, which is
 * exactly the documented P1 boundary. */
static const wpa_crypto_funcs_t g_osal_wpa_crypto_funcs = {
    .size = sizeof(wpa_crypto_funcs_t),
    .version = 1,
};

/* The WIFI_INIT_CONFIG_DEFAULT macro from the pinned esp_wifi.h, expanded
 * through the same sdkconfig.h the blob was built against. The crypto
 * table is filled at call time (a struct copy of a variable is not a
 * static-initializer constant in C). */
static const wifi_init_config_t g_osal_wifi_default_config = {
    .osi_funcs = &g_wifi_osi_funcs,
    .wpa_crypto_funcs = {0},
    .static_rx_buf_num = CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM,
    .dynamic_rx_buf_num = CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM,
    .tx_buf_type = CONFIG_ESP_WIFI_TX_BUFFER_TYPE,
#ifdef CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM
    .static_tx_buf_num = CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM,
#else
    .static_tx_buf_num = 0,
#endif
    .dynamic_tx_buf_num = CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM,
    .rx_mgmt_buf_type = CONFIG_ESP_WIFI_DYNAMIC_RX_MGMT_BUF,
    .rx_mgmt_buf_num = CONFIG_ESP_WIFI_RX_MGMT_BUF_NUM_DEF,
    .cache_tx_buf_num = WIFI_CACHE_TX_BUFFER_NUM,
    .csi_enable = 0,
    .ampdu_rx_enable = 1,
    .ampdu_tx_enable = 1,
    .amsdu_tx_enable = 0,
    .nvs_enable = 0, /* NVS entries are P1 stubs; keep the blob off that path */
    .nano_enable = 0,
    .rx_ba_win = 6,
    .wifi_task_core_id = 0, /* single-hart scheduler */
    .beacon_max_len = WIFI_SOFTAP_BEACON_MAX_LEN,
    .mgmt_sbuf_num = CONFIG_ESP_WIFI_MGMT_SBUF_NUM,
    .feature_caps = 0, /* no SAE/OWE/GCMP/FTM/11R/CSI/... on P1 */
    .sta_disconnected_pm = false,
    .espnow_max_encrypt_num = 7,
    .tx_hetb_queue_num = 1,
    .dump_hesigb_enable = false,
    .privacy_enhancements = true,
    .rmac_auto_reset_int = 0,
    .wifi_task_stack_size = 3584, /* advisory: fiber stacks grow on demand */
    .magic = WIFI_INIT_CONFIG_MAGIC,
};

/* ---- public init path ---- */

static bool s_wifi_inited;

int esp_wifi_init(const wifi_init_config_t *config) {
    /* Cribbed from IDF wifi_init.c esp_wifi_init with every branch that
     * needs IDF-only infrastructure removed (psram check, light-sleep
     * params, tickless retention, beacon monitor, supplicant). Those
     * features are all off in this build's sdkconfig anyway. */
    if (s_wifi_inited) {
        return 0x1030; /* ESP_ERR_WIFI_INIT_STATE */
    }
    int r = esp_wifi_init_internal(config);
    if (r != 0) {
        char msg[48];
        snprintf(msg, sizeof(msg), "[osal] esp_wifi_init_internal failed: 0x%x\n", r);
        osal_print_line(msg);
        return r;
    }
    s_wifi_inited = true;
    return 0;
}

int esp_wifi_deinit(void) {
    if (!s_wifi_inited) {
        return 0x1030;
    }
    int r = esp_wifi_deinit_internal();
    if (r == 0) {
        s_wifi_inited = false;
    }
    return r;
}

/* Public connect wrappers (libmesh's reconnect path calls these). */
int esp_wifi_connect(void) { return esp_wifi_connect_internal(); }
int esp_wifi_disconnect(void) { return esp_wifi_disconnect_internal(); }

/* ---- Go-facing shims (wifi.go's linkname externs) ---- */

int llgo_wifi_init(void) {
    wifi_init_config_t cfg = g_osal_wifi_default_config;
    cfg.wpa_crypto_funcs = g_osal_wpa_crypto_funcs;
    return esp_wifi_init(&cfg);
}

int llgo_wifi_deinit(void) { return esp_wifi_deinit(); }

int llgo_wifi_connect(void) { return esp_wifi_connect_internal(); }
int llgo_wifi_disconnect(void) { return esp_wifi_disconnect_internal(); }

/* llgo_wifi_set_sta_config: build a wifi_sta_config_t from two C strings.
 * Zeroed defaults + SSID/password + PMF-capable (the emu's default AP is a
 * WPA2/WPA3 transition AP; PMF-capable lets a WPA3 handshake be attempted
 * once a supplicant lands; P1 stays at open/PSK-observable only). */
int llgo_wifi_set_sta_config(const char *ssid, const char *pass) {
    wifi_sta_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    if (ssid != 0) {
        strncpy((char *)cfg.ssid, ssid, sizeof(cfg.ssid));
    }
    if (pass != 0) {
        strncpy((char *)cfg.password, pass, sizeof(cfg.password));
    }
    cfg.scan_method = 0;           /* WIFI_FAST_SCAN */
    cfg.sort_method = 0;           /* WIFI_CONNECT_AP_BY_SIGNAL */
    cfg.threshold.rssi = -127;
    cfg.threshold.authmode = 0;    /* WIFI_AUTH_OPEN: accept anything */
    cfg.pmf_cfg.capable = true;
    cfg.pmf_cfg.required = false;
    cfg.listen_interval = 3;
    cfg.failure_retry_cnt = 3;
    return esp_wifi_set_config(0 /* WIFI_IF_STA */, (wifi_config_t *)&cfg);
}

/* llgo_wifi_scan_start: build a wifi_scan_config_t. channel==0 scans all
 * channels (the demo's gate-3 flow); active scan with default times. */
int llgo_wifi_scan_start(uint32_t channel, int32_t show_hidden, int32_t block) {
    wifi_scan_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.channel = (uint8_t)channel;
    cfg.show_hidden = show_hidden != 0;
    cfg.scan_type = 0; /* WIFI_SCAN_TYPE_ACTIVE */
    cfg.scan_time.active.min = 0;
    cfg.scan_time.active.max = 120;
    return esp_wifi_scan_start(&cfg, block != 0);
}

/* llgo_wifi_scan_records: fetch up to max full wifi_ap_record_t from the
 * blob and write the simplified view the Go API returns (the full record
 * contains bitfields and nested structs; the ABI stays on this side). */
typedef struct {
    uint8_t ssid[33];
    int8_t rssi;
    uint8_t channel;
    uint8_t authmode;
} llgo_wifi_ap_view_t;

int llgo_wifi_scan_records(uint32_t max, llgo_wifi_ap_view_t *out) {
    if (max > 32) {
        max = 32;
    }
    uint16_t number = (uint16_t)max;
    wifi_ap_record_t records[32];
    int r = esp_wifi_scan_get_ap_records(&number, records);
    if (r != 0) {
        return r << 16;
    }
    for (uint16_t i = 0; i < number; i++) {
        memset(&out[i], 0, sizeof(out[i]));
        strncpy((char *)out[i].ssid, (const char *)records[i].ssid, 32);
        out[i].rssi = records[i].rssi;
        out[i].channel = records[i].primary;
        out[i].authmode = (uint8_t)records[i].authmode;
    }
    return (int)number; /* count in the low half, rc 0 in the high half */
}

int llgo_wifi_scan_get_ap_num(uint16_t *n) {
    return esp_wifi_scan_get_ap_num(n);
}

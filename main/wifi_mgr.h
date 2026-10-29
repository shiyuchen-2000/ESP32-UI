/*
 * WiFi manager using SoftAP provisioning: when no network is configured the
 * device runs an open hotspot with a tiny HTTP page; the phone connects, opens
 * the page, types the WiFi name/password, and the device saves them and joins
 * the network. No device-side scan (which was unstable on this board). STA
 * auto-reconnects on boot; repeated failures fall back to provisioning.
 *
 * Runs independently of LVGL (events arrive on the default event loop task);
 * the UI polls state via the getters and wifi_mgr_consume_changed().
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    WIFI_MGR_DISABLED = 0,
    WIFI_MGR_PROVISIONING,   /* softAP active; phone fills the form */
    WIFI_MGR_CONNECTING,     /* STA connecting */
    WIFI_MGR_CONNECTED,
    WIFI_MGR_FAILED,
} wifi_mgr_state_t;

/* Init NVS + WiFi + events. Enters provisioning if no saved network, else
 * auto-connects in STA mode. */
void wifi_mgr_init(void);

/* True when credentials for a previous network are stored. */
bool wifi_mgr_has_saved_creds(void);

/* Provisioning hotspot name, e.g. "ESP_Setup_ABCD". */
const char *wifi_mgr_prov_ap_name(void);

/* From the UI: stop STA and run the provisioning softAP + HTTP page. */
void wifi_mgr_start_provisioning(void);

/* Stop provisioning and reconnect to the saved network (no-op if none). */
void wifi_mgr_stop_provisioning(void);

/* Save credentials and join the network (called by the HTTP page and the UI). */
void wifi_mgr_connect(const char *ssid, const char *password);

/* Erase saved credentials and enter provisioning mode. */
void wifi_mgr_forget(void);

wifi_mgr_state_t wifi_mgr_get_state(void);
bool wifi_mgr_is_connected(void);
bool wifi_mgr_get_current_ssid(char *out, size_t len);

/* User on/off switch (persisted in NVS, default on). Disabling stops the
 * radio; enabling reconnects to the saved network or starts provisioning. */
void wifi_mgr_set_enabled(bool enabled);
bool wifi_mgr_is_enabled(void);

/* True once SNTP has synced the system clock. */
bool wifi_mgr_is_ntp_synced(void);

/* The UI polls this: true once since the previous call (cleared on read). */
bool wifi_mgr_consume_changed(void);

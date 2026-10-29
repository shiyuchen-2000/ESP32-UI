/*
 * BLE link for the MotoNav navigation app.
 *
 * Exposes the Nordic UART Service UUIDs the phone app already speaks:
 *   service 6e400001-b5a3-f393-e0a9-e50e24dcca9e
 *   RX      6e400002-...  (phone -> watch, write / write-no-response)
 *   TX      6e400003-...  (watch -> phone, notify)
 *
 * Writes are intentionally unencrypted for the first testing version, so the
 * phone needs no pairing. On subscribe the watch sends "U,1" (×3) which makes
 * the app mark the link verified and resend its state.
 */
#pragma once

#include <stdbool.h>

void nav_ble_init(void);

/* Notify the phone. Safe from the NimBLE host task; silently drops the text
 * when there is no connection/subscription. */
void nav_ble_notify(const char *text);

bool nav_ble_is_connected(void);

/* User on/off switch (persisted in NVS, default on). Off stops advertising
 * and drops the phone; on makes the watch discoverable again. */
void nav_ble_set_enabled(bool enabled);
bool nav_ble_is_enabled(void);

/* Advertised device name, e.g. "MotoNav-1572". */
const char *nav_ble_get_name(void);

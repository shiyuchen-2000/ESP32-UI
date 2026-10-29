#pragma once

#include <stdbool.h>

struct _lv_obj_t;

/* Build the WiFi page into the given parent (the SEC_WIFI settings detail
 * view). Ownership of created objects stays with the parent. */
void ui_wifi_build(struct _lv_obj_t *parent);

/* True while the password sub-page is showing (used for back routing). */
bool ui_wifi_is_in_password(void);

/* Go back one level inside the WiFi page (password -> network list). */
void ui_wifi_back(void);

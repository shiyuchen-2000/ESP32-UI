#pragma once

#include <stdbool.h>

/* Create the Settings app screen (waterfall list of rounded entry cards).
 * Ownership passes to ui_shell. */
struct _lv_obj_t *ui_settings_create(int id);

/* Settings owns its internal navigation (a WiFi password sub-page first, then
 * a detail page back to the list). The shell calls these through the app
 * registry's can_back()/back() hooks and never needs to know the internals. */
bool ui_settings_can_back(void);
void ui_settings_do_back(void);

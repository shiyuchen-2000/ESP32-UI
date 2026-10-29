#pragma once

#include <stdbool.h>

/* Create the Clock app screen (immersive switchable watch faces). Ownership
 * passes to ui_shell. */
struct _lv_obj_t *ui_clock_create(int id);

/* While the face editor is open, back (edge swipe) closes it without saving;
 * the shell calls these through the app registry's can_back()/back() hooks. */
bool ui_clock_can_back(void);
void ui_clock_back(void);

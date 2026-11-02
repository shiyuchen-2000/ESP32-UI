#pragma once

/* Create the multitask / app-switcher screen (phone-style cards of the open
 * apps). Each card has a circular close button (x) on the left; tapping the
 * rest of the card opens the app. Ownership passes to ui_shell. */
struct _lv_obj_t *ui_multitask_create(void);

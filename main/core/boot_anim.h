#pragma once

/* Boot splash: the logo wipes in diagonally, then a soft edge glow pulses
 * once. Calls `done` when the whole animation has finished. Build objects on
 * the active screen; call from the LVGL lock (app_main) like any UI code. */
void boot_anim_start(void (*done)(void));

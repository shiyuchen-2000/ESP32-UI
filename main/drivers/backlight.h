/*
 * Backlight control (LEDC PWM on the configured GPIO, or plain GPIO on/off).
 * Implemented in main.c.
 */
#pragma once

#include <stdint.h>

/* Set the screen backlight, 0-100 percent (clamped). */
void esp_ui_set_backlight(uint8_t percent);

/* Last applied backlight level, 0-100 percent. */
uint8_t esp_ui_get_backlight(void);

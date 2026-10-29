/*
 * Minimal self-contained CST816S touch driver (Waveshare 1.85B).
 * Bypasses esp_lcd_touch entirely: that stack triggered the experimental
 * i2c_master async mode (memory corruption) and never wrote the registers
 * needed to disable auto-sleep, leaving the chip reporting phantom points.
 */
#pragma once

#include "driver/i2c_master.h"

/* The shared on-board I2C bus (SCL=10 SDA=11). Other peripherals on it
 * (ES8311 codec, IMU, RTC, battery gauge) reuse this bus handle. */
i2c_master_bus_handle_t touch_i2c_bus(void);

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Reset + configure the chip (disables auto-sleep), logs the chip id. */
esp_err_t cst816s_init(void);

/* Read one touch point. Returns ESP_OK on success; *pressed set when a valid
 * point is present. Coordinates are raw panel units (may exceed 360; caller
 * filters). */
esp_err_t cst816s_read(uint16_t *x, uint16_t *y, bool *pressed);

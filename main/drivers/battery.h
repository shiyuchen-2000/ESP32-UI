/*
 * Battery gauge: TI BQ27220 fuel gauge on the shared I2C bus (addr 0x55).
 *
 * The gauge is polled by a small background task (max 2 standard commands per
 * second, as required by the BQ27220 TRM); the getters below return the last
 * cached snapshot and never touch I2C, so any UI code can call them freely.
 */
#pragma once

#include <stdbool.h>

typedef enum {
    BATT_STATE_UNKNOWN = 0,   /* gauge unreadable */
    BATT_STATE_NOT_CHARGING,  /* on battery, current flowing out */
    BATT_STATE_CHARGING,      /* current flowing into the battery */
    BATT_STATE_FULL,          /* charge complete / no current in or out */
} battery_state_t;

/* Init the I2C device on the shared bus and start polling (safe to call
 * repeatedly). Takes one synchronous sample so the values are valid on entry. */
void battery_init(void);

/* State of charge in percent (0-100), or -1 if the gauge is unreadable. */
int battery_get_percent(void);

/* Battery voltage in mV, or -1 if unreadable. */
int battery_get_mv(void);

/* Charge state. BQ27220 has no CHG bit: charging is inferred from the sign of
 * Current() (positive = into the battery) plus the full-charge flag/SOC. */
battery_state_t battery_get_state(void);

/* Convenience predicates for battery_get_state(). */
bool battery_is_charging(void);
bool battery_is_full(void);

/* Signed battery current in mA. Positive = charging, negative = discharging.
 * 0 when unreadable. */
int battery_get_current_ma(void);

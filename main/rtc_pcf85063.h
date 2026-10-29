/*
 * PCF85063ATL real-time clock on the shared I2C bus (addr 0x51).
 *
 * The chip is powered from RTC_VDD, which is fed from VBAT through a 3.3 V
 * LDO on this board, so it keeps time as long as the main battery is
 * connected - even when the watch is powered off.
 *
 * The RTC stores UTC. Call rtc_pcf85063_init() once the shared I2C bus
 * exists; it sets the process timezone and, if the stored time is valid,
 * loads it into the system clock. After an NTP sync, write the time back with
 * rtc_pcf85063_set_time().
 */
#pragma once

#include <stdbool.h>
#include <time.h>

void rtc_pcf85063_init(void);

/* Read the RTC into a UTC epoch. False if the oscillator-stopped flag is set
 * or the stored time fails sanity checks. */
bool rtc_pcf85063_get_time(time_t *out_utc);

/* Write a UTC epoch to the RTC (also clears the OS flag and sets the RAM
 * marker used for validity). */
bool rtc_pcf85063_set_time(time_t utc);

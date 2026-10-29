/*
 * PCF85063ATL RTC driver (see rtc_pcf85063.h).
 *
 * Register map (NXP PCF85063A):
 *   0x00 Control_1   bit5 STOP, bit1 12_24
 *   0x03 RAM byte    (used here as a "time has been set" marker)
 *   0x04 Seconds     bit7 = OS (oscillator stopped; time not trustworthy)
 *   0x05 Minutes
 *   0x06 Hours
 *   0x07 Days
 *   0x08 Weekdays
 *   0x09 Months
 *   0x0A Years       (00..99 => 2000..2099)
 * All time fields are BCD.
 */
#include "rtc_pcf85063.h"
#include "touch_cst816s.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include <sys/time.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "rtc";

#define PCF85063_ADDR  0x51
#define REG_CTRL1      0x00
#define REG_RAM        0x03
#define REG_SECONDS    0x04
#define NREGS_TIME     7
#define RAM_MAGIC      0xA5

static i2c_master_dev_handle_t s_dev = NULL;

static uint8_t bcd_to_bin(uint8_t v)
{
    return (uint8_t)((v >> 4) * 10 + (v & 0x0F));
}

static uint8_t bin_to_bcd(uint8_t v)
{
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

static esp_err_t reg_write(uint8_t reg, const uint8_t *data, size_t len)
{
    uint8_t buf[8];
    if (len + 1 > sizeof(buf)) {
        return ESP_ERR_INVALID_SIZE;
    }
    buf[0] = reg;
    memcpy(&buf[1], data, len);
    return i2c_master_transmit(s_dev, buf, len + 1, 100);
}

static esp_err_t reg_read(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, data, len, 100);
}

/* UTC civil date -> epoch without touching the process TZ (localtime is used
 * concurrently by the UI clock timer, so switching TZ would race). */
static time_t utc_to_epoch(const struct tm *tm)
{
    int year = tm->tm_year + 1900;
    int mon = tm->tm_mon + 1;
    int day = tm->tm_mday;
    int64_t y = year - (mon <= 2);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (mon + (mon > 2 ? -3 : 9)) + 2) / 5 + day - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = era * 146097 + (int64_t)doe - 719468;
    return (time_t)(days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec);
}

bool rtc_pcf85063_get_time(time_t *out_utc)
{
    if (!s_dev || !out_utc) {
        return false;
    }

    uint8_t b[NREGS_TIME] = { 0 };
    if (reg_read(REG_SECONDS, b, sizeof(b)) != ESP_OK) {
        return false;
    }
    if (b[0] & 0x80) {
        return false;   /* OS: oscillator stopped, time is not trustworthy */
    }

    struct tm tm = { 0 };
    tm.tm_sec = bcd_to_bin(b[0] & 0x7F);
    tm.tm_min = bcd_to_bin(b[1] & 0x7F);
    tm.tm_hour = bcd_to_bin(b[2] & 0x3F);   /* 24 h mode */
    tm.tm_mday = bcd_to_bin(b[3] & 0x3F);
    tm.tm_wday = bcd_to_bin(b[4] & 0x07);
    tm.tm_mon = bcd_to_bin(b[5] & 0x1F) - 1;
    int year = 2000 + bcd_to_bin(b[6]);

    if (year < 2020 || year > 2099 || tm.tm_mon < 0 || tm.tm_mon > 11 ||
        tm.tm_mday < 1 || tm.tm_mday > 31 || tm.tm_hour > 23 ||
        tm.tm_min > 59 || tm.tm_sec > 59) {
        return false;
    }
    tm.tm_year = year - 1900;
    tm.tm_isdst = 0;

    *out_utc = utc_to_epoch(&tm);
    return true;
}

bool rtc_pcf85063_set_time(time_t utc)
{
    if (!s_dev) {
        return false;
    }

    struct tm tm;
    gmtime_r(&utc, &tm);

    /* 24-hour mode, clock running. Read-modify-write so no other Control_1
     * bits (and especially the 0x58 software-reset pattern) are disturbed. */
    uint8_t ctrl = 0;
    if (reg_read(REG_CTRL1, &ctrl, 1) == ESP_OK) {
        ctrl &= (uint8_t)~((1 << 5) | (1 << 1));
        if (ctrl == 0x58) {
            ctrl = 0x00;
        }
        reg_write(REG_CTRL1, &ctrl, 1);
    }

    uint8_t b[NREGS_TIME];
    b[0] = bin_to_bcd((uint8_t)tm.tm_sec) & 0x7F;   /* writing bit7=0 clears OS */
    b[1] = bin_to_bcd((uint8_t)tm.tm_min);
    b[2] = bin_to_bcd((uint8_t)tm.tm_hour);         /* 24 h mode */
    b[3] = bin_to_bcd((uint8_t)tm.tm_mday);
    b[4] = (uint8_t)(tm.tm_wday & 0x07);
    b[5] = bin_to_bcd((uint8_t)(tm.tm_mon + 1));
    b[6] = bin_to_bcd((uint8_t)((tm.tm_year + 1900) % 100));

    if (reg_write(REG_SECONDS, b, sizeof(b)) != ESP_OK) {
        return false;
    }
    uint8_t magic = RAM_MAGIC;
    reg_write(REG_RAM, &magic, 1);
    (void)magic;

    /* verify by reading back (including the OS flag clearing) */
    time_t check = 0;
    if (!rtc_pcf85063_get_time(&check)) {
        ESP_LOGW(TAG, "write-back verification failed");
        return false;
    }
    ESP_LOGI(TAG, "RTC set to %04d-%02d-%02d %02d:%02d:%02d UTC",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
    return true;
}

void rtc_pcf85063_init(void)
{
    /* The UI always renders local time; set TZ even when WiFi/NTP never runs. */
    setenv("TZ", CONFIG_ESP_UI_TIMEZONE, 1);
    tzset();

    if (s_dev) {
        return;
    }
    i2c_master_bus_handle_t bus = touch_i2c_bus();
    if (!bus) {
        ESP_LOGE(TAG, "no I2C bus");
        return;
    }
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCF85063_ADDR,
        .scl_speed_hz = 100000,
    };
    if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) {
        ESP_LOGE(TAG, "add device failed");
        s_dev = NULL;
        return;
    }

    /* Only trust the stored time if we wrote it ourselves (RAM marker) and the
     * oscillator has not stopped. */
    uint8_t magic = 0;
    bool marker_ok = (reg_read(REG_RAM, &magic, 1) == ESP_OK && magic == RAM_MAGIC);

    time_t utc = 0;
    if (marker_ok && rtc_pcf85063_get_time(&utc)) {
        struct timeval tv = { .tv_sec = utc, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        struct tm tm;
        localtime_r(&utc, &tm);
        ESP_LOGI(TAG, "restored system time from RTC: %04d-%02d-%02d %02d:%02d",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
    } else {
        ESP_LOGW(TAG, "RTC has no valid time yet (marker=%d)", (int)marker_ok);
    }
}

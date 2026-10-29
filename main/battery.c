/*
 * Battery gauge: TI BQ27220 fuel gauge on the shared I2C bus (addr 0x55).
 *
 * Register map (BQ27220 TRM SLUUBD4A, Table 2-1):
 *   0x08 Voltage(), 0x0A BatteryStatus(), 0x0C Current(), 0x2C RelativeSOC().
 *
 * IMPORTANT: the BQ27220 has NO "charging" status bit. BatteryStatus() bit 8
 * is CHGINH (charge inhibit, i.e. temperature out of range), not CHG - the
 * previous driver tested that bit and therefore always reported "not
 * charging". The only charge-mode information the gauge exposes is:
 *   - Current() sign: positive = into the battery (charging), negative =
 *     discharging (TI convention, see AtRate()/AveragePower() in the TRM);
 *   - BatteryStatus()[FC] (bit 9) and the SOC for "charge complete".
 * The charger IC on this board (ETA6098) exposes STAT only on a front-panel
 * LED, not to the ESP32, so there is no GPIO to fall back on.
 *
 * The TRM also forbids issuing any standard command more than twice per
 * second (watchdog reset). A 2 s polling task keeps us at 2 reads/s total.
 */
#include "battery.h"
#include "touch_cst816s.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "battery";

#define BQ27220_ADDR  0x55
#define REG_VOLTAGE   0x08   /* mV, 14-bit */
#define REG_FLAGS     0x0A   /* BatteryStatus() */
#define REG_CURRENT   0x0C   /* signed mA */
#define REG_RSOC      0x2C   /* relative state of charge, % */

/* BatteryStatus() bits (TRM Table 2-6). */
#define BS_DSG      (1u << 0)    /* set in DISCHARGE, clear in CHARGE/RELAX */
#define BS_BATTPRES (1u << 3)    /* battery present */
#define BS_FC       (1u << 9)    /* full charge detected */

#define CHG_MA_MIN   12          /* ambiguous band around zero, mA */
#define FULL_SOC     98          /* gauge can stop at 99% when terminated */

typedef struct {
    int             mv;
    int             percent;
    int             current_ma;
    uint16_t        flags;
    battery_state_t state;
    bool            valid;
} batt_cache_t;

static i2c_master_dev_handle_t s_dev = NULL;
static batt_cache_t s_cache;                 /* guarded by s_lock */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static esp_err_t read_word(uint8_t reg, uint16_t *out)
{
    uint8_t b[2] = { 0 };
    esp_err_t err = i2c_master_transmit_receive(s_dev, &reg, 1, b, 2, 100);
    if (err == ESP_OK) {
        *out = (uint16_t)((b[1] << 8) | b[0]);
    }
    return err;
}

static battery_state_t classify(int ma, uint16_t flags, int soc)
{
    if (ma > CHG_MA_MIN) {
        return BATT_STATE_CHARGING;
    }
    if (ma < -CHG_MA_MIN) {
        return BATT_STATE_NOT_CHARGING;
    }
    /* |I| ~ 0: either the charger terminated (cable still in) or the system
     * is idle on battery with a full pack; both read as "Full" once the pack
     * is at the top. */
    if ((flags & BS_FC) || soc >= FULL_SOC) {
        return BATT_STATE_FULL;
    }
    return BATT_STATE_NOT_CHARGING;
}

static bool poll_once(void)
{
    uint16_t mv = 0, flags = 0, cur = 0, soc = 0;
    if (read_word(REG_VOLTAGE, &mv) != ESP_OK ||
        read_word(REG_FLAGS, &flags) != ESP_OK ||
        read_word(REG_CURRENT, &cur) != ESP_OK ||
        read_word(REG_RSOC, &soc) != ESP_OK) {
        return false;
    }

    batt_cache_t c;
    c.mv = (int)(mv & 0x3FFF);
    c.percent = soc > 100 ? 100 : (int)soc;
    c.current_ma = (int)(int16_t)cur;
    c.flags = flags;
    c.state = classify(c.current_ma, flags, c.percent);
    c.valid = true;

    taskENTER_CRITICAL(&s_lock);
    s_cache = c;
    taskEXIT_CRITICAL(&s_lock);
    return true;
}

static batt_cache_t snapshot(void)
{
    batt_cache_t c;
    taskENTER_CRITICAL(&s_lock);
    c = s_cache;
    taskEXIT_CRITICAL(&s_lock);
    return c;
}

static void poll_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (s_dev) {
            (void)poll_once();
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void battery_init(void)
{
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
        .device_address = BQ27220_ADDR,
        .scl_speed_hz = 100000,
    };
    if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) {
        ESP_LOGE(TAG, "add device failed");
        s_dev = NULL;
        return;
    }
    if (!poll_once()) {
        ESP_LOGW(TAG, "first read failed");
    }
    xTaskCreate(poll_task, "battpoll", 3072, NULL, 3, NULL);
}

int battery_get_mv(void)
{
    batt_cache_t c = snapshot();
    return c.valid ? c.mv : -1;
}

int battery_get_percent(void)
{
    batt_cache_t c = snapshot();
    return c.valid ? c.percent : -1;
}

int battery_get_current_ma(void)
{
    batt_cache_t c = snapshot();
    return c.valid ? c.current_ma : 0;
}

battery_state_t battery_get_state(void)
{
    batt_cache_t c = snapshot();
    return c.valid ? c.state : BATT_STATE_UNKNOWN;
}

bool battery_is_charging(void)
{
    return battery_get_state() == BATT_STATE_CHARGING;
}

bool battery_is_full(void)
{
    return battery_get_state() == BATT_STATE_FULL;
}

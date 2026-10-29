#include "touch_cst816s.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "cst816s";

#define CST816S_ADDR            0x15
#define CST816S_REG_GESTURE     0x01
#define CST816S_REG_DIS_SLEEP   0xFE
#define CST816S_REG_CHIP_ID     0xA7

#define PIN_RST GPIO_NUM_1
#define PIN_SDA GPIO_NUM_11
#define PIN_SCL GPIO_NUM_10

static i2c_master_dev_handle_t s_dev = NULL;
static i2c_master_bus_handle_t s_bus = NULL;

/* Shared on-board I2C bus (SCL=10 SDA=11), also used by the ES8311 codec,
 * IMU, RTC and battery gauge. */
i2c_master_bus_handle_t touch_i2c_bus(void)
{
    return s_bus;
}

static esp_err_t i2c_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 100);
}

static esp_err_t i2c_read_bytes(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, data, len, 100);
}

esp_err_t cst816s_init(void)
{
    /* I2C bus (synchronous: trans_queue_depth 0 avoids the experimental async
     * mode that corrupted memory through esp_lcd's I2C panel IO) */
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_SDA,
        .scl_io_num = PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .trans_queue_depth = 0,
    };
    i2c_master_bus_handle_t bus = NULL;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &bus), TAG, "i2c bus init failed");
    s_bus = bus;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CST816S_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev_cfg, &s_dev), TAG, "i2c add device failed");

    /* Hardware reset: low 10ms, high 50ms (per Waveshare reference) */
    gpio_config_t rst = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << PIN_RST,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&rst), TAG, "rst gpio config failed");
    gpio_set_level(PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Disable auto-sleep so the chip stops dropping into a phantom state */
    esp_err_t err = i2c_write_reg(CST816S_REG_DIS_SLEEP, 10);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "disable auto-sleep write failed 0x%x", err);
    }

    uint8_t id = 0;
    err = i2c_read_bytes(CST816S_REG_CHIP_ID, &id, 1);
    ESP_LOGI(TAG, "chip id read: err=0x%x id=0x%02x", err, id);

    /* Probe the shared on-board I2C bus to identify the board revision:
     * touch 0x15, ES8311 audio codec 0x18, PCF85063 RTC 0x51, QMI8658 0x6A. */
    ESP_LOGI(TAG, "I2C bus scan (SCL=10 SDA=11):");
    for (int addr = 0x08; addr <= 0x77; addr++) {
        if (i2c_master_probe(bus, addr, 30) == ESP_OK) {
            ESP_LOGI(TAG, "  found device at 0x%02X", addr);
        }
    }

    return ESP_OK;
}

esp_err_t cst816s_read(uint16_t *x, uint16_t *y, bool *pressed)
{
    /* Read GestureID..y_l (6 bytes): [gesture, num, x_h, x_l, y_h, y_l] */
    uint8_t buf[6] = {0};
    esp_err_t err = i2c_read_bytes(CST816S_REG_GESTURE, buf, sizeof(buf));
    if (err != ESP_OK) {
        return err;
    }

    uint8_t num = buf[1];
    if (num > 0) {
        *x = (uint16_t)(((buf[2] & 0x0F) << 8) | buf[3]);
        *y = (uint16_t)(((buf[4] & 0x0F) << 8) | buf[5]);
        *pressed = true;
    } else {
        *pressed = false;
    }
    return ESP_OK;
}

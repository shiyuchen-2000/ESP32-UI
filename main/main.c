/*
 * ESP32-S3 round-screen smartwatch UI.
 * Hardware: Waveshare ESP32-S3-Touch-LCD-1.85B, ST77916 360x360 QSPI,
 * CST816S touch. Display init follows the official Waveshare ESP-IDF BSP.
 *
 * UI: desktop (watch face) + 7 placeholder apps + multitask, with gesture
 * navigation (tap app / swipe-up-from-bottom home / hold-middle multitask).
 */

#include <stdio.h>
#include <unistd.h>
#include <sys/lock.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st77916.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"

#include "touch_cst816s.h"
#include "rtc_pcf85063.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lvgl.h"

#include "ui_shell.h"
#include "boot_anim.h"
#include "nav_ble.h"
#include "gesture.h"
#include "backlight.h"
#include "audio.h"
#include "wifi_mgr.h"
#include "battery.h"
#include "auto_rotate.h"
#include "esp_heap_caps.h"

static const char *TAG = "esp_ui";

/* ST77916 vendor init sequences from the official Waveshare BSP */
extern const st77916_lcd_init_cmd_t st77916_init_v1[];
extern const uint16_t st77916_init_v1_size;
extern const st77916_lcd_init_cmd_t st77916_init_v2[];
extern const uint16_t st77916_init_v2_size;

#define ESP_UI_LCD_HOST       SPI2_HOST
#define ESP_UI_LCD_H_RES      CONFIG_ESP_UI_LCD_H_RES
#define ESP_UI_LCD_V_RES      CONFIG_ESP_UI_LCD_V_RES
#define ESP_UI_PIN_SCLK       CONFIG_ESP_UI_PIN_NUM_SCLK
#define ESP_UI_PIN_CS         CONFIG_ESP_UI_PIN_NUM_CS
#define ESP_UI_PIN_SDIO0      CONFIG_ESP_UI_PIN_NUM_SDIO0
#define ESP_UI_PIN_SDIO1      CONFIG_ESP_UI_PIN_NUM_SDIO1
#define ESP_UI_PIN_SDIO2      CONFIG_ESP_UI_PIN_NUM_SDIO2
#define ESP_UI_PIN_SDIO3      CONFIG_ESP_UI_PIN_NUM_SDIO3
#define ESP_UI_PIN_RST        CONFIG_ESP_UI_PIN_NUM_LCD_RST

#define LCD_OPCODE_READ_CMD   (0x0BULL)

#define ESP_UI_LVGL_DRAW_BUF_LINES   CONFIG_ESP_UI_LVGL_DRAW_BUF_LINES
#define ESP_UI_LVGL_TICK_PERIOD_MS   2
#define ESP_UI_LVGL_TASK_MAX_DELAY_MS 500
#define ESP_UI_LVGL_TASK_MIN_DELAY_MS 1000 / CONFIG_FREERTOS_HZ
#define ESP_UI_LVGL_TASK_STACK_SIZE  (8 * 1024)
#define ESP_UI_LVGL_TASK_PRIORITY    2

// LVGL is not thread-safe; a mutex guards calls from other tasks
static _lock_t lvgl_api_lock;

/* ------------------------------------------------------------------ */
/* backlight (LEDC PWM)                                               */
/* ------------------------------------------------------------------ */
static bool s_backlight_pwm = false;
static uint8_t s_backlight_percent = 50;   /* default 50%: no full-brightness blast on boot */

void esp_ui_set_backlight(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    s_backlight_percent = percent;
    if (s_backlight_pwm) {
        uint32_t duty = (uint32_t)percent * 1023 / 100;
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    } else if (CONFIG_ESP_UI_PIN_NUM_BK_LIGHT >= 0) {
        gpio_set_level(CONFIG_ESP_UI_PIN_NUM_BK_LIGHT, percent > 0 ? CONFIG_ESP_UI_BK_LIGHT_ON_LEVEL : !CONFIG_ESP_UI_BK_LIGHT_ON_LEVEL);
    }
}

uint8_t esp_ui_get_backlight(void)
{
    return s_backlight_percent;
}

static void backlight_init(void)
{
    if (CONFIG_ESP_UI_PIN_NUM_BK_LIGHT < 0) {
        return;
    }
#if CONFIG_ESP_UI_BK_LIGHT_PWM
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 20000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    if (ledc_timer_config(&timer) == ESP_OK) {
        ledc_channel_config_t ch = {
            .gpio_num = CONFIG_ESP_UI_PIN_NUM_BK_LIGHT,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = LEDC_CHANNEL_0,
            .timer_sel = LEDC_TIMER_0,
            .duty = 1023,
            .hpoint = 0,
        };
        if (ledc_channel_config(&ch) == ESP_OK) {
            s_backlight_pwm = true;
        }
    }
#endif
    if (!s_backlight_pwm) {
        gpio_config_t bk = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << CONFIG_ESP_UI_PIN_NUM_BK_LIGHT,
        };
        ESP_ERROR_CHECK(gpio_config(&bk));
    }
    /* restore the user's brightness (default 50%); the settings slider writes
     * it back to NVS on release */
    uint8_t v = 50;
    nvs_handle_t h;
    if (nvs_open("ui", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "bright", &v);
        nvs_close(h);
    }
    if (v < 1) {
        v = 1;
    }
    if (v > 100) {
        v = 100;
    }
    esp_ui_set_backlight(v);
}

/* ------------------------------------------------------------------ */
/* LVGL <-> esp_lcd bridge                                            */
/* ------------------------------------------------------------------ */
static bool esp_ui_notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    lv_display_t *disp = (lv_display_t *)user_ctx;
    lv_display_flush_ready(disp);
    return false;
}

/* ST77916 draw_bitmap is picky about alignment; round invalidated areas so the
 * flush regions stay well-formed. */
static void esp_ui_lvgl_rounder_cb(lv_event_t *e)
{
    lv_area_t *area = (lv_area_t *)lv_event_get_param(e);
    area->x1 = (area->x1 >> 1) << 1;
    area->y1 = (area->y1 >> 1) << 1;
    area->x2 = ((area->x2 >> 1) << 1) + 1;
    area->y2 = ((area->y2 >> 1) << 1) + 1;
}

static void esp_ui_lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel_handle = lv_display_get_user_data(disp);
    int offsetx1 = area->x1;
    int offsetx2 = area->x2;
    int offsety1 = area->y1;
    int offsety2 = area->y2;
#if CONFIG_ESP_UI_LCD_SWAP_RGB565
    // this panel expects the RGB565 byte order swapped
    lv_draw_sw_rgb565_swap(px_map, (offsetx2 + 1 - offsetx1) * (offsety2 + 1 - offsety1));
#endif
    if (auto_rotate_get_rotation() != 0) {
        /* Rotate the strip into physical space (auto screen rotation). The
         * scratch holds at most one full draw buffer (H_RES x DRAW_BUF_LINES). */
        static uint8_t *s_rot_scratch = NULL;
        if (!s_rot_scratch) {
            s_rot_scratch = heap_caps_malloc(ESP_UI_LCD_H_RES * ESP_UI_LVGL_DRAW_BUF_LINES * sizeof(uint16_t),
                                             MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        }
        if (s_rot_scratch) {
            lv_area_t dst;
            auto_rotate_map_rect(area, &dst);
            auto_rotate_pixels(area, px_map, &dst, s_rot_scratch);
            esp_lcd_panel_draw_bitmap(panel_handle, dst.x1, dst.y1, dst.x2 + 1, dst.y2 + 1, s_rot_scratch);
            return;
        }
    }
    esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, px_map);
}

static void esp_ui_increase_lvgl_tick(void *arg)
{
    lv_tick_inc(ESP_UI_LVGL_TICK_PERIOD_MS);
}

static void esp_ui_lvgl_port_task(void *arg)
{
    ESP_LOGI(TAG, "Starting LVGL task");
    uint32_t time_till_next_ms = 0;
    while (1) {
        _lock_acquire(&lvgl_api_lock);
        time_till_next_ms = lv_timer_handler();
        _lock_release(&lvgl_api_lock);
        time_till_next_ms = MAX(time_till_next_ms, ESP_UI_LVGL_TASK_MIN_DELAY_MS);
        time_till_next_ms = MIN(time_till_next_ms, ESP_UI_LVGL_TASK_MAX_DELAY_MS);
        usleep(1000 * time_till_next_ms);
    }
}

/* ------------------------------------------------------------------ */
/* touch (CST816S) + gesture                                          */
/* ------------------------------------------------------------------ */
static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    uint16_t x = 0, y = 0;
    bool pressed = false;
    esp_err_t err = cst816s_read(&x, &y, &pressed);

    /* Reject read errors and out-of-range coordinates; never feed garbage to
     * LVGL (it caused crashes before the touch driver was fixed). */
    bool valid = (err == ESP_OK) && pressed &&
                 x < ESP_UI_LCD_H_RES && y < ESP_UI_LCD_V_RES;

    if (valid) {
        /* Always feed the live press position so LVGL sees a normal press or
         * drag: taps click, drags don't. This keeps a swipe on a multitask
         * card from also firing the card's click (which opened the app). */
        int tx = x, ty = y;
        auto_rotate_transform_touch(&tx, &ty);   /* physical -> content space */
        x = (uint16_t)tx;
        y = (uint16_t)ty;
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
        gesture_process(true, x, y);
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
        gesture_process(false, 0, 0);
    }
}

static void touch_init(lv_display_t *display)
{
    ESP_ERROR_CHECK(cst816s_init());

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(indev, display);
    lv_indev_set_user_data(indev, NULL);
    lv_indev_set_read_cb(indev, touch_read_cb);
    ESP_LOGI(TAG, "Touch initialized");
}

/* ------------------------------------------------------------------ */
/* app_main                                                           */
/* ------------------------------------------------------------------ */
void app_main(void)
{
    /* NVS first: the audio / wifi / BLE modules use NVS (repeated init is a
     * no-op). Backlight is enabled later, after the LCD/LVGL are ready, so the
     * panel never shows a white flash. */
    esp_err_t nerr = nvs_flash_init();
    if (nerr == ESP_ERR_NVS_NO_FREE_PAGES || nerr == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nerr = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nerr);

    // Hardware reset the LCD on GPIO3 (low then high), per the official BSP
    if (ESP_UI_PIN_RST >= 0) {
        gpio_config_t rst = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << ESP_UI_PIN_RST,
        };
        ESP_ERROR_CHECK(gpio_config(&rst));
        gpio_set_level(ESP_UI_PIN_RST, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(ESP_UI_PIN_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // QSPI bus with the QUAD flag
    ESP_LOGI(TAG, "Initialize QSPI bus");
    spi_bus_config_t buscfg = {
        .data0_io_num = ESP_UI_PIN_SDIO0,
        .data1_io_num = ESP_UI_PIN_SDIO1,
        .sclk_io_num = ESP_UI_PIN_SCLK,
        .data2_io_num = ESP_UI_PIN_SDIO2,
        .data3_io_num = ESP_UI_PIN_SDIO3,
        .data4_io_num = -1,
        .data5_io_num = -1,
        .data6_io_num = -1,
        .data7_io_num = -1,
        .max_transfer_sz = ESP_UI_LCD_H_RES * 80 * sizeof(uint16_t),
        .flags = SPICOMMON_BUSFLAG_QUAD,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(ESP_UI_LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    // Slow (3 MHz) panel IO, read register 0x04 to detect the panel version
    ESP_LOGI(TAG, "Install panel IO (3 MHz) for version detection");
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = ESP_UI_PIN_CS,
        .dc_gpio_num = -1,
        .spi_mode = 0,
        .pclk_hz = 3 * 1000 * 1000,
        .trans_queue_depth = 10,
        .on_color_trans_done = NULL,
        .user_ctx = NULL,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags = {
            .dc_low_on_data = 0,
            .octal_mode = 0,
            .quad_mode = 1,
            .sio_mode = 0,
            .lsb_first = 0,
            .cs_high_active = 0,
        },
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)ESP_UI_LCD_HOST, &io_config, &io_handle));

    uint8_t reg[4] = {0};
    int lcd_cmd = 0x04;
    lcd_cmd &= 0xff;
    lcd_cmd <<= 8;
    lcd_cmd |= (int)(LCD_OPCODE_READ_CMD << 24);
    esp_err_t ret = esp_lcd_panel_io_rx_param(io_handle, lcd_cmd, reg, 4);
    ESP_LOGI(TAG, "ST77916 reg 0x04 read: err=0x%x data=%02x %02x %02x %02x", ret, reg[0], reg[1], reg[2], reg[3]);

    // Recreate the panel IO at the fast clock, then install the panel
    ESP_LOGI(TAG, "Install panel IO (%d Hz)", CONFIG_ESP_UI_LCD_PCLK_HZ);
    io_config.pclk_hz = CONFIG_ESP_UI_LCD_PCLK_HZ;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)ESP_UI_LCD_HOST, &io_config, &io_handle));

    ESP_LOGI(TAG, "Install ST77916 panel driver");
    esp_lcd_panel_handle_t panel_handle = NULL;
    st77916_vendor_config_t vendor_config = {
        .flags = {
            .use_qspi_interface = 1,
        },
    };
    if (ret == ESP_OK && reg[0] == 0x00 && reg[1] == 0x7F && reg[2] == 0x7F && reg[3] == 0x7F) {
        vendor_config.init_cmds = st77916_init_v1;
        vendor_config.init_cmds_size = st77916_init_v1_size;
        ESP_LOGI(TAG, "Panel version 1 -> init v1");
    } else if (ret == ESP_OK && reg[0] == 0x00 && reg[1] == 0x02 && reg[2] == 0x7F && reg[3] == 0x7F) {
        vendor_config.init_cmds = st77916_init_v2;
        vendor_config.init_cmds_size = st77916_init_v2_size;
        ESP_LOGI(TAG, "Panel version 2 -> init v2");
    } else {
        ESP_LOGW(TAG, "Unknown panel version, falling back to init v1");
        vendor_config.init_cmds = st77916_init_v1;
        vendor_config.init_cmds_size = st77916_init_v1_size;
    }

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = ESP_UI_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .flags = {
            .reset_active_high = 0,
        },
        .vendor_config = (void *)&vendor_config,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st77916(io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    // LVGL
    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    lv_display_t *display = lv_display_create(ESP_UI_LCD_H_RES, ESP_UI_LCD_V_RES);

    size_t draw_buffer_sz = ESP_UI_LCD_H_RES * ESP_UI_LVGL_DRAW_BUF_LINES * sizeof(lv_color16_t);
    void *buf1 = spi_bus_dma_memory_alloc(ESP_UI_LCD_HOST, draw_buffer_sz, 0);
    assert(buf1);
    void *buf2 = spi_bus_dma_memory_alloc(ESP_UI_LCD_HOST, draw_buffer_sz, 0);
    assert(buf2);
    lv_display_set_buffers(display, buf1, buf2, draw_buffer_sz, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_user_data(display, panel_handle);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(display, esp_ui_lvgl_flush_cb);
    lv_display_add_event_cb(display, esp_ui_lvgl_rounder_cb, LV_EVENT_INVALIDATE_AREA, display);

    ESP_LOGI(TAG, "Install LVGL tick timer");
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &esp_ui_increase_lvgl_tick,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, ESP_UI_LVGL_TICK_PERIOD_MS * 1000));

    ESP_LOGI(TAG, "Register io panel event callback for LVGL flush ready notification");
    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = esp_ui_notify_lvgl_flush_ready,
    };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(io_handle, &cbs, display));

    /* Black default screen: avoids a white flash between LVGL start and the
     * boot splash (peripheral init runs in between). */
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_black(), 0);
    lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, 0);

    ESP_LOGI(TAG, "Create LVGL task");
    /* Pin the GUI to Core 0 so it never wanders onto Core 1 where the WiFi
     * driver runs (keeps the WiFi ISR from being starved during display
     * refresh -> avoids "Interrupt wdt timeout"). */
    xTaskCreatePinnedToCore(esp_ui_lvgl_port_task, "LVGL", ESP_UI_LVGL_TASK_STACK_SIZE, NULL,
                            ESP_UI_LVGL_TASK_PRIORITY, NULL, 0);

    // Touch (with gesture layer). After this, the shared I2C bus exists, so the
    // auto-rotate IMU can attach to it.
    touch_init(display);

    // RTC: restore the system clock before the UI starts (survives WiFi-off
    // reboots; NTP writes it back after a sync)
    rtc_pcf85063_init();

    // Auto screen rotation (toggle in Settings; off by default)
    auto_rotate_init(display);

    // Start the I2S audio path to the on-board ES8311 codec (non-fatal)
    audio_init();

    // WiFi: SoftAP provisioning if no saved network, else STA auto-connect.
    // Conservative internal-RAM config (see project memory: aggressive PSRAM
    // WiFi settings previously caused InstrFetchProhibited).
    wifi_mgr_init();

    // BLE navigation link (MotoNav protocol; unencrypted for the test build)
    nav_ble_init();

    ESP_LOGI(TAG, "Free heap: internal %u B, total %u B",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT));

    // Battery gauge (BQ27220 on the shared I2C bus)
    battery_init();
    ESP_LOGI(TAG, "Battery: %d mV, %d%%, state=%d, cur=%+d mA",
             battery_get_mv(), battery_get_percent(), (int)battery_get_state(), battery_get_current_ma());

    /* Backlight on now that the panel and LVGL are ready and the screen is
     * already black - no white flash. */
    backlight_init();

    // Boot splash (logo wipe + glow), then the desktop
    ESP_LOGI(TAG, "Show boot splash");
    _lock_acquire(&lvgl_api_lock);
    boot_anim_start(ui_shell_show_desktop);
    _lock_release(&lvgl_api_lock);
}

/*
 * Audio output to the on-board ES8311 codec (Waveshare ESP32-S3-Touch-LCD-1.85B):
 * I2C control at 0x18 on the shared bus (SCL=10 SDA=11), I2S data with
 * MCLK=GPIO2 / BCLK=48 / WS=38 / DOUT=47, and the power amplifier is enabled
 * via GPIO9. Volume is the codec's digital output volume (0-100).
 *
 * Beeps are played by a dedicated low-priority task so the LVGL task never
 * blocks on I2S writes, and no large buffers sit on the (8 KB) LVGL stack.
 */
#include "audio.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "touch_cst816s.h"
#include <math.h>

static const char *TAG = "esp_ui_audio";

#define AUDIO_SAMPLE_RATE  44100
#define AUDIO_MCLK_PIN     2
#define AUDIO_BCK_PIN      48
#define AUDIO_WS_PIN       38
#define AUDIO_DOUT_PIN     47
#define AUDIO_PA_PIN       9

#define BEEP_MS      140
#define BEEP_FREQ    880
#define BEEP_CHUNK   256   /* samples per write (small; beep task stack is 8 KB) */

static i2s_chan_handle_t s_tx = NULL;
static esp_codec_dev_handle_t s_codec = NULL;
static int s_volume = 0;    /* 0-100; default muted so a boot never blasts audio */

/* Beep playback task */
static StackType_t s_beep_stack[2048];
static StaticTask_t s_beep_tcb;
static TaskHandle_t s_beep_task = NULL;

static void beep_task(void *arg)
{
    int16_t buf[BEEP_CHUNK];

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);   /* wait for a beep request */
        if (!s_codec || s_volume <= 0) {
            continue;
        }

        size_t total = (size_t)(AUDIO_SAMPLE_RATE * BEEP_MS / 1000);
        float amp = (float)s_volume / 100.0f * 0.45f * 32767.0f;
        size_t written = 0;
        while (written < total) {
            size_t chunk = total - written;
            if (chunk > BEEP_CHUNK) {
                chunk = BEEP_CHUNK;
            }
            for (size_t i = 0; i < chunk; i++) {
                size_t idx = written + i;
                /* half-sine envelope: smooth attack and release, no clicks */
                float env = sinf(3.14159265f * (float)idx / (float)total);
                float v = sinf(2.0f * 3.14159265f * BEEP_FREQ * (float)idx / AUDIO_SAMPLE_RATE);
                buf[i] = (int16_t)(v * amp * env);
            }
            int n = esp_codec_dev_write(s_codec, buf, (int)(chunk * sizeof(int16_t)));
            if (n <= 0) {
                break;
            }
            written += (size_t)n / sizeof(int16_t);
        }
    }
}

void audio_set_volume(int vol)
{
    if (vol < 0) {
        vol = 0;
    }
    if (vol > 100) {
        vol = 100;
    }
    s_volume = vol;
    if (s_codec) {
        esp_codec_dev_set_out_vol(s_codec, vol);
    }
}

int audio_get_volume(void)
{
    return s_volume;
}

void audio_init(void)
{
    if (s_codec) {
        return;
    }

    /* 1. I2S TX channel, MCLK on GPIO2 for the ES8311 */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    if (i2s_new_channel(&chan_cfg, &s_tx, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed");
        s_tx = NULL;
        return;
    }
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = AUDIO_MCLK_PIN,
            .bclk = AUDIO_BCK_PIN,
            .ws = AUDIO_WS_PIN,
            .dout = AUDIO_DOUT_PIN,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    if (i2s_channel_init_std_mode(s_tx, &std_cfg) != ESP_OK ||
        i2s_channel_enable(s_tx) != ESP_OK) {
        ESP_LOGE(TAG, "I2S setup failed");
        i2s_del_channel(s_tx);
        s_tx = NULL;
        return;
    }

    /* 2. codec data interface over I2S */
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .tx_handle = s_tx,
        .rx_handle = NULL,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (!data_if) {
        ESP_LOGE(TAG, "audio_codec_new_i2s_data failed");
        return;
    }

    /* 3. codec control interface over the shared I2C bus. Note: the driver
     * expects the 8-bit address (0x30) and right-shifts it to 0x18. */
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_NUM_0,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = touch_i2c_bus(),
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!ctrl_if) {
        ESP_LOGE(TAG, "audio_codec_new_i2c_ctrl failed");
        return;
    }

    /* 4. ES8311 codec: DAC output to the speaker through the PA on GPIO9 */
    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = audio_codec_new_gpio(),
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = AUDIO_PA_PIN,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = {
            .pa_voltage = 3.3,
            .codec_dac_voltage = 3.3,
        },
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es8311_cfg);
    if (!codec_if) {
        ESP_LOGE(TAG, "es8311_codec_new failed");
        return;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_codec = esp_codec_dev_new(&dev_cfg);
    if (!s_codec) {
        ESP_LOGE(TAG, "esp_codec_dev_new failed");
        return;
    }

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = AUDIO_SAMPLE_RATE,
        .channel = 1,
        .bits_per_sample = 16,
    };
    if (esp_codec_dev_open(s_codec, &fs) != ESP_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open failed");
        return;
    }

    /* restore the stored volume (default 0 = muted); the settings slider
     * persists it to NVS on release */
    nvs_handle_t h;
    uint8_t v = (uint8_t)s_volume;
    if (nvs_open("ui", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "volume", &v);
        nvs_close(h);
    }
    if (v > 100) {
        v = 100;
    }
    s_volume = v;
    esp_codec_dev_set_out_vol(s_codec, s_volume);
    ESP_LOGI(TAG, "ES8311 ready (MCLK=%d BCLK=%d WS=%d DOUT=%d PA=%d)",
             AUDIO_MCLK_PIN, AUDIO_BCK_PIN, AUDIO_WS_PIN, AUDIO_DOUT_PIN, AUDIO_PA_PIN);

    if (!s_beep_task) {
        s_beep_task = xTaskCreateStatic(beep_task, "audio_beep", 2048, NULL, 5,
                                        s_beep_stack, &s_beep_tcb);
    }
}

void audio_play_beep(void)
{
    if (s_beep_task) {
        xTaskNotifyGive(s_beep_task);   /* non-blocking: the beep task plays it */
    }
}

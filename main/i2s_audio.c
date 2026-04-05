#include "i2s_audio.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#define TAG "I2S_AUDIO"

// Pin definitions
#define SPK_I2S_BCLK   14
#define SPK_I2S_WS     15
#define SPK_I2S_DOUT   22
#define AMP_SD_GPIO    27

static i2s_chan_handle_t tx_handle = NULL;

esp_err_t i2s_speaker_init(void) {
    // Enable amplifier via SD pin
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << AMP_SD_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(AMP_SD_GPIO, 1);  // Bring MAX98357A out of shutdown

    // Create I2S TX channel
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(
        I2S_NUM_0, I2S_ROLE_MASTER
    );
    chan_cfg.auto_clear = true;

    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, NULL));

    // Standard I2S config for A2DP (starts at 44100 Hz, stereo, 16-bit)
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT,
            I2S_SLOT_MODE_STEREO
            // I2S_SLOT_MODE_MONO
        ),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = SPK_I2S_BCLK,
            .ws   = SPK_I2S_WS,
            .dout = SPK_I2S_DOUT,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));

    ESP_LOGI(TAG, "I2S speaker initialized");
    return ESP_OK;
}

esp_err_t i2s_speaker_write(const uint8_t *data, size_t len, size_t *bytes_written) {
    if (!tx_handle) return ESP_ERR_INVALID_STATE;
    return i2s_channel_write(tx_handle, data, len, bytes_written, portMAX_DELAY);
}

esp_err_t i2s_speaker_set_sample_rate(uint32_t sample_rate) {
    if (!tx_handle) return ESP_ERR_INVALID_STATE;

    ESP_ERROR_CHECK(i2s_channel_disable(tx_handle));

    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    ESP_ERROR_CHECK(i2s_channel_reconfig_std_clock(tx_handle, &clk_cfg));

    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));

    ESP_LOGI(TAG, "Sample rate updated to %" PRIu32 " Hz", sample_rate);
    return ESP_OK;
}

esp_err_t i2s_speaker_set_slot_mode(i2s_slot_mode_t mode) {
    if (!tx_handle) return ESP_ERR_INVALID_STATE;

    ESP_ERROR_CHECK(i2s_channel_disable(tx_handle));

    i2s_std_slot_config_t slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_16BIT, mode
    );
    ESP_ERROR_CHECK(i2s_channel_reconfig_std_slot(tx_handle, &slot_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));

    ESP_LOGI("I2S_AUDIO", "Slot mode updated to %s",
             mode == I2S_SLOT_MODE_MONO ? "mono" : "stereo");
    return ESP_OK;
}
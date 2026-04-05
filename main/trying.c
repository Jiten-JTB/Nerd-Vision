#include "bt_a2dp.h"
#include "bt_hfp.h"
#include "i2s_audio.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG         "MAIN"
#define BUTTON_GPIO  32

// Button timing
#define SHORT_PRESS_MAX_MS  500
#define LONG_PRESS_MIN_MS   500

#define DOUBLE_CLICK_MAX_MS  400   // max gap between two clicks

static void button_task(void *arg) {
    bool     last_state      = true;
    uint32_t press_start     = 0;
    uint32_t last_release_ms = 0;
    bool     waiting_double  = false;

    while (1) {
        bool current = gpio_get_level(BUTTON_GPIO);
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

        if (!current && last_state) {
            // Button just pressed
            press_start = now;
        }

        if (current && !last_state) {
            // Button just released
            uint32_t press_time = now - press_start;

            if (bt_hfp_is_call_active()) {
                // During call — single press behaviour only
                if (press_time < SHORT_PRESS_MAX_MS) {
                    ESP_LOGI(TAG, "Short press → answer call");
                    bt_hfp_answer_call();
                } else {
                    ESP_LOGI(TAG, "Long press → reject/end call");
                    bt_hfp_reject_call();
                }
                waiting_double  = false;
                last_release_ms = 0;

            } else {
                // No call — check for double click
                if (press_time >= LONG_PRESS_MIN_MS) {
                    // Long press — reserved for future use
                    ESP_LOGI(TAG, "Long press — no action assigned");
                    waiting_double  = false;
                    last_release_ms = 0;

                } else if (waiting_double &&
                           (now - last_release_ms) < DOUBLE_CLICK_MAX_MS) {
                    // Second click within window → trigger assistant
                    ESP_LOGI(TAG, "Double click → triggering assistant");
                    bt_hfp_trigger_voice_recognition();
                    waiting_double  = false;
                    last_release_ms = 0;

                } else {
                    // First click — start waiting for second
                    waiting_double  = true;
                    last_release_ms = now;
                }
            }
        }

        // Double click window expired — treat as single click
        // Single Click → Play/Pause
        if (waiting_double &&
            (now - last_release_ms) >= DOUBLE_CLICK_MAX_MS) {
            if (!bt_hfp_is_call_active()) {
                ESP_LOGI(TAG, "Single click → play/pause");
                bt_a2dp_play_pause();
            }
            waiting_double  = false;
            last_release_ms = 0;
        }

        last_state = current;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void app_main(void) {
    // Button GPIO
    gpio_config_t btn = {
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn);

    // Init audio
    ESP_ERROR_CHECK(i2s_speaker_init());

    // Init Bluetooth profiles
    ESP_ERROR_CHECK(bt_a2dp_sink_init("SmartGlasses"));
    ESP_ERROR_CHECK(bt_hfp_init());

    // Button task on Core 1 at low priority
    xTaskCreatePinnedToCore(
        button_task, "button",
        2048, NULL, 3, NULL, 1
    );

    ESP_LOGI(TAG, "System ready — advertising as 'SmartGlasses'");
}
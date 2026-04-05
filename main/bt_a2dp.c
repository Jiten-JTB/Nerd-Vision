#include "bt_a2dp.h"
#include "i2s_audio.h"
#include <string.h>
#include "nvs_flash.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"

#define TAG          "BT_A2DP"
#define RINGBUF_SIZE (64 * 1024)   // 64 KB ring buffer in PSRAM

static RingbufHandle_t s_ringbuf      = NULL;
static TaskHandle_t    s_writer_task  = NULL;
static bool            s_connected    = false;
static uint32_t        s_sample_rate  = 44100;
static uint32_t        s_current_sample_rate = 44100;
static bool            s_a2dp_streaming = false;

uint32_t bt_a2dp_get_sample_rate(void);
uint32_t bt_a2dp_get_sample_rate(void) {
    return s_current_sample_rate;
}

/* ─── Ring-buffer writer task ─────────────────────────────────────── */
static void audio_write_task(void *arg) {
    size_t item_size;
    while (1) {
        uint8_t *data = (uint8_t *)xRingbufferReceive(
            s_ringbuf, &item_size, pdMS_TO_TICKS(100)
        );
        if (data) {
            size_t written = 0;
            i2s_speaker_write(data, item_size, &written);
            vRingbufferReturnItem(s_ringbuf, data);
        }
    }
}

/* ─── A2DP callbacks ──────────────────────────────────────────────── */
// Stereo
static void a2dp_data_cb(const uint8_t *data, uint32_t len) {
    // Called from BT task — push to ring buffer, don't block I2S directly
    if (s_ringbuf) {
        xRingbufferSend(s_ringbuf, data, len, pdMS_TO_TICKS(10));
    }
}

// Mono
// static void a2dp_data_cb(const uint8_t *data, uint32_t len) {
//     if (!s_ringbuf) return;

//     #define CHUNK_SAMPLES 256
//     int16_t mono_buf[CHUNK_SAMPLES];
//     int16_t *src = (int16_t *)data;
//     uint32_t total_stereo_samples = len / 2;  // total int16 values
//     uint32_t i = 0;

//     while (i < total_stereo_samples) {
//         uint32_t chunk = 0;
//         while (chunk < CHUNK_SAMPLES && i + 1 < total_stereo_samples) {
//             // Average left and right into one mono sample
//             int32_t mixed = ((int32_t)src[i] + (int32_t)src[i + 1]) >> 1;
//             mono_buf[chunk++] = (int16_t)mixed;
//             i += 2;
//         }
//         if (chunk > 0) {
//             xRingbufferSend(s_ringbuf, mono_buf, chunk * sizeof(int16_t), pdMS_TO_TICKS(10));
//         }
//     }
// }

static void a2dp_event_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param) {
    switch (event) {

    case ESP_A2D_CONNECTION_STATE_EVT: {
        if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            ESP_LOGI(TAG, "A2DP connected — %02x:%02x:%02x:%02x:%02x:%02x",
                param->conn_stat.remote_bda[0], param->conn_stat.remote_bda[1],
                param->conn_stat.remote_bda[2], param->conn_stat.remote_bda[3],
                param->conn_stat.remote_bda[4], param->conn_stat.remote_bda[5]);
            s_connected = true;
        } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            ESP_LOGI(TAG, "A2DP disconnected");
            s_connected = false;
            esp_bt_gap_set_scan_mode(
                ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE
            );
        }
        break;
    }

    case ESP_A2D_AUDIO_STATE_EVT: {
        if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED) {
            ESP_LOGI(TAG, "Audio streaming started");
            s_a2dp_streaming = true;
        } else if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STOPPED) {
            ESP_LOGI(TAG, "Audio streaming stopped");
            s_a2dp_streaming = false;
        }
        break;
    }

    case ESP_A2D_AUDIO_CFG_EVT: {
        uint32_t sr = param->audio_cfg.mcc.cie.sbc[0] & 0x0F;
        uint32_t rate = 44100;
        switch (sr) {
            case 0x08: rate = 16000; break;
            case 0x04: rate = 32000; break;
            case 0x02: rate = 44100; break;
            case 0x01: rate = 48000; break;
        }
        s_current_sample_rate = rate;   // ← save it
        if (rate != s_sample_rate) {
            s_sample_rate = rate;
            i2s_speaker_set_sample_rate(rate);
        }
        ESP_LOGI(TAG, "Audio cfg: sample rate %" PRIu32 " Hz", rate);
        break;
    }

    default:
        break;
    }
}

/* ─── GAP callbacks ───────────────────────────────────────────────── */
static void gap_event_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
    switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "Paired with: %s", param->auth_cmpl.device_name);
        } else {
            ESP_LOGE(TAG, "Pairing failed, status: %d", param->auth_cmpl.stat);
        }
        break;
    case ESP_BT_GAP_PIN_REQ_EVT:
        // Respond with default PIN "1234"
        ESP_LOGI(TAG, "PIN request");
        esp_bt_pin_code_t pin = {'1','2','3','4'};
        esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin);
        break;
    default:
        break;
    }
}

/* ─── AVRCP callbacks (target — we receive commands from phone) ───── */
static void avrc_tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param) {
    switch (event) {
    case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT:
        ESP_LOGI(TAG, "AVRCP set volume: %d", param->set_abs_vol.volume);
        // Map 0–127 to your amp gain if needed
        break;
    default:
        break;
    }
}

static void avrc_ct_cb(esp_avrc_ct_cb_event_t event,
                       esp_avrc_ct_cb_param_t *param) {
    switch (event) {
    case ESP_AVRC_CT_CONNECTION_STATE_EVT:
        if (param->conn_stat.connected) {
            ESP_LOGI(TAG, "AVRCP controller connected");
        } else {
            ESP_LOGI(TAG, "AVRCP controller disconnected");
        }
        break;
    case ESP_AVRC_CT_PASSTHROUGH_RSP_EVT:
        ESP_LOGI(TAG, "AVRCP passthrough rsp: key=%d state=%d",
                 param->psth_rsp.key_code,
                 param->psth_rsp.key_state);
        break;
    default:
        break;
    }
}

/* ─── Public API ──────────────────────────────────────────────────── */
esp_err_t bt_a2dp_sink_init(const char *device_name) {
    esp_err_t ret;

    // NVS init (BT needs it)
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));

    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    // Set device name visible to phones
    ESP_ERROR_CHECK(esp_bt_gap_set_device_name(device_name));

    // Register callbacks
    ESP_ERROR_CHECK(esp_bt_gap_register_callback(gap_event_cb));

    ESP_ERROR_CHECK(esp_avrc_tg_init());
    ESP_ERROR_CHECK(esp_avrc_tg_register_callback(avrc_tg_cb));

    ESP_ERROR_CHECK(esp_avrc_ct_init());
    ESP_ERROR_CHECK(esp_avrc_ct_register_callback(avrc_ct_cb));

    ESP_ERROR_CHECK(esp_a2d_register_callback(a2dp_event_cb));
    ESP_ERROR_CHECK(esp_a2d_sink_register_data_callback(a2dp_data_cb));
    ESP_ERROR_CHECK(esp_a2d_sink_init());

    // Make device discoverable & connectable
    ESP_ERROR_CHECK(esp_bt_gap_set_scan_mode(
        ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE
    ));

    // Ring buffer allocated in PSRAM
    s_ringbuf = xRingbufferCreate(RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF);
    if (!s_ringbuf) {
        ESP_LOGE(TAG, "Failed to create ring buffer");
        return ESP_ERR_NO_MEM;
    }

    // Audio write task pinned to Core 0 (BT runs on Core 0 too, avoid conflicts)
    xTaskCreatePinnedToCore(
        audio_write_task, "audio_write",
        4096, NULL, 5, &s_writer_task, 1   // pin to Core 1
    );

    ESP_LOGI(TAG, "A2DP sink ready — advertising as \"%s\"", device_name);
    return ESP_OK;
}

void bt_a2dp_sink_deinit(void) {
    if (s_writer_task) {
        vTaskDelete(s_writer_task);
        s_writer_task = NULL;
    }
    if (s_ringbuf) {
        vRingbufferDelete(s_ringbuf);
        s_ringbuf = NULL;
    }
    esp_a2d_sink_deinit();
    esp_avrc_tg_deinit();
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
}

/* Called by HFP after call ends to re-enable A2DP discoverability */
void bt_a2dp_resume(void) {
    ESP_LOGI(TAG, "A2DP resuming after call");
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    // A2DP will auto-resume streaming when the phone sends audio again
}

void bt_a2dp_play_pause(void) {
    ESP_LOGI(TAG, "AVRCP play/pause");
    if (s_a2dp_streaming) {
        esp_avrc_tg_send_rn_rsp(
            ESP_AVRC_RN_PLAY_STATUS_CHANGE,
            ESP_AVRC_RN_RSP_CHANGED,
            NULL
        );
    }
    // Also try passthrough via TG — some phones accept this
    esp_avrc_ct_send_passthrough_cmd(
        0, s_a2dp_streaming ?
            ESP_AVRC_PT_CMD_PAUSE :
            ESP_AVRC_PT_CMD_PLAY,
        ESP_AVRC_PT_CMD_STATE_PRESSED
    );
    esp_avrc_ct_send_passthrough_cmd(
        0, s_a2dp_streaming ?
            ESP_AVRC_PT_CMD_PAUSE :
            ESP_AVRC_PT_CMD_PLAY,
        ESP_AVRC_PT_CMD_STATE_RELEASED
    );
}
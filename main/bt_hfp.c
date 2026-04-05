#include "bt_hfp.h"
#include "i2s_audio.h"
#include <string.h>
#include "esp_hf_client_api.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "driver/i2s_std.h"

#define TAG             "BT_HFP"

#define SCO_RINGBUF_SIZE (32 * 1024) // was 16KB — too small for smooth SCO at 16kHz

/* ─── MIC pins (ICS43434) ────────────────────────────────────────── */
#define MIC_I2S_BCLK   26
#define MIC_I2S_WS     25
#define MIC_I2S_DIN    33

static i2s_chan_handle_t  s_rx_handle    = NULL;
static RingbufHandle_t    s_sco_ringbuf  = NULL;
static bool               s_call_active  = false;
static bool               s_call_ringing = false;
static TaskHandle_t       s_sco_task = NULL;
static esp_bd_addr_t      s_peer_bda = {0};
static TaskHandle_t       s_sco_connect_task = NULL;
static bool               s_vr_active = false;

/* forward declarations */
static void hfp_client_cb(esp_hf_client_cb_event_t event,
                          esp_hf_client_cb_param_t *param);
static void hfp_client_audio_cb(const uint8_t *data, uint32_t len);

/* ─── I2S RX init (mic) ──────────────────────────────────────────── */
static esp_err_t i2s_mic_init(uint32_t sample_rate) {
    if (s_rx_handle) return ESP_OK;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(
        I2S_NUM_1, I2S_ROLE_MASTER
    );
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &s_rx_handle));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_32BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT,
            .slot_mode      = I2S_SLOT_MODE_MONO,
            .slot_mask      = I2S_STD_SLOT_LEFT,   // SEL=GND → left channel
            .ws_width       = 32,
            .ws_pol         = false,
            .bit_shift      = true,                // required for I2S standard mics
        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_I2S_BCLK,
            .ws   = MIC_I2S_WS,
            .dout = I2S_GPIO_UNUSED,
            .din  = MIC_I2S_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx_handle));
    ESP_LOGI(TAG, "Mic I2S RX initialized at %" PRIu32 " Hz", sample_rate);
    return ESP_OK;
}

static void i2s_mic_deinit(void) {
    if (!s_rx_handle) return;
    i2s_channel_disable(s_rx_handle);
    i2s_del_channel(s_rx_handle);
    s_rx_handle = NULL;
    ESP_LOGI(TAG, "Mic I2S RX stopped");
}

/* ─── HFP outgoing data callback (mic → phone) ───────────────────── */
/*
 * The HFP stack calls this when it needs microphone data to send.
 * We read directly from I2S here instead of a ring buffer
 * to minimise mic-to-phone latency.
 */
uint32_t hfp_outgoing_data_cb(uint8_t *p_buf, uint32_t len) {
    if (!s_rx_handle) {
        memset(p_buf, 0, len);
        return len;
    }

    static int32_t raw[120];
    uint32_t samples_needed = len / 2;
    uint32_t read_bytes = samples_needed * 4;
    if (read_bytes > sizeof(raw)) read_bytes = sizeof(raw);

    size_t bytes_read = 0;
    i2s_channel_read(s_rx_handle, raw, read_bytes, &bytes_read, pdMS_TO_TICKS(10));

    uint32_t samples_read = bytes_read / 4;
    int16_t *out = (int16_t *)p_buf;

    #define VOLUME_GAIN 2   // adjust between 0-4 based on call quality

    for (uint32_t i = 0; i < samples_read; i++) {
        int16_t s16 = (int16_t)(raw[i] >> 14);  // matches your working project
        s16 <<= VOLUME_GAIN;
        out[i] = s16;
    }

    if (samples_read < samples_needed) {
        memset(out + samples_read, 0, (samples_needed - samples_read) * 2);
    }

    return len;
}


/* ─── HFP incoming audio callback (phone → speaker) ─────────────── */
static void hfp_client_audio_cb(const uint8_t *data, uint32_t len) {
    if (!s_sco_ringbuf) return;
    xRingbufferSend(s_sco_ringbuf, data, len, pdMS_TO_TICKS(20));  // was 5ms

    // Signal stack to pull mic data — one mic frame per speaker frame received
    esp_hf_client_outgoing_data_ready();
}

/* ─── SCO audio writer task (ring buffer → I2S speaker) ─────────── */
static void sco_write_task(void *arg) {
    // Pre-buffer — wait for ring buffer to fill before starting playback
    // Prevents choppy audio at call start
    vTaskDelay(pdMS_TO_TICKS(80));

    size_t item_size;
    while (1) {
        uint8_t *data = (uint8_t *)xRingbufferReceive(
            s_sco_ringbuf, &item_size, pdMS_TO_TICKS(100)
        );
        if (data) {
            size_t written = 0;
            i2s_speaker_write(data, item_size, &written);
            vRingbufferReturnItem(s_sco_ringbuf, data);
        }
    }
}

/* ─── Audio path switch helpers ──────────────────────────────────── */
static void audio_enter_hfp(uint32_t sample_rate) {
    ESP_LOGI(TAG, "Switching audio path → HFP SCO (%"PRIu32" Hz)", sample_rate);

    // Reconfigure I2S speaker for SCO sample rate (8000 or 16000 Hz)
    i2s_speaker_set_sample_rate(sample_rate);

    // Reconfigure I2S speaker to Mono
    i2s_speaker_set_slot_mode(I2S_SLOT_MODE_MONO);

    // Start mic
    i2s_mic_init(sample_rate);

    // Create SCO ring buffer in PSRAM
    s_sco_ringbuf = xRingbufferCreate(SCO_RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF);

    // SCO writer task on Core 1
    xTaskCreatePinnedToCore(
        sco_write_task, "sco_write",
        8192,               // was 4096
        NULL, 22, &s_sco_task, 1
    );
}

static void audio_leave_hfp(void) {
    // Guard — only run cleanup if something was actually started
    if (!s_sco_ringbuf && !s_rx_handle && !s_sco_task) {
        ESP_LOGI(TAG, "Audio leave HFP — nothing to clean up");
        // Still restore sample rate
        extern uint32_t bt_a2dp_get_sample_rate(void);
        uint32_t rate = bt_a2dp_get_sample_rate();
        ESP_LOGI(TAG, "Restoring I2S to %" PRIu32 " Hz", rate);
        i2s_speaker_set_sample_rate(rate);
        i2s_speaker_set_slot_mode(I2S_SLOT_MODE_STEREO);
        return;
    }

    ESP_LOGI(TAG, "Restoring audio path → A2DP");

    if (s_sco_task) {
        vTaskDelete(s_sco_task);
        s_sco_task = NULL;
    }
    i2s_mic_deinit();

    if (s_sco_ringbuf) {
        vRingbufferDelete(s_sco_ringbuf);
        s_sco_ringbuf = NULL;
    }

    extern uint32_t bt_a2dp_get_sample_rate(void);
    uint32_t rate = bt_a2dp_get_sample_rate();
    ESP_LOGI(TAG, "Restoring I2S to %" PRIu32 " Hz", rate);
    i2s_speaker_set_sample_rate(rate);
    i2s_speaker_set_slot_mode(I2S_SLOT_MODE_STEREO);
}

#define SCO_INITIAL_DELAY_MS 3000   // was 2000 — Samsung needs more time
#define SCO_RETRY_DELAY_MS   3000
#define SCO_RETRY_COUNT      5

/* ─── SCO Connect Task ─────────────────────────────────────────── */
static void sco_connect_task(void *arg) {
    // Longer initial wait — let phone fully establish call first
    vTaskDelay(pdMS_TO_TICKS(SCO_INITIAL_DELAY_MS));

    for (int attempt = 1; attempt <= SCO_RETRY_COUNT; attempt++) {

        if (!s_call_active) {
            ESP_LOGI(TAG, "SCO retry cancelled — call no longer active");
            break;
        }

        if (s_sco_ringbuf != NULL) {
            ESP_LOGI(TAG, "SCO already connected — stopping retries");
            break;
        }

        esp_err_t ret = esp_hf_client_connect_audio(s_peer_bda);
        ESP_LOGI(TAG, "SCO connect attempt %d/%d — result: %s",
                 attempt, SCO_RETRY_COUNT,
                 ret == ESP_OK ? "sent" : esp_err_to_name(ret));

        // Wait before next retry
        if (attempt < SCO_RETRY_COUNT) {
            vTaskDelay(pdMS_TO_TICKS(SCO_RETRY_DELAY_MS));
        }
    }

    s_sco_connect_task = NULL;
    vTaskDelete(NULL);
}

/* ─── HFP event callback ─────────────────────────────────────────── */
static void hfp_client_cb(esp_hf_client_cb_event_t event,
                          esp_hf_client_cb_param_t *param) {
    switch (event) {

    case ESP_HF_CLIENT_CONNECTION_STATE_EVT:
        if (param->conn_stat.state == ESP_HF_CLIENT_CONNECTION_STATE_CONNECTED) {
            // Save peer address for later use with audio connect/disconnect
            memcpy(s_peer_bda, param->conn_stat.remote_bda, sizeof(esp_bd_addr_t));
            ESP_LOGI(TAG, "HFP connected");
        } else if (param->conn_stat.state == ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTED) {
            ESP_LOGI(TAG, "HFP disconnected");
            s_call_active  = false;
            s_call_ringing = false;
        }
        break;

    case ESP_HF_CLIENT_AUDIO_STATE_EVT:
        if (param->audio_stat.state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED) {
            ESP_LOGI(TAG, "SCO audio connected — CVSD narrowband (8000 Hz)");
            audio_enter_hfp(8000);

        } else if (param->audio_stat.state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC) {
            ESP_LOGI(TAG, "SCO audio connected — mSBC wideband (16000 Hz)");
            audio_enter_hfp(16000);

        } else if (param->audio_stat.state == ESP_HF_CLIENT_AUDIO_STATE_DISCONNECTED) {
            ESP_LOGI(TAG, "SCO audio disconnected");
            audio_leave_hfp();
            extern void bt_a2dp_resume(void);
            bt_a2dp_resume();
        }
        break;

    case ESP_HF_CLIENT_CIND_CALL_EVT:
        s_call_active = (param->call.status == ESP_HF_CALL_STATUS_CALL_IN_PROGRESS);
        ESP_LOGI(TAG, "Call status: %s", s_call_active ? "active" : "none");

        if (!s_call_active) {
            // Cancel pending SCO request if any
            if (s_sco_connect_task) {
                vTaskDelete(s_sco_connect_task);
                s_sco_connect_task = NULL;
                ESP_LOGI(TAG, "SCO request cancelled — call ended");
            }
            // Always restore audio path when call ends
            // audio_leave_hfp is safe to call even if SCO was never connected
            audio_leave_hfp();
            extern void bt_a2dp_resume(void);
            bt_a2dp_resume();
        }
        break;

    case ESP_HF_CLIENT_CIND_CALL_SETUP_EVT: {
        esp_hf_call_setup_status_t setup = param->call_setup.status;

        if (setup == ESP_HF_CALL_SETUP_STATUS_INCOMING) {
            s_call_ringing = true;
            ESP_LOGI(TAG, "Incoming call — short press answer, long press reject");

        } else if (setup == ESP_HF_CALL_SETUP_STATUS_IDLE && s_call_active) {
            s_call_ringing = false;
            ESP_LOGI(TAG, "Call active — scheduling SCO request in 1s");
            // Cancel any previous pending request
            if (s_sco_connect_task) {
                vTaskDelete(s_sco_connect_task);
                s_sco_connect_task = NULL;
            }
            xTaskCreatePinnedToCore(
                sco_connect_task, "sco_conn",
                3072,               // was 2048
                NULL, 5, &s_sco_connect_task, 1
            );
        } else if (setup == ESP_HF_CALL_SETUP_STATUS_IDLE && !s_call_active) {
            s_call_ringing = false;
            ESP_LOGI(TAG, "Call ended before answer");
        }
        break;
    }

    case ESP_HF_CLIENT_RING_IND_EVT:
        ESP_LOGI(TAG, "Ring!");
        break;

    case ESP_HF_CLIENT_CLIP_EVT:
        ESP_LOGI(TAG, "Caller ID: %s", param->clip.number ? param->clip.number : "unknown");
        break;

    case ESP_HF_CLIENT_BVRA_EVT:
        if (param->bvra.value) {
            ESP_LOGI(TAG, "Voice recognition started — switching to HFP audio");
            s_vr_active = true;
            // SCO will open automatically — handled by AUDIO_STATE_EVT
        } else {
            ESP_LOGI(TAG, "Voice recognition stopped");
            s_vr_active = false;
            // SCO closes automatically — AUDIO_STATE_EVT DISCONNECTED fires
            // which calls audio_leave_hfp and bt_a2dp_resume
        }
        break;

    default:
        break;
    }
}

/* ─── Public API ─────────────────────────────────────────────────── */
esp_err_t bt_hfp_init(void) {
    ESP_ERROR_CHECK(esp_hf_client_register_callback(hfp_client_cb));

    ESP_ERROR_CHECK(esp_hf_client_init());

    // Register outgoing (mic) data callback
    esp_err_t ret = esp_hf_client_register_data_callback(
        hfp_client_audio_cb,
        hfp_outgoing_data_cb
    );
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register data callback: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "HFP client initialized");
    return ESP_OK;
}

void bt_hfp_deinit(void) {
    audio_leave_hfp();
    esp_hf_client_deinit();
}

bool bt_hfp_is_call_active(void) {
    return s_call_active || s_call_ringing || s_vr_active;
}

void bt_hfp_answer_call(void) {
    ESP_LOGI(TAG, "Answering call");
    esp_hf_client_answer_call();
}

void bt_hfp_reject_call(void) {
    if (s_call_ringing) {
        ESP_LOGI(TAG, "Rejecting incoming call");
        esp_hf_client_reject_call();
    } else if (s_call_active) {
        ESP_LOGI(TAG, "Ending active call");
        esp_hf_client_reject_call(); // AT+CHUP — hangs up active call too
    }
    s_call_active  = false;
    s_call_ringing = false;
}

void bt_hfp_end_call(void) {
    esp_hf_client_reject_call(); // AT+CHUP
    s_call_active  = false;
    s_call_ringing = false;
}

void bt_hfp_trigger_voice_recognition(void) {
    ESP_LOGI(TAG, "Starting voice recognition");
    esp_err_t ret = esp_hf_client_start_voice_recognition();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start voice recognition: %s",
                 esp_err_to_name(ret));
    }
}
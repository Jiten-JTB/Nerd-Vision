#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#include "driver/i2s_std.h" 

esp_err_t i2s_speaker_init(void);
esp_err_t i2s_speaker_write(const uint8_t *data, size_t len, size_t *bytes_written);
esp_err_t i2s_speaker_set_sample_rate(uint32_t sample_rate);
esp_err_t i2s_speaker_set_slot_mode(i2s_slot_mode_t mode);
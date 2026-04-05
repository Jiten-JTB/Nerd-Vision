#pragma once
#include "esp_err.h"

esp_err_t bt_a2dp_sink_init(const char *device_name);
void      bt_a2dp_sink_deinit(void);
void      bt_a2dp_resume(void);
void      bt_a2dp_play_pause(void);
#pragma once
#include "esp_err.h"
#include <stdbool.h>

esp_err_t bt_hfp_init(void);
void      bt_hfp_deinit(void);
bool      bt_hfp_is_call_active(void);
void      bt_hfp_answer_call(void);
void      bt_hfp_reject_call(void);
void      bt_hfp_end_call(void);
void      bt_hfp_trigger_voice_recognition(void);
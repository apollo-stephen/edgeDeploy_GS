#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t inference_start(void);
esp_err_t inference_run_once(void);

#ifdef __cplusplus
}
#endif

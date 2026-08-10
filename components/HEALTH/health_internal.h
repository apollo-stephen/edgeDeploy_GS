#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t health_sample_once(void);
bool health_test_lease_expired(uint64_t now_us);

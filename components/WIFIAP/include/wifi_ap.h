#pragma once

#include "esp_err.h"

esp_err_t wifi_ap_init(void);

/**
 * Returns the SoftAP IPv4 address, or an empty string until initialization
 * succeeds.
 */
const char *wifi_ap_get_ip(void);

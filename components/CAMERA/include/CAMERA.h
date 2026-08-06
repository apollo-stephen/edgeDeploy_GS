#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_camera.h"
#include "esp_err.h"

#define PWDN_GPIO_NUM  -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM  15
#define CAMERA_XCLK_FREQ_HZ 16000000U
#define SIOD_GPIO_NUM  4
#define SIOC_GPIO_NUM  5

#define Y9_GPIO_NUM 16
#define Y8_GPIO_NUM 17
#define Y7_GPIO_NUM 18
#define Y6_GPIO_NUM 12
#define Y5_GPIO_NUM 10
#define Y4_GPIO_NUM 8
#define Y3_GPIO_NUM 9
#define Y2_GPIO_NUM 11

#define VSYNC_GPIO_NUM 6
#define HREF_GPIO_NUM  7
#define PCLK_GPIO_NUM  13

#define CAMERA_FRAME_WIDTH  128
#define CAMERA_FRAME_HEIGHT 128
#define CAMERA_JPEG_QUALITY 12
#define CAMERA_MAX_JPEG_BYTES 8192U

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t camera_init(void);
camera_fb_t *camera_capture_frame(uint32_t timeout_ms);
void camera_release_frame(camera_fb_t *frame);
bool camera_is_ready(void);
const char *camera_frame_size_name(void);

#ifdef __cplusplus
}
#endif

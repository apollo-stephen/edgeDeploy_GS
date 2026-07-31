#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    PIXFORMAT_RGB565,
    PIXFORMAT_JPEG,
} pixformat_t;

typedef enum {
    FRAMESIZE_QQVGA,
    FRAMESIZE_128X128,
} framesize_t;

typedef enum {
    LEDC_TIMER_0,
} ledc_timer_t;

typedef enum {
    LEDC_CHANNEL_0,
} ledc_channel_t;

typedef enum {
    CAMERA_FB_IN_DRAM,
    CAMERA_FB_IN_PSRAM,
} camera_fb_location_t;

typedef enum {
    CAMERA_GRAB_WHEN_EMPTY,
    CAMERA_GRAB_LATEST,
} camera_grab_mode_t;

typedef struct {
    int pin_pwdn;
    int pin_reset;
    int pin_xclk;
    int pin_sccb_sda;
    int pin_sccb_scl;
    int pin_d7;
    int pin_d6;
    int pin_d5;
    int pin_d4;
    int pin_d3;
    int pin_d2;
    int pin_d1;
    int pin_d0;
    int pin_vsync;
    int pin_href;
    int pin_pclk;
    int xclk_freq_hz;
    ledc_timer_t ledc_timer;
    ledc_channel_t ledc_channel;
    pixformat_t pixel_format;
    framesize_t frame_size;
    int jpeg_quality;
    int fb_count;
    camera_fb_location_t fb_location;
    camera_grab_mode_t grab_mode;
} camera_config_t;

typedef struct {
    uint8_t *buf;
    size_t len;
    size_t width;
    size_t height;
    pixformat_t format;
} camera_fb_t;

typedef struct sensor {
    struct {
        uint16_t PID;
    } id;
    int (*set_pixformat)(struct sensor *sensor, pixformat_t format);
    int (*set_framesize)(struct sensor *sensor, framesize_t size);
    int (*set_hmirror)(struct sensor *sensor, int enabled);
    int (*set_vflip)(struct sensor *sensor, int enabled);
} sensor_t;

esp_err_t esp_camera_init(const camera_config_t *config);
sensor_t *esp_camera_sensor_get(void);
camera_fb_t *esp_camera_fb_get(void);
void esp_camera_fb_return(camera_fb_t *frame);

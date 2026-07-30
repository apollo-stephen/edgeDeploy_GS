#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "CAMERA.h"
#include "freertos/semphr.h"

struct fake_semaphore {
    int marker;
};

static struct fake_semaphore s_mutex;
static camera_config_t s_config;
static camera_fb_t s_frame;
static sensor_t s_sensor;
static esp_err_t s_init_result = ESP_OK;
static int s_create_mutex_calls;
static int s_delete_mutex_calls;
static int s_take_result = pdTRUE;
static int s_take_calls;
static TickType_t s_last_timeout;
static int s_give_calls;
static int s_camera_init_calls;
static int s_fb_get_calls;
static int s_fb_return_calls;
static int s_set_pixformat_calls;
static int s_set_framesize_calls;
static int s_set_hmirror_calls;
static int s_set_vflip_calls;
static camera_fb_t *s_next_frame = &s_frame;

const char *esp_err_to_name(esp_err_t err)
{
    (void)err;
    return "test-error";
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    ++s_create_mutex_calls;
    return &s_mutex;
}

int xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t timeout)
{
    assert(semaphore == &s_mutex);
    ++s_take_calls;
    s_last_timeout = timeout;
    return s_take_result;
}

int xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    assert(semaphore == &s_mutex);
    ++s_give_calls;
    return pdTRUE;
}

void vSemaphoreDelete(SemaphoreHandle_t semaphore)
{
    assert(semaphore == &s_mutex);
    ++s_delete_mutex_calls;
}

esp_err_t esp_camera_init(const camera_config_t *config)
{
    ++s_camera_init_calls;
    s_config = *config;
    return s_init_result;
}

static int set_pixformat(sensor_t *sensor, pixformat_t format)
{
    assert(sensor == &s_sensor);
    assert(format == PIXFORMAT_JPEG);
    ++s_set_pixformat_calls;
    return 0;
}

static int set_framesize(sensor_t *sensor, framesize_t size)
{
    assert(sensor == &s_sensor);
    assert(size == FRAMESIZE_128X128);
    ++s_set_framesize_calls;
    return 0;
}

static int set_hmirror(sensor_t *sensor, int enabled)
{
    assert(sensor == &s_sensor);
    assert(enabled == 1);
    ++s_set_hmirror_calls;
    return 0;
}

static int set_vflip(sensor_t *sensor, int enabled)
{
    assert(sensor == &s_sensor);
    assert(enabled == 1);
    ++s_set_vflip_calls;
    return 0;
}

sensor_t *esp_camera_sensor_get(void)
{
    s_sensor.id.PID = 0x5640;
    s_sensor.set_pixformat = set_pixformat;
    s_sensor.set_framesize = set_framesize;
    s_sensor.set_hmirror = set_hmirror;
    s_sensor.set_vflip = set_vflip;
    return &s_sensor;
}

camera_fb_t *esp_camera_fb_get(void)
{
    ++s_fb_get_calls;
    return s_next_frame;
}

void esp_camera_fb_return(camera_fb_t *frame)
{
    assert(frame == &s_frame);
    ++s_fb_return_calls;
}

static void verify_camera_configuration(void)
{
    assert(s_config.pin_pwdn == -1);
    assert(s_config.pin_reset == -1);
    assert(s_config.pin_xclk == 15);
    assert(s_config.pin_sccb_sda == 4);
    assert(s_config.pin_sccb_scl == 5);
    assert(s_config.pin_d0 == 11);
    assert(s_config.pin_d1 == 9);
    assert(s_config.pin_d2 == 8);
    assert(s_config.pin_d3 == 10);
    assert(s_config.pin_d4 == 12);
    assert(s_config.pin_d5 == 18);
    assert(s_config.pin_d6 == 17);
    assert(s_config.pin_d7 == 16);
    assert(s_config.pin_vsync == 6);
    assert(s_config.pin_href == 7);
    assert(s_config.pin_pclk == 13);
    assert(CAMERA_XCLK_FREQ_HZ == 16000000U);
    assert(s_config.xclk_freq_hz == CAMERA_XCLK_FREQ_HZ);
    assert(s_config.pixel_format == PIXFORMAT_JPEG);
    assert(s_config.frame_size == FRAMESIZE_128X128);
    assert(s_config.jpeg_quality == 12);
    assert(s_config.fb_count == 2);
    assert(s_config.fb_location == CAMERA_FB_IN_DRAM);
    assert(s_config.grab_mode == CAMERA_GRAB_LATEST);
}

static void verify_initialization_failure_cleanup(void)
{
    s_init_result = ESP_FAIL;
    assert(camera_init() == ESP_FAIL);
    assert(!camera_is_ready());
    assert(s_create_mutex_calls == 1);
    assert(s_camera_init_calls == 1);
    assert(s_delete_mutex_calls == 1);
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "init-failure") == 0) {
        verify_initialization_failure_cleanup();
        puts("camera initialization failure cleanup passed");
        return 0;
    }

    assert(strcmp(camera_frame_size_name(), "128x128") == 0);
    assert(!camera_is_ready());

    assert(camera_init() == ESP_OK);
    assert(camera_is_ready());
    assert(s_create_mutex_calls == 1);
    assert(s_camera_init_calls == 1);
    verify_camera_configuration();
    assert(s_set_pixformat_calls == 1);
    assert(s_set_framesize_calls == 1);
    assert(s_set_hmirror_calls == 1);
    assert(s_set_vflip_calls == 1);

    assert(camera_init() == ESP_OK);
    assert(s_create_mutex_calls == 1);
    assert(s_camera_init_calls == 1);

    s_frame.buf = (uint8_t *)"jpeg";
    s_frame.len = 4;
    s_frame.width = 128;
    s_frame.height = 128;
    s_frame.format = PIXFORMAT_JPEG;
    assert(camera_capture_frame(250) == &s_frame);
    assert(s_take_calls == 1);
    assert(s_last_timeout == 250);
    assert(s_fb_get_calls == 1);
    camera_release_frame(&s_frame);
    assert(s_fb_return_calls == 1);
    assert(s_give_calls == 1);

    s_next_frame = NULL;
    assert(camera_capture_frame(UINT32_MAX) == NULL);
    assert(s_last_timeout == portMAX_DELAY);
    assert(s_give_calls == 2);

    s_take_result = pdFALSE;
    assert(camera_capture_frame(10) == NULL);
    assert(s_fb_get_calls == 2);
    assert(s_give_calls == 2);

    puts("camera component behavior passed");
    return 0;
}

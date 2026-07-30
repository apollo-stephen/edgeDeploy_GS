#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "CAMERA.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "freertos/task.h"
#include "http_capture.h"
#include "wifi_ap.h"

static httpd_uri_t s_uris[2][8];
static size_t s_uri_count[2];
static httpd_config_t s_server_config[2];
static size_t s_server_count;
static int s_stop_calls[2];
static camera_fb_t s_frame;
static camera_fb_t *s_next_frame = &s_frame;
static int s_release_calls;
static int s_sequence;
static int s_send_sequence;
static int s_release_sequence;
static char s_response_copy[8192];
static uint8_t s_chunk_copy[16384];
static size_t s_chunk_length;
static int s_chunk_calls;
static int64_t s_fake_time_us = 1000000;
static TickType_t s_last_delay_ticks;
static int s_delay_calls;
static const httpd_uri_t *s_stream_uri;
static int s_register_calls;
static int s_fail_register_call;
static bool s_fail_control_stop_once;
static bool s_fail_control_stop;
static bool s_fail_stream_stop;
static pthread_mutex_t s_stream_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_stream_condition = PTHREAD_COND_INITIALIZER;
static bool s_wait_for_stream_exit_on_stop;
static bool s_stream_send_started;
static bool s_stream_stop_called;
static bool s_stream_handler_finished;

static void reset_request(httpd_req_t *request);

const char *esp_err_to_name(esp_err_t err)
{
    (void)err;
    return "test-error";
}

esp_err_t httpd_start(httpd_handle_t *server, const httpd_config_t *config)
{
    assert(s_server_count < 2);
    s_server_config[s_server_count] = *config;
    *server = (httpd_handle_t)(uintptr_t)(s_server_count + 1);
    ++s_server_count;
    return ESP_OK;
}

esp_err_t httpd_stop(httpd_handle_t server)
{
    const size_t index = (size_t)(uintptr_t)server - 1;
    assert(index < 2);
    ++s_stop_calls[index];
    if ((index == 0 && s_fail_control_stop) ||
        (index == 1 && s_fail_stream_stop)) {
        return ESP_FAIL;
    }
    if (index == 0 && s_fail_control_stop_once && s_stop_calls[index] == 1) {
        return ESP_FAIL;
    }
    if (index == 1 && s_wait_for_stream_exit_on_stop) {
        pthread_mutex_lock(&s_stream_mutex);
        s_stream_stop_called = true;
        pthread_cond_broadcast(&s_stream_condition);
        while (!s_stream_handler_finished) {
            pthread_cond_wait(&s_stream_condition, &s_stream_mutex);
        }
        pthread_mutex_unlock(&s_stream_mutex);
    }
    return ESP_OK;
}

esp_err_t httpd_register_uri_handler(httpd_handle_t server,
                                     const httpd_uri_t *uri)
{
    const size_t index = (size_t)(uintptr_t)server - 1;
    assert(index < 2);
    ++s_register_calls;
    if (s_fail_register_call == s_register_calls) {
        return ESP_FAIL;
    }
    assert(s_uri_count[index] < 8);
    s_uris[index][s_uri_count[index]++] = *uri;
    return ESP_OK;
}

esp_err_t httpd_resp_set_type(httpd_req_t *request, const char *type)
{
    request->response_type = type;
    return ESP_OK;
}

esp_err_t httpd_resp_set_status(httpd_req_t *request, const char *status)
{
    request->response_status = status;
    return ESP_OK;
}

esp_err_t httpd_resp_set_hdr(httpd_req_t *request,
                             const char *name,
                             const char *value)
{
    assert(request->header_count < 12);
    request->header_names[request->header_count] = name;
    request->header_values[request->header_count] = strdup(value);
    ++request->header_count;
    return ESP_OK;
}

esp_err_t httpd_resp_send(httpd_req_t *request,
                          const char *body,
                          long length)
{
    ++s_sequence;
    s_send_sequence = s_sequence;
    const size_t body_length = length == HTTPD_RESP_USE_STRLEN
                                   ? strlen(body)
                                   : (size_t)length;
    assert(body_length < sizeof(s_response_copy));
    memcpy(s_response_copy, body, body_length);
    s_response_copy[body_length] = '\0';
    request->response_body = s_response_copy;
    request->response_length = body_length;
    return ESP_OK;
}

esp_err_t httpd_resp_sendstr(httpd_req_t *request, const char *body)
{
    return httpd_resp_send(request, body, HTTPD_RESP_USE_STRLEN);
}

esp_err_t httpd_resp_send_chunk(httpd_req_t *request,
                                const char *chunk,
                                ssize_t length)
{
    (void)request;
    ++s_chunk_calls;
    s_fake_time_us += 1000;

    pthread_mutex_lock(&s_stream_mutex);
    s_stream_send_started = true;
    pthread_cond_broadcast(&s_stream_condition);
    while (s_wait_for_stream_exit_on_stop && !s_stream_stop_called) {
        pthread_cond_wait(&s_stream_condition, &s_stream_mutex);
    }
    pthread_mutex_unlock(&s_stream_mutex);

    if (s_chunk_calls == 4) {
        return ESP_FAIL;
    }

    assert(chunk != NULL);
    assert(length >= 0);
    assert(s_chunk_length + (size_t)length < sizeof(s_chunk_copy));
    memcpy(s_chunk_copy + s_chunk_length, chunk, (size_t)length);
    s_chunk_length += (size_t)length;
    return ESP_OK;
}

esp_err_t httpd_resp_send_err(httpd_req_t *request,
                              const char *status,
                              const char *message)
{
    httpd_resp_set_status(request, status);
    return httpd_resp_sendstr(request, message);
}

uint32_t esp_get_free_heap_size(void)
{
    return 123456;
}

size_t heap_caps_get_free_size(unsigned int capabilities)
{
    assert(capabilities == MALLOC_CAP_SPIRAM);
    return 654321;
}

int64_t esp_timer_get_time(void)
{
    return s_fake_time_us;
}

void vTaskDelay(TickType_t ticks)
{
    s_last_delay_ticks = ticks;
    ++s_delay_calls;
    s_fake_time_us += (int64_t)ticks * 1000;
}

camera_fb_t *camera_capture_frame(uint32_t timeout_ms)
{
    assert(timeout_ms == 2000);
    return s_next_frame;
}

void camera_release_frame(camera_fb_t *frame)
{
    assert(frame == &s_frame);
    ++s_sequence;
    s_release_sequence = s_sequence;
    ++s_release_calls;
}

bool camera_is_ready(void)
{
    return true;
}

const char *camera_frame_size_name(void)
{
    return "128x128";
}

esp_err_t camera_init(void)
{
    return ESP_OK;
}

const char *wifi_ap_get_ip(void)
{
    return "192.168.4.1";
}

esp_err_t wifi_ap_init(void)
{
    return ESP_OK;
}

static const httpd_uri_t *find_uri(size_t server_index, const char *uri)
{
    assert(server_index < 2);
    for (size_t index = 0; index < s_uri_count[server_index]; ++index) {
        if (strcmp(s_uris[server_index][index].uri, uri) == 0) {
            return &s_uris[server_index][index];
        }
    }
    return NULL;
}

static void verify_stream(const httpd_uri_t *stream_uri)
{
    static uint8_t jpeg[] = {0xff, 0xd8, 0xff, 0xd9};
    s_frame.buf = jpeg;
    s_frame.len = sizeof(jpeg);
    s_frame.width = 128;
    s_frame.height = 128;
    s_frame.format = PIXFORMAT_JPEG;
    s_next_frame = &s_frame;
    s_stream_uri = stream_uri;
    s_chunk_length = 0;
    s_chunk_calls = 0;
    s_delay_calls = 0;
    const int releases_before = s_release_calls;

    httpd_req_t request = {0};
    assert(stream_uri->handler(&request) == ESP_FAIL);
    assert(strcmp(request.response_type,
                  "multipart/x-mixed-replace;"
                  "boundary=123456789000000000000987654321") == 0);
    assert(s_chunk_calls == 4);
    assert(s_release_calls == releases_before + 2);
    assert(s_delay_calls == 1);
    assert(s_last_delay_ticks == 64);
    assert(s_chunk_length > sizeof(jpeg));
    assert(memcmp(s_chunk_copy + s_chunk_length - sizeof(jpeg),
                  jpeg,
                  sizeof(jpeg)) == 0);
    assert(strstr((const char *)s_chunk_copy, "Content-Type: image/jpeg") != NULL);
    assert(strstr((const char *)s_chunk_copy, "Content-Length: 4") != NULL);
    reset_request(&request);
}

typedef struct {
    const httpd_uri_t *stream_uri;
    httpd_req_t request;
    esp_err_t result;
} stream_thread_context_t;

static void *run_stream_handler(void *argument)
{
    stream_thread_context_t *context = argument;
    context->result = context->stream_uri->handler(&context->request);

    pthread_mutex_lock(&s_stream_mutex);
    s_stream_handler_finished = true;
    pthread_cond_broadcast(&s_stream_condition);
    pthread_mutex_unlock(&s_stream_mutex);
    return NULL;
}

static void verify_stream_stops_cleanly(const httpd_uri_t *stream_uri)
{
    s_stream_uri = stream_uri;
    s_chunk_length = 0;
    s_chunk_calls = 0;
    s_delay_calls = 0;
    s_wait_for_stream_exit_on_stop = true;
    s_stream_send_started = false;
    s_stream_stop_called = false;
    s_stream_handler_finished = false;
    const int releases_before = s_release_calls;

    stream_thread_context_t context = {
        .stream_uri = stream_uri,
        .request = {0},
        .result = ESP_FAIL,
    };
    pthread_t stream_thread;
    assert(pthread_create(&stream_thread,
                          NULL,
                          run_stream_handler,
                          &context) == 0);

    pthread_mutex_lock(&s_stream_mutex);
    while (!s_stream_send_started) {
        pthread_cond_wait(&s_stream_condition, &s_stream_mutex);
    }
    pthread_mutex_unlock(&s_stream_mutex);

    http_capture_stop();
    assert(pthread_join(stream_thread, NULL) == 0);
    assert(context.result == ESP_OK);
    assert(s_chunk_calls == 3);
    assert(s_release_calls == releases_before + 1);
    assert(s_delay_calls == 0);
    assert(s_stop_calls[0] == 1);
    assert(s_stop_calls[1] == 1);
    reset_request(&context.request);
    s_wait_for_stream_exit_on_stop = false;
}

static void verify_failed_rollback_keeps_server_handle(void)
{
    memset(s_uri_count, 0, sizeof(s_uri_count));
    memset(s_stop_calls, 0, sizeof(s_stop_calls));
    s_server_count = 0;
    s_register_calls = 0;
    s_fail_register_call = 1;
    s_fail_control_stop_once = true;

    assert(http_capture_start() == ESP_FAIL);
    assert(s_server_count == 1);
    assert(s_stop_calls[0] == 1);

    http_capture_stop();
    assert(s_stop_calls[0] == 2);
}

static void verify_start_retries_incomplete_stop(void)
{
    memset(s_uri_count, 0, sizeof(s_uri_count));
    memset(s_stop_calls, 0, sizeof(s_stop_calls));
    s_server_count = 0;
    s_register_calls = 0;
    s_fail_register_call = 0;
    s_fail_control_stop_once = false;

    assert(http_capture_start() == ESP_OK);
    s_fail_control_stop = true;
    s_fail_stream_stop = true;
    http_capture_stop();
    assert(s_stop_calls[0] == 1);
    assert(s_stop_calls[1] == 1);

    assert(http_capture_start() == ESP_FAIL);
    assert(s_stop_calls[0] == 2);
    assert(s_stop_calls[1] == 2);

    s_fail_control_stop = false;
    s_fail_stream_stop = false;
    http_capture_stop();
    assert(s_stop_calls[0] == 3);
    assert(s_stop_calls[1] == 3);
}

static const char *find_header(const httpd_req_t *request, const char *name)
{
    for (size_t index = 0; index < request->header_count; ++index) {
        if (strcmp(request->header_names[index], name) == 0) {
            return request->header_values[index];
        }
    }
    return NULL;
}

static void reset_request(httpd_req_t *request)
{
    for (size_t index = 0; index < request->header_count; ++index) {
        free((void *)request->header_values[index]);
    }
    memset(request, 0, sizeof(*request));
    memset(s_response_copy, 0, sizeof(s_response_copy));
}

static void verify_preview_page(const httpd_uri_t *index_uri)
{
    httpd_req_t request = {0};
    assert(index_uri->handler(&request) == ESP_OK);
    assert(strcmp(request.response_type, "text/html; charset=utf-8") == 0);
    assert(strstr(request.response_body, "Capture now") != NULL);
    assert(strstr(request.response_body, "Pause preview") != NULL);
    assert(strstr(request.response_body, "width:128px;height:128px") != NULL);
    assert(strstr(request.response_body,
                  "const streamUrl=`http://${location.hostname}:81/stream`;") != NULL);
    assert(strstr(request.response_body,
                  "preview.src=`${streamUrl}?t=${Date.now()}`;") != NULL);
    assert(strstr(request.response_body, "window.open") != NULL);
    assert(strstr(request.response_body, "Date.now()") != NULL);
    assert(strstr(request.response_body, "setInterval") == NULL);
    assert(strstr(request.response_body, "response.blob()") == NULL);
    assert(strstr(request.response_body, "URL.createObjectURL") == NULL);
    assert(strstr(request.response_body, "fetch(`/capture") == NULL);
    assert(strcmp(find_header(&request, "Cache-Control"), "no-store") == 0);
    reset_request(&request);
}

static void verify_valid_capture(const httpd_uri_t *capture_uri)
{
    static uint8_t jpeg[] = {0xff, 0xd8, 0xff, 0xd9};
    s_frame.buf = jpeg;
    s_frame.len = sizeof(jpeg);
    s_frame.width = 128;
    s_frame.height = 128;
    s_frame.format = PIXFORMAT_JPEG;
    s_next_frame = &s_frame;
    s_sequence = 0;
    s_send_sequence = 0;
    s_release_sequence = 0;

    httpd_req_t request = {0};
    assert(capture_uri->handler(&request) == ESP_OK);
    assert(strcmp(request.response_type, "image/jpeg") == 0);
    assert(request.response_length == sizeof(jpeg));
    assert(memcmp(request.response_body, jpeg, sizeof(jpeg)) == 0);
    assert(strcmp(find_header(&request, "X-Frame-Width"), "128") == 0);
    assert(strcmp(find_header(&request, "X-Frame-Height"), "128") == 0);
    assert(strstr(find_header(&request, "Cache-Control"), "no-store") != NULL);
    assert(s_send_sequence > 0);
    assert(s_release_sequence > s_send_sequence);
    reset_request(&request);
}

static void verify_capture_failures(const httpd_uri_t *capture_uri)
{
    const int releases_before = s_release_calls;
    s_next_frame = NULL;
    httpd_req_t request = {0};
    assert(capture_uri->handler(&request) == ESP_OK);
    assert(strcmp(request.response_status, "503 Service Unavailable") == 0);
    assert(s_release_calls == releases_before);
    reset_request(&request);

    s_next_frame = &s_frame;
    s_frame.format = PIXFORMAT_RGB565;
    assert(capture_uri->handler(&request) == ESP_OK);
    assert(strcmp(request.response_status,
                  HTTPD_500_INTERNAL_SERVER_ERROR) == 0);
    assert(s_release_calls == releases_before + 1);
    reset_request(&request);

    s_frame.format = PIXFORMAT_JPEG;
    s_frame.width = 160;
    s_frame.height = 120;
    assert(capture_uri->handler(&request) == ESP_OK);
    assert(strcmp(request.response_status,
                  HTTPD_500_INTERNAL_SERVER_ERROR) == 0);
    assert(s_release_calls == releases_before + 2);
    reset_request(&request);
}

static void verify_status(const httpd_uri_t *status_uri)
{
    httpd_req_t request = {0};
    assert(status_uri->handler(&request) == ESP_OK);
    assert(strcmp(request.response_type, "application/json") == 0);
    assert(strstr(request.response_body, "\"camera_ready\":true") != NULL);
    assert(strstr(request.response_body, "\"frame_size\":\"128x128\"") != NULL);
    assert(strstr(request.response_body,
                  "\"camera_xclk_hz\":16000000") != NULL);
    assert(strstr(request.response_body,
                  "\"stream_target_fps\":15") != NULL);
    assert(strstr(request.response_body,
                  "\"stream_client_connected\":false") != NULL);
    assert(strstr(request.response_body, "\"stream_frame_count\":") != NULL);
    assert(strstr(request.response_body, "\"stream_failures\":0") != NULL);
    assert(strstr(request.response_body, "\"stream_fps\":") != NULL);
    assert(strstr(request.response_body, "\"free_heap_bytes\":123456") != NULL);
    assert(strstr(request.response_body, "\"free_psram_bytes\":654321") != NULL);
    reset_request(&request);
}

int main(void)
{
    assert(http_capture_start() == ESP_OK);
    assert(http_capture_start() == ESP_OK);
    assert(s_server_count == 2);
    assert(s_uri_count[0] == 3);
    assert(s_uri_count[1] == 1);
    assert(s_server_config[0].server_port == 80);
    assert(s_server_config[0].ctrl_port == 32768);
    assert(s_server_config[0].stack_size == 8192);
    assert(s_server_config[0].max_uri_handlers == 8);
    assert(s_server_config[0].lru_purge_enable);
    assert(s_server_config[0].max_open_sockets == 3);
    assert(s_server_config[0].send_wait_timeout == 5);
    assert(s_server_config[1].server_port == 81);
    assert(s_server_config[1].ctrl_port == 32769);
    assert(s_server_config[1].stack_size == 8192);
    assert(s_server_config[1].max_uri_handlers == 1);
    assert(!s_server_config[1].lru_purge_enable);
    assert(s_server_config[1].max_open_sockets == 1);
    assert(s_server_config[1].send_wait_timeout == 1);

    const httpd_uri_t *index_uri = find_uri(0, "/");
    const httpd_uri_t *capture_uri = find_uri(0, "/capture");
    const httpd_uri_t *status_uri = find_uri(0, "/api/status");
    const httpd_uri_t *stream_uri = find_uri(1, "/stream");
    assert(index_uri != NULL);
    assert(capture_uri != NULL);
    assert(status_uri != NULL);
    assert(stream_uri != NULL);

    verify_preview_page(index_uri);
    verify_valid_capture(capture_uri);
    verify_capture_failures(capture_uri);
    verify_stream(stream_uri);
    verify_status(status_uri);
    verify_stream_stops_cleanly(stream_uri);

    http_capture_stop();
    http_capture_stop();
    assert(s_stop_calls[0] == 1);
    assert(s_stop_calls[1] == 1);

    verify_failed_rollback_keeps_server_handle();
    verify_start_retries_incomplete_stop();

    puts("http capture component behavior passed");
    return 0;
}

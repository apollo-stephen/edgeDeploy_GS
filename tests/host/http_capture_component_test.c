#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "CAMERA.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "http_capture.h"
#include "wifi_ap.h"

static httpd_uri_t s_uris[8];
static size_t s_uri_count;
static httpd_config_t s_server_config;
static int s_stop_calls;
static camera_fb_t s_frame;
static camera_fb_t *s_next_frame = &s_frame;
static int s_release_calls;
static int s_sequence;
static int s_send_sequence;
static int s_release_sequence;
static char s_response_copy[8192];

const char *esp_err_to_name(esp_err_t err)
{
    (void)err;
    return "test-error";
}

esp_err_t httpd_start(httpd_handle_t *server, const httpd_config_t *config)
{
    s_server_config = *config;
    *server = (httpd_handle_t)0x1;
    return ESP_OK;
}

esp_err_t httpd_stop(httpd_handle_t server)
{
    assert(server == (httpd_handle_t)0x1);
    ++s_stop_calls;
    return ESP_OK;
}

esp_err_t httpd_register_uri_handler(httpd_handle_t server,
                                     const httpd_uri_t *uri)
{
    assert(server == (httpd_handle_t)0x1);
    assert(s_uri_count < 8);
    s_uris[s_uri_count++] = *uri;
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

static const httpd_uri_t *find_uri(const char *uri)
{
    for (size_t index = 0; index < s_uri_count; ++index) {
        if (strcmp(s_uris[index].uri, uri) == 0) {
            return &s_uris[index];
        }
    }
    return NULL;
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
    assert(strstr(request.response_body, "Pause auto refresh") != NULL);
    assert(strstr(request.response_body, "setInterval") != NULL);
    assert(strstr(request.response_body, "Date.now()") != NULL);
    assert(strstr(request.response_body, "inFlight") != NULL);
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
    assert(strstr(request.response_body, "\"free_heap_bytes\":123456") != NULL);
    assert(strstr(request.response_body, "\"free_psram_bytes\":654321") != NULL);
    reset_request(&request);
}

int main(void)
{
    assert(http_capture_start() == ESP_OK);
    assert(http_capture_start() == ESP_OK);
    assert(s_uri_count == 3);
    assert(s_server_config.stack_size == 8192);
    assert(s_server_config.max_uri_handlers == 8);
    assert(s_server_config.lru_purge_enable);

    const httpd_uri_t *index_uri = find_uri("/");
    const httpd_uri_t *capture_uri = find_uri("/capture");
    const httpd_uri_t *status_uri = find_uri("/api/status");
    assert(index_uri != NULL);
    assert(capture_uri != NULL);
    assert(status_uri != NULL);

    verify_preview_page(index_uri);
    verify_valid_capture(capture_uri);
    verify_capture_failures(capture_uri);
    verify_status(status_uri);

    http_capture_stop();
    http_capture_stop();
    assert(s_stop_calls == 1);

    puts("http capture component behavior passed");
    return 0;
}

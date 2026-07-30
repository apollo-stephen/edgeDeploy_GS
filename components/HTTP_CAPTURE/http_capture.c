#include "http_capture.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "CAMERA.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_ap.h"

#define CAPTURE_TIMEOUT_MS 2000
#define STREAM_SERVER_PORT 81
#define STREAM_TARGET_FPS 15
#define STREAM_FRAME_PERIOD_MS 67
#define STREAM_RETRY_DELAY_MS 20
#define STREAM_MAX_CAPTURE_FAILURES 3
#define PART_BOUNDARY "123456789000000000000987654321"

static const char *TAG = "http_capture";
static const char *STREAM_CONTENT_TYPE =
    "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *STREAM_PART =
    "Content-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n";

static httpd_handle_t s_control_server;
static httpd_handle_t s_stream_server;
static uint32_t s_capture_count;
static uint32_t s_capture_failures;
static size_t s_last_capture_size;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_stream_client_connected;
static bool s_stream_stop_requested;
static uint32_t s_stream_frame_count;
static uint32_t s_stream_failures;
static double s_stream_fps;

static const char INDEX_HTML[] =
    "<!doctype html><html lang=\"en\"><head>"
    "<meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>EdgeDeploy camera</title>"
    "<style>"
    "body{font-family:system-ui,sans-serif;max-width:560px;margin:2rem auto;"
    "padding:0 1rem;background:#f5f7fa;color:#172033}"
    ".card{background:white;border-radius:16px;padding:1.25rem;"
    "box-shadow:0 8px 28px #18243a1f}"
    "img{display:block;width:128px;height:128px;max-width:100%;"
    "margin:1rem auto;image-rendering:auto;background:#e7ebf0;"
    "border-radius:10px;object-fit:contain}"
    ".controls{display:flex;gap:.75rem;flex-wrap:wrap}"
    "button{border:0;border-radius:9px;padding:.7rem 1rem;font-weight:650;"
    "cursor:pointer;background:#155eef;color:white}"
    "button.secondary{background:#e7ecf5;color:#172033}"
    "#statusText{min-height:1.5rem;color:#46546a}"
    "</style></head><body><main class=\"card\">"
    "<h1>OV5640 capture</h1>"
    "<p>Native 128x128 MJPEG preview at 15 FPS</p>"
    "<img id=\"preview\" alt=\"Camera capture\">"
    "<p id=\"statusText\">Starting preview...</p>"
    "<div class=\"controls\">"
    "<button id=\"captureButton\" type=\"button\">Capture now</button>"
    "<button id=\"streamButton\" class=\"secondary\" type=\"button\">"
    "Pause preview</button></div>"
    "<script>"
    "const preview=document.getElementById('preview');"
    "const statusText=document.getElementById('statusText');"
    "const captureButton=document.getElementById('captureButton');"
    "const streamButton=document.getElementById('streamButton');"
    "const streamUrl=`http://${location.hostname}:81/stream`;"
    "let streaming=false;"
    "function startStream(){"
    "preview.src=`${streamUrl}?t=${Date.now()}`;"
    "streaming=true;"
    "streamButton.textContent='Pause preview';"
    "statusText.textContent='Streaming at up to 15 FPS';"
    "}"
    "function stopStream(){"
    "preview.removeAttribute('src');"
    "streaming=false;"
    "streamButton.textContent='Resume preview';"
    "statusText.textContent='Preview paused';"
    "}"
    "captureButton.addEventListener('click',()=>{"
    "window.open(`/capture?t=${Date.now()}`,'_blank','noopener');"
    "});"
    "streamButton.addEventListener('click',()=>{"
    "if(streaming)stopStream();else startStream();"
    "});"
    "preview.addEventListener('error',()=>{"
    "if(streaming)statusText.textContent='Stream disconnected; pause and resume to retry';"
    "});"
    "startStream();"
    "</script></main></body></html>";

static esp_err_t index_get_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static bool frame_is_valid_jpeg(const camera_fb_t *frame)
{
    return frame != NULL &&
           frame->format == PIXFORMAT_JPEG &&
           frame->buf != NULL &&
           frame->len > 0 &&
           frame->width == CAMERA_FRAME_WIDTH &&
           frame->height == CAMERA_FRAME_HEIGHT;
}

static bool stream_claim_client(void)
{
    bool claimed = false;
    portENTER_CRITICAL(&s_state_lock);
    if (!s_stream_client_connected && !s_stream_stop_requested) {
        s_stream_client_connected = true;
        claimed = true;
    }
    portEXIT_CRITICAL(&s_state_lock);
    return claimed;
}

static bool stream_stop_is_requested(void)
{
    bool stop_requested;
    portENTER_CRITICAL(&s_state_lock);
    stop_requested = s_stream_stop_requested;
    portEXIT_CRITICAL(&s_state_lock);
    return stop_requested;
}

static void stream_set_stop_requested(bool stop_requested)
{
    portENTER_CRITICAL(&s_state_lock);
    s_stream_stop_requested = stop_requested;
    portEXIT_CRITICAL(&s_state_lock);
}

static void stream_release_client(void)
{
    portENTER_CRITICAL(&s_state_lock);
    s_stream_client_connected = false;
    portEXIT_CRITICAL(&s_state_lock);
}

static void stream_record_failure(void)
{
    portENTER_CRITICAL(&s_state_lock);
    ++s_stream_failures;
    portEXIT_CRITICAL(&s_state_lock);
}

static void stream_record_frame(uint32_t connection_frames,
                                int64_t stream_started_us,
                                int64_t now_us)
{
    double fps = 0.0;
    if (now_us > stream_started_us) {
        fps = (double)connection_frames * 1000000.0 /
              (double)(now_us - stream_started_us);
    }

    portENTER_CRITICAL(&s_state_lock);
    ++s_stream_frame_count;
    s_stream_fps = fps;
    portEXIT_CRITICAL(&s_state_lock);
}

static esp_err_t status_get_handler(httpd_req_t *request)
{
    bool stream_client_connected;
    uint32_t stream_frame_count;
    uint32_t stream_failures;
    double stream_fps;
    portENTER_CRITICAL(&s_state_lock);
    stream_client_connected = s_stream_client_connected;
    stream_frame_count = s_stream_frame_count;
    stream_failures = s_stream_failures;
    stream_fps = s_stream_fps;
    portEXIT_CRITICAL(&s_state_lock);

    char response[640];
    const size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const int length = snprintf(
        response,
        sizeof(response),
        "{\"camera_ready\":%s,\"frame_size\":\"%s\","
        "\"camera_xclk_hz\":%u,\"stream_target_fps\":%u,"
        "\"stream_client_connected\":%s,"
        "\"stream_frame_count\":%" PRIu32 ","
        "\"stream_failures\":%" PRIu32 ",\"stream_fps\":%.2f,"
        "\"capture_count\":%" PRIu32 ",\"capture_failures\":%" PRIu32 ","
        "\"last_capture_bytes\":%u,\"free_heap_bytes\":%u,"
        "\"free_psram_bytes\":%u}",
        camera_is_ready() ? "true" : "false",
        camera_frame_size_name(),
        (unsigned int)CAMERA_XCLK_FREQ_HZ,
        (unsigned int)STREAM_TARGET_FPS,
        stream_client_connected ? "true" : "false",
        stream_frame_count,
        stream_failures,
        stream_fps,
        s_capture_count,
        s_capture_failures,
        (unsigned int)s_last_capture_size,
        (unsigned int)esp_get_free_heap_size(),
        (unsigned int)free_psram);

    if (length < 0 || length >= (int)sizeof(response)) {
        return httpd_resp_send_err(request,
                                   HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Status response overflow");
    }

    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, response, length);
}

static esp_err_t capture_get_handler(httpd_req_t *request)
{
    camera_fb_t *frame = camera_capture_frame(CAPTURE_TIMEOUT_MS);
    if (frame == NULL) {
        ++s_capture_failures;
        httpd_resp_set_status(request, "503 Service Unavailable");
        httpd_resp_set_type(request, "text/plain; charset=utf-8");
        return httpd_resp_sendstr(request, "Camera is busy or capture failed");
    }

    if (!frame_is_valid_jpeg(frame)) {
        ++s_capture_failures;
        camera_release_frame(frame);
        return httpd_resp_send_err(request,
                                   HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Camera returned an invalid 128x128 JPEG");
    }

    char width[12];
    char height[12];
    snprintf(width, sizeof(width), "%zu", frame->width);
    snprintf(height, sizeof(height), "%zu", frame->height);

    httpd_resp_set_type(request, "image/jpeg");
    httpd_resp_set_hdr(request,
                       "Cache-Control",
                       "no-store, no-cache, must-revalidate");
    httpd_resp_set_hdr(request, "Pragma", "no-cache");
    httpd_resp_set_hdr(request, "X-Frame-Width", width);
    httpd_resp_set_hdr(request, "X-Frame-Height", height);
    httpd_resp_set_hdr(request,
                       "Content-Disposition",
                       "inline; filename=edgedeploy.jpg");

    const size_t frame_size = frame->len;
    const esp_err_t result = httpd_resp_send(request,
                                             (const char *)frame->buf,
                                             frame->len);
    camera_release_frame(frame);

    if (result == ESP_OK) {
        s_last_capture_size = frame_size;
        ++s_capture_count;
    } else {
        ++s_capture_failures;
        ESP_LOGW(TAG,
                 "Failed to send captured JPEG: %s",
                 esp_err_to_name(result));
    }

    return result;
}

static esp_err_t stream_get_handler(httpd_req_t *request)
{
    if (!stream_claim_client()) {
        httpd_resp_set_status(request, "503 Service Unavailable");
        httpd_resp_set_type(request, "text/plain; charset=utf-8");
        return httpd_resp_sendstr(request, "MJPEG stream already in use");
    }

    ESP_LOGI(TAG, "MJPEG client connected");
    esp_err_t result = httpd_resp_set_type(request, STREAM_CONTENT_TYPE);
    if (result == ESP_OK) {
        result = httpd_resp_set_hdr(request,
                                    "Cache-Control",
                                    "no-store, no-cache, must-revalidate");
    }

    const int64_t stream_started_us = esp_timer_get_time();
    uint32_t connection_frames = 0;
    unsigned int consecutive_capture_failures = 0;

    while (result == ESP_OK && !stream_stop_is_requested()) {
        const int64_t frame_started_us = esp_timer_get_time();
        camera_fb_t *frame = camera_capture_frame(CAPTURE_TIMEOUT_MS);
        if (frame == NULL) {
            if (stream_stop_is_requested()) {
                break;
            }
            stream_record_failure();
            ++consecutive_capture_failures;
            if (consecutive_capture_failures >=
                STREAM_MAX_CAPTURE_FAILURES) {
                result = ESP_FAIL;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(STREAM_RETRY_DELAY_MS));
            continue;
        }

        if (!frame_is_valid_jpeg(frame)) {
            camera_release_frame(frame);
            stream_record_failure();
            ++consecutive_capture_failures;
            if (consecutive_capture_failures >=
                STREAM_MAX_CAPTURE_FAILURES) {
                result = ESP_FAIL;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(STREAM_RETRY_DELAY_MS));
            continue;
        }

        consecutive_capture_failures = 0;
        char part_header[96];
        const int header_length = snprintf(part_header,
                                           sizeof(part_header),
                                           STREAM_PART,
                                           frame->len);
        if (header_length < 0 ||
            header_length >= (int)sizeof(part_header)) {
            stream_record_failure();
            result = ESP_FAIL;
        }
        if (result == ESP_OK) {
            result = httpd_resp_send_chunk(request,
                                           STREAM_BOUNDARY,
                                           strlen(STREAM_BOUNDARY));
        }
        if (result == ESP_OK) {
            result = httpd_resp_send_chunk(request,
                                           part_header,
                                           (size_t)header_length);
        }
        if (result == ESP_OK) {
            result = httpd_resp_send_chunk(request,
                                           (const char *)frame->buf,
                                           frame->len);
        }
        camera_release_frame(frame);

        if (result != ESP_OK) {
            break;
        }

        ++connection_frames;
        const int64_t frame_finished_us = esp_timer_get_time();
        stream_record_frame(connection_frames,
                            stream_started_us,
                            frame_finished_us);

        if (stream_stop_is_requested()) {
            break;
        }

        const int64_t elapsed_us = frame_finished_us - frame_started_us;
        const int64_t target_us =
            (int64_t)STREAM_FRAME_PERIOD_MS * 1000;
        if (elapsed_us < target_us) {
            const int64_t remaining_us = target_us - elapsed_us;
            const TickType_t delay_ticks =
                pdMS_TO_TICKS((remaining_us + 999) / 1000);
            if (delay_ticks > 0) {
                vTaskDelay(delay_ticks);
            }
        }
    }

    stream_release_client();
    ESP_LOGI(TAG,
             "MJPEG client disconnected after %" PRIu32 " frames",
             connection_frames);
    return result;
}

static esp_err_t register_control_handlers(httpd_handle_t server)
{
    const httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_get_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t capture_uri = {
        .uri = "/capture",
        .method = HTTP_GET,
        .handler = capture_get_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t status_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = status_get_handler,
        .user_ctx = NULL,
    };

    esp_err_t err = httpd_register_uri_handler(server, &index_uri);
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(server, &capture_uri);
    }
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(server, &status_uri);
    }
    return err;
}

static esp_err_t register_stream_handler(httpd_handle_t server)
{
    const httpd_uri_t stream_uri = {
        .uri = "/stream",
        .method = HTTP_GET,
        .handler = stream_get_handler,
        .user_ctx = NULL,
    };
    return httpd_register_uri_handler(server, &stream_uri);
}

static esp_err_t stop_server(httpd_handle_t *server, const char *description)
{
    (void)description;
    if (*server == NULL) {
        return ESP_OK;
    }

    const esp_err_t err = httpd_stop(*server);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "%s stop failed: %s",
                 description,
                 esp_err_to_name(err));
        return err;
    }

    *server = NULL;
    return ESP_OK;
}

esp_err_t http_capture_start(void)
{
    if (s_control_server != NULL &&
        s_stream_server != NULL &&
        !stream_stop_is_requested()) {
        return ESP_OK;
    }
    if (s_control_server != NULL || s_stream_server != NULL) {
        http_capture_stop();
        if (s_control_server != NULL || s_stream_server != NULL) {
            return ESP_FAIL;
        }
    }

    stream_set_stop_requested(false);

    httpd_config_t control_config = HTTPD_DEFAULT_CONFIG();
    control_config.stack_size = 8192;
    control_config.max_uri_handlers = 8;
    control_config.max_open_sockets = 3;
    control_config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_control_server, &control_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "HTTP server startup failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    err = register_control_handlers(s_control_server);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "HTTP handler registration failed: %s",
                 esp_err_to_name(err));
        stop_server(&s_control_server, "HTTP server");
        return err;
    }

    httpd_config_t stream_config = HTTPD_DEFAULT_CONFIG();
    stream_config.server_port = STREAM_SERVER_PORT;
    stream_config.ctrl_port += 1;
    stream_config.stack_size = 8192;
    stream_config.max_uri_handlers = 1;
    stream_config.max_open_sockets = 1;
    stream_config.lru_purge_enable = false;
    stream_config.send_wait_timeout = 1;

    err = httpd_start(&s_stream_server, &stream_config);
    if (err == ESP_OK) {
        err = register_stream_handler(s_stream_server);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "MJPEG stream server startup failed: %s",
                 esp_err_to_name(err));
        stop_server(&s_stream_server, "MJPEG stream server");
        stop_server(&s_control_server, "HTTP server");
        return err;
    }

    ESP_LOGI(TAG,
             "Capture page ready at http://%s/",
             wifi_ap_get_ip());
    ESP_LOGI(TAG,
             "MJPEG stream ready at http://%s:%d/stream",
             wifi_ap_get_ip(),
             STREAM_SERVER_PORT);
    return ESP_OK;
}

void http_capture_stop(void)
{
    stream_set_stop_requested(true);

    stop_server(&s_stream_server, "MJPEG stream server");
    stop_server(&s_control_server, "HTTP server");
}

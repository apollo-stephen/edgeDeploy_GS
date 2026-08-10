#include "http_capture.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "CAMERA.h"
#include "dashboard_page.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "health.h"
#include "inference.h"
#include "wifi_ap.h"

#define CAPTURE_TIMEOUT_MS 2000
#define STREAM_SERVER_PORT 81
#define STREAM_TARGET_FPS 15
#define STREAM_FRAME_PERIOD_MS 67
#define STREAM_RETRY_DELAY_MS 20
#define STREAM_MAX_CAPTURE_FAILURES 3
#define HEALTH_CONTROL_BODY_MAX 64
#define PART_BOUNDARY "123456789000000000000987654321"

static const char *TAG = "http_capture";

#define HTTP_STATUS_CONFLICT "409 Conflict"
#define HTTP_STATUS_SERVICE_UNAVAILABLE "503 Service Unavailable"
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
static uint8_t *s_inference_response_buffer;

static bool append_format(char *buffer,
                          size_t capacity,
                          size_t *used,
                          const char *format,
                          ...)
{
    if (buffer == NULL || used == NULL || format == NULL || *used >= capacity) {
        return false;
    }

    va_list args;
    va_start(args, format);
    const int length = vsnprintf(buffer + *used,
                                 capacity - *used,
                                 format,
                                 args);
    va_end(args);
    if (length < 0 || (size_t)length >= capacity - *used) {
        return false;
    }
    *used += (size_t)length;
    return true;
}

static bool append_json_string(char *buffer,
                               size_t capacity,
                               size_t *used,
                               const char *value)
{
    if (value == NULL || !append_format(buffer, capacity, used, "\"")) {
        return false;
    }

    for (const unsigned char *cursor = (const unsigned char *)value;
         *cursor != '\0';
         ++cursor) {
        const char *escape = NULL;
        switch (*cursor) {
            case '"': escape = "\\\""; break;
            case '\\': escape = "\\\\"; break;
            case '\b': escape = "\\b"; break;
            case '\f': escape = "\\f"; break;
            case '\n': escape = "\\n"; break;
            case '\r': escape = "\\r"; break;
            case '\t': escape = "\\t"; break;
            default: break;
        }
        if (escape != NULL) {
            if (!append_format(buffer, capacity, used, "%s", escape)) {
                return false;
            }
        }
        else if (*cursor < 0x20U) {
            if (!append_format(buffer,
                               capacity,
                               used,
                               "\\u%04x",
                               (unsigned int)*cursor)) {
                return false;
            }
        }
        else if (!append_format(buffer,
                                capacity,
                                used,
                                "%c",
                                (int)*cursor)) {
            return false;
        }
    }
    return append_format(buffer, capacity, used, "\"");
}

static esp_err_t index_get_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request,
                           http_capture_dashboard_html(),
                           HTTPD_RESP_USE_STRLEN);
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

static esp_err_t send_health_monitor_state(httpd_req_t *request,
                                           bool enabled,
                                           bool ready,
                                           const char *state_name)
{
    char response[96];
    const int length = snprintf(response,
                                sizeof(response),
                                "{\"enabled\":%s,\"ready\":%s,"
                                "\"state\":\"%s\"}",
                                enabled ? "true" : "false",
                                ready ? "true" : "false",
                                state_name);
    if (length < 0 || length >= (int)sizeof(response)) {
        return httpd_resp_send_err(request,
                                   HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Health state response overflow");
    }
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, response, length);
}

static esp_err_t send_health_control_bad_request(httpd_req_t *request,
                                                 const char *message)
{
    httpd_resp_set_status(request, HTTPD_400);
    httpd_resp_set_type(request, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(request, message);
}

static const char *skip_json_space(const char *cursor)
{
    while (*cursor == ' ' || *cursor == '\t' ||
           *cursor == '\r' || *cursor == '\n') {
        ++cursor;
    }
    return cursor;
}

static bool parse_health_control_body(const char *body, bool *enabled)
{
    if (body == NULL || enabled == NULL) {
        return false;
    }
    const char *cursor = skip_json_space(body);
    if (*cursor++ != '{') {
        return false;
    }
    cursor = skip_json_space(cursor);
    static const char key[] = "\"enabled\"";
    if (strncmp(cursor, key, sizeof(key) - 1U) != 0) {
        return false;
    }
    cursor += sizeof(key) - 1U;
    cursor = skip_json_space(cursor);
    if (*cursor++ != ':') {
        return false;
    }
    cursor = skip_json_space(cursor);
    if (strncmp(cursor, "true", 4U) == 0) {
        *enabled = true;
        cursor += 4U;
    }
    else if (strncmp(cursor, "false", 5U) == 0) {
        *enabled = false;
        cursor += 5U;
    }
    else {
        return false;
    }
    cursor = skip_json_space(cursor);
    if (*cursor++ != '}') {
        return false;
    }
    return *skip_json_space(cursor) == '\0';
}

static esp_err_t read_health_control_body(httpd_req_t *request,
                                          char *body,
                                          size_t capacity)
{
    if (request->content_len == 0U ||
        request->content_len >= capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t used = 0U;
    while (used < request->content_len) {
        const int received = httpd_req_recv(request,
                                            body + used,
                                            request->content_len - used);
        if (received <= 0) {
            return ESP_FAIL;
        }
        used += (size_t)received;
    }
    body[used] = '\0';
    return ESP_OK;
}

static esp_err_t health_get_handler(httpd_req_t *request)
{
    health_monitor_status_t monitor = {0};
    if (health_get_monitor_status(&monitor) != ESP_OK) {
        return httpd_resp_send_err(request,
                                   HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Health monitor status unavailable");
    }
    if (!monitor.enabled) {
        return send_health_monitor_state(request, false, false, "off");
    }
    if (health_refresh_lease() != ESP_OK) {
        return httpd_resp_send_err(request,
                                   HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Health monitor lease unavailable");
    }
    if (!monitor.ready) {
        return send_health_monitor_state(request, true, false, "starting");
    }

    health_snapshot_t snapshot = {0};
    const esp_err_t snapshot_err = health_get_snapshot(&snapshot);

    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    if (snapshot_err != ESP_OK || !snapshot.ready) {
        return httpd_resp_send_err(request,
                                   HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Health snapshot unavailable");
    }

    const char *state_name = health_state_name(snapshot.state);
    if (state_name == NULL) {
        return httpd_resp_send_err(request,
                                   HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Invalid health state");
    }

    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    const uint64_t sample_age_ms = now_us >= snapshot.sampled_us
                                       ? (now_us - snapshot.sampled_us) / 1000U
                                       : 0U;
    char response[1536];
    const int length = snprintf(
        response,
        sizeof(response),
        "{\"enabled\":true,\"ready\":true,\"sequence\":%" PRIu32
        ",\"state\":\"%s\",\"reason_flags\":%" PRIu32
        ",\"sample_age_ms\":%" PRIu64
        ",\"uptime_ms\":%" PRIu64
        ",\"inference_age_ms\":%" PRIu64
        ",\"inference\":{\"attempt_running\":%s"
        ",\"attempt_count\":%" PRIu32
        ",\"success_count\":%" PRIu32
        ",\"failure_count\":%" PRIu32
        ",\"consecutive_failure_count\":%" PRIu32
        ",\"last_error\":%d"
        ",\"last_attempt_started_ms\":%" PRIu64
        ",\"last_attempt_finished_ms\":%" PRIu64
        ",\"last_success_ms\":%" PRIu64
        ",\"last_duration_ms\":%" PRIu64
        ",\"max_duration_ms\":%" PRIu64
        ",\"stack_high_water_mark_bytes\":%" PRIu32 "}"
        ",\"health_stack_high_water_mark_bytes\":%" PRIu32
        ",\"memory\":{\"internal\":{\"free_bytes\":%zu"
        ",\"minimum_free_bytes\":%zu"
        ",\"largest_free_block_bytes\":%zu}"
        ",\"psram\":{\"free_bytes\":%zu"
        ",\"minimum_free_bytes\":%zu"
        ",\"largest_free_block_bytes\":%zu}}}",
        snapshot.sequence,
        state_name,
        snapshot.reason_flags,
        sample_age_ms,
        snapshot.uptime_us / 1000U,
        snapshot.inference_age_us / 1000U,
        snapshot.inference.attempt_running ? "true" : "false",
        snapshot.inference.attempt_count,
        snapshot.inference.success_count,
        snapshot.inference.failure_count,
        snapshot.inference.consecutive_failure_count,
        snapshot.inference.last_error,
        snapshot.inference.last_attempt_started_us / 1000U,
        snapshot.inference.last_attempt_finished_us / 1000U,
        snapshot.inference.last_success_us / 1000U,
        snapshot.inference.last_duration_us / 1000U,
        snapshot.inference.max_duration_us / 1000U,
        snapshot.inference.stack_high_water_mark_bytes,
        snapshot.health_stack_high_water_mark_bytes,
        snapshot.internal_free_bytes,
        snapshot.internal_minimum_free_bytes,
        snapshot.internal_largest_free_block_bytes,
        snapshot.psram_free_bytes,
        snapshot.psram_minimum_free_bytes,
        snapshot.psram_largest_free_block_bytes);

    if (length < 0 || length >= (int)sizeof(response)) {
        return httpd_resp_send_err(request,
                                   HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Health response overflow");
    }
    return httpd_resp_send(request, response, length);
}

static esp_err_t health_control_post_handler(httpd_req_t *request)
{
    char body[HEALTH_CONTROL_BODY_MAX];
    bool enabled = false;
    if (read_health_control_body(request, body, sizeof(body)) != ESP_OK ||
        !parse_health_control_body(body, &enabled)) {
        return send_health_control_bad_request(request,
                                               "Invalid health control body");
    }
    if (health_set_enabled(enabled) != ESP_OK) {
        return httpd_resp_send_err(request,
                                   HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Health control failed");
    }
    return health_get_handler(request);
}

static esp_err_t inference_metadata_get_handler(httpd_req_t *request)
{
    inference_snapshot_metadata_t metadata = {0};
    const esp_err_t metadata_err =
        inference_get_latest_metadata(&metadata);

    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    if (metadata_err == ESP_ERR_NOT_FOUND) {
        return httpd_resp_sendstr(request, "{\"ready\":false}");
    }
    if (metadata_err != ESP_OK || !metadata.ready ||
        metadata.label_count > INFERENCE_MAX_LABELS) {
        return httpd_resp_send_err(request,
                                   HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Inference metadata unavailable");
    }

    const uint64_t now_ms = (uint64_t)esp_timer_get_time() / 1000U;
    const uint64_t age_ms = now_ms >= metadata.published_ms
                                ? now_ms - metadata.published_ms
                                : 0U;
    char response[1536] = {0};
    size_t used = 0;
    bool valid = append_format(response,
                               sizeof(response),
                               &used,
                               "{\"ready\":true,\"sequence\":%" PRIu32
                               ",\"prediction\":",
                               metadata.sequence);
    valid = valid && append_json_string(response,
                                        sizeof(response),
                                        &used,
                                        metadata.prediction);
    valid = valid && append_format(
        response,
        sizeof(response),
        &used,
        ",\"confidence\":%.5f,\"published_ms\":%" PRIu64
        ",\"age_ms\":%" PRIu64 ",\"jpeg_bytes\":%u,"
        "\"timing\":{\"dsp_ms\":%d,\"classification_ms\":%d,"
        "\"anomaly_ms\":%d},\"scores\":[",
        (double)metadata.confidence,
        metadata.published_ms,
        age_ms,
        (unsigned int)metadata.jpeg_bytes,
        metadata.timing.dsp_ms,
        metadata.timing.classification_ms,
        metadata.timing.anomaly_ms);

    for (size_t index = 0; valid && index < metadata.label_count; ++index) {
        valid = append_format(response,
                              sizeof(response),
                              &used,
                              "%s{\"label\":",
                              index == 0 ? "" : ",");
        valid = valid && append_json_string(response,
                                            sizeof(response),
                                            &used,
                                            metadata.scores[index].label);
        valid = valid && append_format(response,
                                       sizeof(response),
                                       &used,
                                       ",\"value\":%.5f}",
                                       (double)metadata.scores[index].value);
    }
    valid = valid && append_format(response, sizeof(response), &used, "]}");
    if (!valid) {
        return httpd_resp_send_err(request,
                                   HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Inference response overflow");
    }
    return httpd_resp_send(request, response, (long)used);
}

static esp_err_t send_inference_image_error(httpd_req_t *request,
                                            const char *status,
                                            const char *message)
{
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(request, message);
}

static bool parse_inference_sequence(httpd_req_t *request,
                                     uint32_t *sequence)
{
    static const char prefix[] = "sequence=";
    char query[48];
    const size_t query_length = httpd_req_get_url_query_len(request);
    if (sequence == NULL || query_length == 0U ||
        query_length >= sizeof(query) ||
        httpd_req_get_url_query_str(request,
                                    query,
                                    sizeof(query)) != ESP_OK ||
        strncmp(query, prefix, sizeof(prefix) - 1U) != 0) {
        return false;
    }

    const char *value = query + sizeof(prefix) - 1U;
    if (*value == '\0') {
        return false;
    }
    for (const char *cursor = value; *cursor != '\0'; ++cursor) {
        if (*cursor < '0' || *cursor > '9') {
            return false;
        }
    }

    errno = 0;
    char *end = NULL;
    const unsigned long parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed > UINT32_MAX) {
        return false;
    }
    *sequence = (uint32_t)parsed;
    return true;
}

static esp_err_t inference_image_get_handler(httpd_req_t *request)
{
    uint32_t sequence = 0;
    if (!parse_inference_sequence(request, &sequence)) {
        return send_inference_image_error(request,
                                          HTTPD_400,
                                          "Invalid inference sequence");
    }

    size_t jpeg_bytes = 0;
    const esp_err_t copy_err = inference_copy_latest_jpeg(
        sequence,
        s_inference_response_buffer,
        INFERENCE_MAX_JPEG_BYTES,
        &jpeg_bytes);
    if (copy_err == ESP_ERR_NOT_FOUND) {
        return send_inference_image_error(request,
                                          HTTP_STATUS_SERVICE_UNAVAILABLE,
                                          "Inference snapshot not ready");
    }
    if (copy_err == ESP_ERR_INVALID_STATE) {
        return send_inference_image_error(request,
                                          HTTP_STATUS_CONFLICT,
                                          "Inference sequence is stale");
    }
    if (copy_err != ESP_OK) {
        return send_inference_image_error(request,
                                          HTTPD_500,
                                          "Inference snapshot unavailable");
    }

    char sequence_header[12];
    snprintf(sequence_header, sizeof(sequence_header), "%" PRIu32, sequence);
    httpd_resp_set_type(request, "image/jpeg");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Inference-Sequence", sequence_header);
    return httpd_resp_send(request,
                           (const char *)s_inference_response_buffer,
                           (long)jpeg_bytes);
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
    const httpd_uri_t health_uri = {
        .uri = "/api/health",
        .method = HTTP_GET,
        .handler = health_get_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t health_control_uri = {
        .uri = "/api/health/control",
        .method = HTTP_POST,
        .handler = health_control_post_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t inference_uri = {
        .uri = "/api/inference",
        .method = HTTP_GET,
        .handler = inference_metadata_get_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t inference_image_uri = {
        .uri = "/api/inference/image",
        .method = HTTP_GET,
        .handler = inference_image_get_handler,
        .user_ctx = NULL,
    };

    esp_err_t err = httpd_register_uri_handler(server, &index_uri);
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(server, &capture_uri);
    }
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(server, &status_uri);
    }
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(server, &health_uri);
    }
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(server, &health_control_uri);
    }
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(server, &inference_uri);
    }
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(server, &inference_image_uri);
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

static void release_inference_buffer_if_stopped(void)
{
    if (s_control_server == NULL && s_stream_server == NULL &&
        s_inference_response_buffer != NULL) {
        heap_caps_free(s_inference_response_buffer);
        s_inference_response_buffer = NULL;
    }
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

    if (s_inference_response_buffer == NULL) {
        s_inference_response_buffer = heap_caps_malloc(
            INFERENCE_MAX_JPEG_BYTES,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_inference_response_buffer == NULL) {
            ESP_LOGE(TAG, "Failed to allocate inference HTTP JPEG buffer");
            return ESP_ERR_NO_MEM;
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
        release_inference_buffer_if_stopped();
        return err;
    }

    err = register_control_handlers(s_control_server);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "HTTP handler registration failed: %s",
                 esp_err_to_name(err));
        stop_server(&s_control_server, "HTTP server");
        release_inference_buffer_if_stopped();
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
        release_inference_buffer_if_stopped();
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
    release_inference_buffer_if_stopped();
}

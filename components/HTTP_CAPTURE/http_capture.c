#include "http_capture.h"

#include <inttypes.h>
#include <stdio.h>

#include "CAMERA.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "wifi_ap.h"

#define CAPTURE_TIMEOUT_MS 2000

static const char *TAG = "http_capture";
static httpd_handle_t s_server;
static uint32_t s_capture_count;
static uint32_t s_capture_failures;
static size_t s_last_capture_size;

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
    "<p>Native 128x128 JPEG preview</p>"
    "<img id=\"preview\" alt=\"Camera capture\">"
    "<p id=\"statusText\">Waiting for the first capture...</p>"
    "<div class=\"controls\">"
    "<button id=\"captureButton\" type=\"button\">Capture now</button>"
    "<button id=\"autoButton\" class=\"secondary\" type=\"button\">"
    "Pause auto refresh</button></div>"
    "<script>"
    "const preview=document.getElementById('preview');"
    "const statusText=document.getElementById('statusText');"
    "const captureButton=document.getElementById('captureButton');"
    "const autoButton=document.getElementById('autoButton');"
    "let inFlight=false;"
    "let pendingManual=false;"
    "let autoRefresh=true;"
    "async function capture(manual=false){"
    "if(inFlight){if(manual)pendingManual=true;return;}"
    "inFlight=true;"
    "if(manual||!preview.src)statusText.textContent='Capturing...';"
    "try{"
    "const response=await fetch(`/capture?t=${Date.now()}`,{cache:'no-store'});"
    "if(!response.ok)throw new Error(`HTTP ${response.status}`);"
    "const blob=await response.blob();"
    "const oldUrl=preview.src;"
    "preview.src=URL.createObjectURL(blob);"
    "if(oldUrl.startsWith('blob:'))URL.revokeObjectURL(oldUrl);"
    "const width=response.headers.get('X-Frame-Width')||'?';"
    "const height=response.headers.get('X-Frame-Height')||'?';"
    "statusText.textContent=`${width}x${height}, ${blob.size} bytes`;"
    "}catch(error){statusText.textContent=`Capture failed: ${error.message}`;}"
    "finally{"
    "inFlight=false;"
    "if(pendingManual){pendingManual=false;capture(true);}"
    "}"
    "}"
    "captureButton.addEventListener('click',()=>capture(true));"
    "autoButton.addEventListener('click',()=>{"
    "autoRefresh=!autoRefresh;"
    "autoButton.textContent=autoRefresh?'Pause auto refresh':'Resume auto refresh';"
    "if(autoRefresh)capture();"
    "});"
    "setInterval(()=>{if(autoRefresh)capture();},200);"
    "capture();"
    "</script></main></body></html>";

static esp_err_t index_get_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_get_handler(httpd_req_t *request)
{
    char response[384];
    const size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const int length = snprintf(
        response,
        sizeof(response),
        "{\"camera_ready\":%s,\"frame_size\":\"%s\","
        "\"capture_count\":%" PRIu32 ",\"capture_failures\":%" PRIu32 ","
        "\"last_capture_bytes\":%u,\"free_heap_bytes\":%u,"
        "\"free_psram_bytes\":%u}",
        camera_is_ready() ? "true" : "false",
        camera_frame_size_name(),
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

    if (frame->format != PIXFORMAT_JPEG ||
        frame->buf == NULL ||
        frame->len == 0 ||
        frame->width != CAMERA_FRAME_WIDTH ||
        frame->height != CAMERA_FRAME_HEIGHT) {
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

static esp_err_t register_handlers(httpd_handle_t server)
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

esp_err_t http_capture_start(void)
{
    if (s_server != NULL) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_uri_handlers = 8;
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "HTTP server startup failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    err = register_handlers(s_server);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "HTTP handler registration failed: %s",
                 esp_err_to_name(err));
        httpd_stop(s_server);
        s_server = NULL;
        return err;
    }

    ESP_LOGI(TAG,
             "Capture page ready at http://%s/",
             wifi_ap_get_ip());
    return ESP_OK;
}

void http_capture_stop(void)
{
    if (s_server == NULL) {
        return;
    }

    const esp_err_t err = httpd_stop(s_server);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "HTTP server stop failed: %s",
                 esp_err_to_name(err));
    }
    s_server = NULL;
}

# HTTP Image Transfer Design

## Goal

Add the smallest independently verifiable image path for the ESP32-S3:

1. initialize the OV5640;
2. capture a 128x128 JPEG on demand;
3. return that JPEG through an HTTP endpoint; and
4. show it on a web page with both automatic refresh and manual capture.

Model inference, image resizing, MJPEG streaming, dataset automation, and LCD
output are outside this feature.

## Starting Point

The branch starts from `main` commit `c2d34ef`, which contains the extracted
`WIFIAP` component. The user has already registered
`espressif/esp32-camera` version 2.1.7 and created an initial `CAMERA`
component skeleton. Those changes are part of this feature and must be
preserved.

The `esp_http_server` dependency is provided by ESP-IDF 5.5.4. No additional
Component Registry download is required for the project-owned `HTTP_CAPTURE`
component.

## Architecture

### CAMERA component

`components/CAMERA` owns camera initialization and frame-buffer access. Its
public interface exposes:

- idempotent camera initialization;
- serialized acquisition of one `camera_fb_t`;
- release of the acquired frame;
- readiness and configured-frame-size queries.

The OV5640 configuration uses the verified board wiring from the reference
EdgeDeploy project:

| Signal | GPIO |
| --- | ---: |
| PWDN | -1 |
| RESET | -1 |
| XCLK | 15 |
| SCCB SDA | 4 |
| SCCB SCL | 5 |
| D0-D7 | 11, 9, 8, 10, 12, 18, 17, 16 |
| VSYNC | 6 |
| HREF | 7 |
| PCLK | 13 |

The driver uses `PIXFORMAT_JPEG`, `FRAMESIZE_128X128`, JPEG quality 12, two
frame buffers, and latest-frame acquisition. A mutex ensures that an automatic
refresh and a manual request cannot own the camera frame buffer concurrently.
Every successful acquisition must have exactly one matching release.

Initialization logs the detected sensor PID. Captures are validated using the
actual `camera_fb_t` format, width, height, buffer pointer, and length rather
than assuming the configured size proves the returned frame is valid.

### HTTP_CAPTURE component

`components/HTTP_CAPTURE` owns the built-in ESP-IDF HTTP server and registers:

- `GET /` for the self-contained preview page;
- `GET /capture` for one freshly captured JPEG; and
- `GET /api/status` for camera state and capture counters.

`GET /capture` acquires a frame only for the duration of the response. A valid
response has `Content-Type: image/jpeg`, no-cache headers, and headers exposing
the actual frame width and height. The handler releases the frame on every
path after acquisition.

If capture is unavailable or busy, the endpoint returns HTTP 503. If the
driver returns a malformed or non-JPEG frame, it returns HTTP 500. Send
failures are logged and counted.

### Preview page

The page displays the current JPEG, status text, actual dimensions, and byte
count. It supports:

- automatic refresh once per second;
- pausing and resuming automatic refresh; and
- an immediate manual capture button.

Each request includes a timestamp query parameter so browser caching cannot
hide a new capture. The page prevents overlapping browser requests; a manual
capture may trigger immediately after the current request finishes.

### Application startup

`app_main` initializes subsystems in this order:

1. NVS;
2. CAMERA;
3. WIFIAP; and
4. HTTP_CAPTURE.

Failure at any step is logged with `esp_err_to_name()` and prevents later
dependent initialization. The ready log prints the page URL returned from the
actual SoftAP IP getter.

## Resource and Ownership Rules

- The HTTP server does not retain `camera_fb_t->buf` after a response.
- No queue contains camera-driver-owned pointers.
- This feature does not allocate a second full JPEG copy.
- Concurrent HTTP capture attempts are serialized by the CAMERA component.
- The server stack is explicitly sized for the JPEG handler and page response.

## Verification

Host-side tests verify component boundaries, dependencies, endpoint
registration, cache headers, frame validation, release behavior, page
controls, and application startup order.

The firmware must build for ESP32-S3 using ESP-IDF 5.5.4. Hardware acceptance
then requires:

1. serial output identifies the OV5640 and prints the preview URL;
2. the preview page loads after joining the SoftAP;
3. automatic refresh and manual capture both update the image;
4. `/capture` returns a JPEG whose actual dimensions are 128x128; and
5. repeated and overlapping requests do not crash, deadlock, or exhaust frame
   buffers.

Build success is not treated as proof of hardware acceptance. Flashing,
sensor detection, returned dimensions, and repeated-capture stability must be
reported separately.

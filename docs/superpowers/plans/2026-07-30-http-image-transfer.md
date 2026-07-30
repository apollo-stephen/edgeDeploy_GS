# HTTP Image Transfer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Initialize the OV5640, capture native 128x128 JPEG frames, and expose them through a web page supporting one-second automatic refresh and manual capture.

**Architecture:** A project-owned `CAMERA` component exclusively manages the `esp32-camera` driver and serializes frame ownership. A project-owned `HTTP_CAPTURE` component uses ESP-IDF's built-in `esp_http_server` to acquire one frame per `/capture` request, return it without copying, and release it immediately after the response.

**Tech Stack:** ESP-IDF 5.5.4, ESP32-S3, `espressif/esp32-camera` 2.1.7, `esp_http_server`, FreeRTOS mutexes, Python `unittest` source-structure tests.

## Global Constraints

- Use OV5640 wiring: PWDN -1, RESET -1, XCLK 15, SCCB SDA 4, SCCB SCL 5, D0-D7 11/9/8/10/12/18/17/16, VSYNC 6, HREF 7, PCLK 13.
- Configure `PIXFORMAT_JPEG`, `FRAMESIZE_128X128`, JPEG quality 12, two frame buffers, and `CAMERA_GRAB_LATEST`.
- Do not implement model inference, image resizing, MJPEG streaming, dataset automation, or LCD output.
- Do not retain `camera_fb_t->buf`, put it in a queue, or allocate a second full JPEG copy.
- Treat build, flash, sensor detection, actual returned dimensions, and stability as separate evidence levels.

---

### Task 1: OV5640 CAMERA Component

**Files:**
- Modify: `components/CAMERA/CAMERA.c`
- Modify: `components/CAMERA/include/CAMERA.h`
- Modify: `components/CAMERA/CMakeLists.txt`
- Modify: `main/idf_component.yml`
- Modify: `dependencies.lock`
- Create: `tests/test_camera_component.py`

**Interfaces:**
- Consumes: `esp_camera_init(const camera_config_t *)`, `esp_camera_fb_get(void)`, `esp_camera_fb_return(camera_fb_t *)`.
- Produces: `esp_err_t camera_init(void)`, `camera_fb_t *camera_capture_frame(uint32_t timeout_ms)`, `void camera_release_frame(camera_fb_t *frame)`, `bool camera_is_ready(void)`, and `const char *camera_frame_size_name(void)`.

- [ ] **Step 1: Write the failing CAMERA boundary tests**

Create `tests/test_camera_component.py` with `unittest` assertions that:

```python
header = read("components/CAMERA/include/CAMERA.h")
source = read("components/CAMERA/CAMERA.c")
cmake = read("components/CAMERA/CMakeLists.txt")

self.assertIn("esp_err_t camera_init(void);", header)
self.assertIn("camera_fb_t *camera_capture_frame(uint32_t timeout_ms);", header)
self.assertIn("void camera_release_frame(camera_fb_t *frame);", header)
self.assertIn("#define CAMERA_FRAME_WIDTH  128", header)
self.assertIn("#define CAMERA_FRAME_HEIGHT 128", header)
self.assertIn(".pixel_format = PIXFORMAT_JPEG", source)
self.assertIn(".frame_size = FRAMESIZE_128X128", source)
self.assertIn(".fb_count = 2", source)
self.assertIn(".grab_mode = CAMERA_GRAB_LATEST", source)
self.assertIn("xSemaphoreTake", source)
self.assertIn("esp_camera_fb_get()", source)
self.assertIn("esp_camera_fb_return(frame)", source)
self.assertIn("REQUIRES esp32-camera", cmake)
```

Also assert every required GPIO macro has the exact value from Global
Constraints and that the stub declaration `void func(void);` is absent.

- [ ] **Step 2: Run the CAMERA test and verify RED**

Run:

```bash
python3 -m unittest tests/test_camera_component.py -v
```

Expected: failures for the missing public API, camera configuration, mutex,
and `esp32-camera` dependency.

- [ ] **Step 3: Implement the minimal CAMERA component**

Replace the stub header with the GPIO constants, frame constants, and public
signatures in the Interfaces block. In `CAMERA.c`:

```c
static SemaphoreHandle_t s_capture_mutex;
static bool s_camera_ready;

esp_err_t camera_init(void)
{
    if (s_camera_ready) {
        return ESP_OK;
    }
    s_capture_mutex = xSemaphoreCreateMutex();
    if (s_capture_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const camera_config_t config = {
        /* exact GPIO macros from CAMERA.h */
        .xclk_freq_hz = 24000000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = FRAMESIZE_128X128,
        .jpeg_quality = CAMERA_JPEG_QUALITY,
        .fb_count = 2,
        .fb_location = CAMERA_FB_IN_DRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
    };
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        vSemaphoreDelete(s_capture_mutex);
        s_capture_mutex = NULL;
        return err;
    }
    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor != NULL) {
        sensor->set_pixformat(sensor, PIXFORMAT_JPEG);
        sensor->set_framesize(sensor, FRAMESIZE_128X128);
        sensor->set_hmirror(sensor, 1);
        sensor->set_vflip(sensor, 1);
        ESP_LOGI(TAG, "Detected camera sensor PID: 0x%04x", sensor->id.PID);
    }
    s_camera_ready = true;
    return ESP_OK;
}
```

Implement `camera_capture_frame()` by taking the mutex with the requested
timeout and returning `esp_camera_fb_get()`. If acquisition fails, release the
mutex. Implement `camera_release_frame()` so a non-null frame is returned
before releasing the mutex. Return `"128x128"` from
`camera_frame_size_name()`.

Set the component registration to:

```cmake
idf_component_register(
    SRCS "CAMERA.c"
    INCLUDE_DIRS "include"
    REQUIRES esp32-camera
)
```

Preserve the registered `espressif/esp32-camera: ^2.1.7` manifest and resolved
lockfile entries already produced by ESP-IDF Component Manager.

- [ ] **Step 4: Run CAMERA and existing WiFiAP tests**

Run:

```bash
python3 -m unittest tests/test_camera_component.py tests/test_wifi_ap_component.py -v
```

Expected: all tests pass.

- [ ] **Step 5: Commit the CAMERA component**

```bash
git add components/CAMERA main/idf_component.yml dependencies.lock tests/test_camera_component.py
git commit -m "feat: add OV5640 camera capture component"
```

### Task 2: HTTP Capture Server and Preview Page

**Files:**
- Create: `components/HTTP_CAPTURE/include/http_capture.h`
- Create: `components/HTTP_CAPTURE/http_capture.c`
- Create: `components/HTTP_CAPTURE/CMakeLists.txt`
- Create: `tests/test_http_capture_component.py`

**Interfaces:**
- Consumes: `camera_capture_frame(uint32_t)`, `camera_release_frame(camera_fb_t *)`, `camera_is_ready(void)`, `camera_frame_size_name(void)`, and `wifi_ap_get_ip(void)`.
- Produces: `esp_err_t http_capture_start(void)` and `void http_capture_stop(void)`.

- [ ] **Step 1: Write failing HTTP component tests**

Create `tests/test_http_capture_component.py` with assertions that the header
contains the two public signatures and the source contains:

```python
self.assertIn('.uri = "/"', source)
self.assertIn('.uri = "/capture"', source)
self.assertIn('.uri = "/api/status"', source)
self.assertIn('httpd_resp_set_type(request, "image/jpeg")', source)
self.assertIn("camera_capture_frame(CAPTURE_TIMEOUT_MS)", source)
self.assertIn("camera_release_frame(frame)", source)
self.assertIn("frame->format != PIXFORMAT_JPEG", source)
self.assertIn('"503 Service Unavailable"', source)
self.assertIn('"Cache-Control"', source)
self.assertIn('"X-Frame-Width"', source)
self.assertIn('"X-Frame-Height"', source)
self.assertIn("setInterval", source)
self.assertIn("Capture now", source)
self.assertIn("Pause auto refresh", source)
self.assertIn("esp_http_server CAMERA WIFIAP", cmake)
```

Add a release-order assertion requiring `httpd_resp_send(...)` to appear
before `camera_release_frame(frame)` in the capture handler. Add assertions
that the page uses a timestamp query and an in-flight guard.

- [ ] **Step 2: Run the HTTP test and verify RED**

Run:

```bash
python3 -m unittest tests/test_http_capture_component.py -v
```

Expected: failure because `components/HTTP_CAPTURE` does not exist.

- [ ] **Step 3: Implement the HTTP server**

Declare the two public functions in `http_capture.h`. Register the component:

```cmake
idf_component_register(
    SRCS "http_capture.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server CAMERA WIFIAP esp_psram
)
```

Implement a self-contained HTML page with an `<img>` preview, status fields,
`Capture now`, and `Pause auto refresh` controls. JavaScript uses:

```javascript
let inFlight = false;
let autoRefresh = true;
async function capture() {
  if (inFlight) return;
  inFlight = true;
  try {
    const response = await fetch(`/capture?t=${Date.now()}`, {cache: "no-store"});
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const blob = await response.blob();
    const oldUrl = preview.src;
    preview.src = URL.createObjectURL(blob);
    if (oldUrl.startsWith("blob:")) URL.revokeObjectURL(oldUrl);
    statusText.textContent =
      `${response.headers.get("X-Frame-Width")}x${response.headers.get("X-Frame-Height")}, ${blob.size} bytes`;
  } finally {
    inFlight = false;
  }
}
setInterval(() => { if (autoRefresh) capture(); }, 1000);
```

The `/capture` handler:

1. calls `camera_capture_frame(CAPTURE_TIMEOUT_MS)`;
2. returns 503 when no frame is acquired;
3. checks JPEG format, non-null buffer, non-zero length, and actual 128x128
   dimensions;
4. sets no-cache, width, height, and inline-filename headers;
5. sends the frame directly with `httpd_resp_send`; and
6. calls `camera_release_frame(frame)` exactly once after the send attempt.

The `/api/status` JSON includes `camera_ready`, configured frame size,
successful capture count, failed capture count, last JPEG byte size, free heap,
and free PSRAM. Configure an 8192-byte HTTP task stack and enable LRU purge.

- [ ] **Step 4: Run all host-side tests**

Run:

```bash
python3 -m unittest discover -s tests -v
```

Expected: CAMERA, HTTP_CAPTURE, and WIFIAP tests all pass.

- [ ] **Step 5: Commit the HTTP component**

```bash
git add components/HTTP_CAPTURE tests/test_http_capture_component.py
git commit -m "feat: serve camera JPEG captures over HTTP"
```

### Task 3: Application Integration and User Instructions

**Files:**
- Modify: `main/main.c`
- Modify: `main/CMakeLists.txt`
- Modify: `README.md`
- Create: `tests/test_http_image_transfer_integration.py`

**Interfaces:**
- Consumes: `camera_init(void)`, `wifi_ap_init(void)`, `http_capture_start(void)`, and `wifi_ap_get_ip(void)`.
- Produces: a complete NVS-to-camera-to-SoftAP-to-HTTP startup path and documented browser workflow.

- [ ] **Step 1: Write failing integration tests**

Create `tests/test_http_image_transfer_integration.py` with assertions:

```python
self.assertIn('#include "CAMERA.h"', main)
self.assertIn('#include "http_capture.h"', main)
self.assertLess(main.index("camera_init()"), main.index("wifi_ap_init()"))
self.assertLess(main.index("wifi_ap_init()"), main.index("http_capture_start()"))
self.assertIn("CAMERA", cmake)
self.assertIn("HTTP_CAPTURE", cmake)
self.assertIn("http://192.168.4.1/", readme)
self.assertIn("/capture", readme)
self.assertIn("128x128", readme)
```

Also assert that every initialization call has an `ESP_OK` check and that
camera or Wi-Fi failure prevents HTTP startup.

- [ ] **Step 2: Run the integration test and verify RED**

Run:

```bash
python3 -m unittest tests/test_http_image_transfer_integration.py -v
```

Expected: failures because the current application initializes only NVS and
WiFiAP.

- [ ] **Step 3: Integrate startup and rewrite focused README sections**

Update `main/main.c` to initialize NVS, CAMERA, WIFIAP, and HTTP_CAPTURE in that
order. Return immediately on each error and finish with:

```c
ESP_LOGI(TAG, "Image preview ready at http://%s/", wifi_ap_get_ip());
```

Add `CAMERA` and `HTTP_CAPTURE` to `main/CMakeLists.txt` private requirements.
Rewrite the README title and usage so it documents:

- ESP-IDF 5.5.4 and ESP32-S3 target setup;
- build, flash, and monitor commands;
- joining the configured SoftAP;
- opening `http://192.168.4.1/`;
- using automatic and manual capture;
- directly testing `/capture`; and
- checking that returned headers and image dimensions are 128x128.

- [ ] **Step 4: Run all host-side tests**

Run:

```bash
python3 -m unittest discover -s tests -v
```

Expected: all tests pass.

- [ ] **Step 5: Commit integration**

```bash
git add main/main.c main/CMakeLists.txt README.md tests/test_http_image_transfer_integration.py
git commit -m "feat: integrate HTTP image preview workflow"
```

### Task 4: Build and Evidence Audit

**Files:**
- Modify only if required by a demonstrated build error: `components/CAMERA/**`, `components/HTTP_CAPTURE/**`, `main/**`

**Interfaces:**
- Consumes: complete firmware from Tasks 1-3.
- Produces: host-test output, ESP-IDF build result, binary-size output, and an explicit list of remaining hardware-only checks.

- [ ] **Step 1: Verify the active ESP-IDF environment**

Run:

```bash
idf.py --version
python3 --version
```

Expected: ESP-IDF v5.5.4. If `idf.py` is missing or reports a different
environment, report the environment mismatch rather than changing the user's
installation.

- [ ] **Step 2: Run the complete host test suite**

Run:

```bash
python3 -m unittest discover -s tests -v
```

Expected: all tests pass with zero failures.

- [ ] **Step 3: Build from current source**

Run:

```bash
idf.py set-target esp32s3
idf.py build
```

Expected: successful link of the ESP32-S3 application. If the build exposes a
source defect, add a failing host regression test where practical, make the
smallest fix, rerun all tests, and rebuild.

- [ ] **Step 4: Record size evidence**

Run:

```bash
idf.py size
```

Expected: command succeeds and reports current firmware Flash and RAM usage.

- [ ] **Step 5: Audit the final diff**

Run:

```bash
git status --short
git diff --check
git log --oneline main..HEAD
```

Expected: no unintended files, no whitespace errors, and distinct commits for
design, CAMERA, HTTP server, and integration.

- [ ] **Step 6: Report evidence without overstating hardware status**

Report host tests, exact ESP-IDF version, build status, and size output as
verified. Mark flashing, OV5640 detection, browser display, actual 128x128
response, and repeated-request stability as unverified until serial and board
evidence are collected.

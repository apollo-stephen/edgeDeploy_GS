# OV5640 15 FPS MJPEG Low-Heat Preview Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the 5 FPS snapshot polling preview with a single-client 15 FPS MJPEG stream while reducing OV5640 XCLK from 24 MHz to 16 MHz and preserving `/capture`.

**Architecture:** Keep the existing CAMERA ownership API and double-buffer latest-frame mode. Run page, status, and still capture on HTTP port 80, and run the long-lived MJPEG handler on a second ESP-IDF HTTP server on port 81 with a distinct control port. Protect cross-server stream metrics with a short FreeRTOS critical section.

**Tech Stack:** ESP-IDF 5.5.4, esp32-camera, esp_http_server, FreeRTOS, C11 host behavior tests, Python unittest.

## Global Constraints

- Preserve native 128×128 JPEG, JPEG quality 12, double buffering, `CAMERA_GRAB_LATEST`, and the 8192-byte custom JPEG frame buffer.
- Set camera XCLK to exactly 16,000,000 Hz.
- Limit MJPEG output to one client and a 67 ms minimum frame period.
- Keep `/capture` and all existing status fields.
- Do not add storage, authentication, OTA, or a custom camera producer task.
- Do not log successful frames individually.
- Separate coded, built, flashed, and measured evidence in the handoff.

---

### Task 1: Reduce OV5640 XCLK

**Files:**
- Modify: `components/CAMERA/include/CAMERA.h`
- Modify: `components/CAMERA/CAMERA.c`
- Modify: `tests/host/camera_component_test.c`

**Interfaces:**
- Produces: `CAMERA_XCLK_FREQ_HZ` as the single source of truth for camera configuration and status reporting.

- [ ] **Step 1: Write the failing clock test**

Change the host assertion to:

```c
assert(CAMERA_XCLK_FREQ_HZ == 16000000U);
assert(s_config.xclk_freq_hz == CAMERA_XCLK_FREQ_HZ);
```

- [ ] **Step 2: Run the focused test and verify failure**

Run:

```bash
python3 -m unittest tests.test_camera_component -v
```

Expected: FAIL because `CAMERA_XCLK_FREQ_HZ` is missing or the configured value is still 24 MHz.

- [ ] **Step 3: Add and use the clock constant**

Add to `CAMERA.h`:

```c
#define CAMERA_XCLK_FREQ_HZ 16000000U
```

Set `.xclk_freq_hz = CAMERA_XCLK_FREQ_HZ` in `CAMERA.c`.

- [ ] **Step 4: Run the focused test**

Run:

```bash
python3 -m unittest tests.test_camera_component -v
```

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add components/CAMERA/include/CAMERA.h components/CAMERA/CAMERA.c tests/host/camera_component_test.c
git commit -m "fix: reduce OV5640 capture clock"
```

### Task 2: Add MJPEG stream protocol and ownership behavior

**Files:**
- Modify: `components/HTTP_CAPTURE/http_capture.c`
- Modify: `tests/host/include/esp_http_server.h`
- Create: `tests/host/include/esp_timer.h`
- Modify: `tests/host/include/freertos/FreeRTOS.h`
- Create: `tests/host/include/freertos/task.h`
- Modify: `tests/host/http_capture_component_test.c`

**Interfaces:**
- Consumes: `camera_capture_frame(uint32_t)` and `camera_release_frame(camera_fb_t *)`.
- Produces: `GET /stream` with `multipart/x-mixed-replace`, a one-client guard, 67 ms pacing, and stream metrics.

- [ ] **Step 1: Extend host fakes and write failing stream tests**

Extend `httpd_config_t` with `server_port` and `ctrl_port`, and add:

```c
esp_err_t httpd_resp_send_chunk(httpd_req_t *request,
                                const char *chunk,
                                long length);
```

Add fake `esp_timer_get_time()` and `vTaskDelay()`. Update the HTTP behavior test to expect two servers, `/stream` on the second server, this content type:

```text
multipart/x-mixed-replace;boundary=123456789000000000000987654321
```

and this sequence:

```text
boundary -> JPEG headers -> JPEG bytes -> frame release
```

Force the next chunk call to fail after one complete frame so the infinite handler exits. Assert that the frame is returned and a simultaneous second stream request receives `503 Service Unavailable`.

- [ ] **Step 2: Run the focused test and verify failure**

Run:

```bash
python3 -m unittest tests.test_http_capture_component -v
```

Expected: FAIL because no second server or `/stream` handler exists.

- [ ] **Step 3: Implement stream state and frame sending**

In `http_capture.c`, add:

```c
#define STREAM_SERVER_PORT 81
#define STREAM_FRAME_PERIOD_MS 67
#define STREAM_MAX_CAPTURE_FAILURES 3
#define PART_BOUNDARY "123456789000000000000987654321"
```

Add a short critical-section-protected state containing:

```c
bool stream_client_connected;
uint32_t stream_frame_count;
uint32_t stream_failures;
double stream_fps;
```

Implement a stream handler that atomically claims the one-client slot, sends native JPEG chunks, releases every acquired frame on every path, delays only the remainder of the 67 ms period, and clears the client flag before returning.

- [ ] **Step 4: Start and stop the second HTTP server**

Use a second `httpd_handle_t`. Copy `HTTPD_DEFAULT_CONFIG()`, then set:

```c
stream_config.server_port = 81;
stream_config.ctrl_port += 1;
stream_config.stack_size = 8192;
stream_config.max_uri_handlers = 1;
stream_config.lru_purge_enable = true;
```

If stream startup or handler registration fails, stop every server already started and return the original error. Stop the stream server before the control server.

- [ ] **Step 5: Run the focused test**

Run:

```bash
python3 -m unittest tests.test_http_capture_component -v
```

Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add components/HTTP_CAPTURE/http_capture.c tests/host/include/esp_http_server.h tests/host/include/esp_timer.h tests/host/include/freertos/FreeRTOS.h tests/host/include/freertos/task.h tests/host/http_capture_component_test.c
git commit -m "feat: stream camera frames over MJPEG"
```

### Task 3: Switch the page to MJPEG and expose metrics

**Files:**
- Modify: `components/HTTP_CAPTURE/http_capture.c`
- Modify: `tests/host/http_capture_component_test.c`

**Interfaces:**
- Consumes: `/stream` on port 81 and the stream state from Task 2.
- Produces: native-size MJPEG preview, pause/resume controls, manual still capture, and expanded `/api/status`.

- [ ] **Step 1: Write failing page and status tests**

Assert that the page contains:

```javascript
const streamUrl=`http://${location.hostname}:81/stream`;
```

and does not contain `setInterval`, `fetch(`/capture`, `response.blob()`, or `URL.createObjectURL`.

Assert that status contains:

```json
"camera_xclk_hz":16000000
"stream_target_fps":15
"stream_client_connected":false
"stream_frame_count":
"stream_failures":
"stream_fps":
```

- [ ] **Step 2: Run the focused test and verify failure**

Run:

```bash
python3 -m unittest tests.test_http_capture_component -v
```

Expected: FAIL because the page still polls snapshots and stream fields are absent.

- [ ] **Step 3: Implement the page**

Use the existing 128×128 `<img>`. On load and resume, assign a timestamped port-81 stream URL. On pause, remove `src`. Make the still-capture button open a timestamped `/capture` URL in a new tab so it does not replace or stop the stream.

- [ ] **Step 4: Add status snapshot fields**

Copy counters under the state critical section, then format the JSON outside it. Report `CAMERA_XCLK_FREQ_HZ`, target 15 FPS, connection state, total stream frames, failures, and the measured connection-average FPS.

- [ ] **Step 5: Run focused and full host tests**

Run:

```bash
python3 -m unittest tests.test_http_capture_component -v
python3 -m unittest discover -s tests -v
```

Expected: all tests PASS.

- [ ] **Step 6: Commit**

```bash
git add components/HTTP_CAPTURE/http_capture.c tests/host/http_capture_component_test.c
git commit -m "feat: show native MJPEG camera preview"
```

### Task 4: Build and hand off device validation

**Files:**
- Modify only if required by a verified build error: `components/HTTP_CAPTURE/CMakeLists.txt`

**Interfaces:**
- Produces: a fresh ESP-IDF firmware image in `build/edgeDeploy_GS.bin`.

- [ ] **Step 1: Run a clean verification pass**

Run:

```bash
python3 -m unittest discover -s tests -v
git diff --check
```

Expected: all tests PASS and `git diff --check` produces no output.

- [ ] **Step 2: Build with ESP-IDF 5.5.4**

Activate the known project environment and run:

```bash
idf.py build
```

Expected: build succeeds and reports application size and remaining factory partition space.

- [ ] **Step 3: Verify artifact and repository state**

Run:

```bash
ls -lh build/edgeDeploy_GS.bin build/edgeDeploy_GS.elf
git status --short --branch
```

Expected: fresh binary and ELF exist; the feature branch has no uncommitted implementation changes.

- [ ] **Step 4: Provide hardware-only acceptance commands**

After the user flashes, verify:

```bash
curl http://192.168.4.1/api/status
curl -s -o /tmp/edgedeploy.jpg http://192.168.4.1/capture
```

Expected on hardware: no `FB-OVF`, `stream_fps` between 14 and 16 under normal light, still capture remains valid during preview, and ten-minute operation has no reset or sustained failures.

# Inference Web Dashboard Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Show the existing live MJPEG preview beside the exact JPEG from the latest successful inference and render its version-matched dynamic classification results below.

**Architecture:** The inference component owns two fixed 8192-byte PSRAM JPEG buffers and publishes a versioned metadata record by swapping buffers under a FreeRTOS mutex. The HTTP component copies metadata and the expected-sequence JPEG through public inference APIs, serves JSON/JPEG endpoints, and hosts a responsive dashboard whose browser logic commits a new image-result pair only after sequence validation.

**Tech Stack:** ESP-IDF 5.5.4, ESP32-S3 dual-core FreeRTOS, Edge Impulse SDK 1.94.2, bundled ESP-NN, ESP-IDF HTTP server, C/C++17, browser JavaScript, Python `unittest` host harnesses.

## Global Constraints

- Preserve inference on CPU1, task priority `5`, stack size `8192`, capture timeout `250 ms`, and two-second period.
- Preserve ESP-NN, `EI_MAX_OVERFLOW_BUFFER_COUNT=256`, 16-byte aligned Edge Impulse allocations, and the five-second task-watchdog policy.
- Preserve the existing CAMERA frame-ownership API and release each camera frame before running the classifier.
- Store snapshot JPEG buffers and the HTTP response-copy buffer in PSRAM with `MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT`.
- Limit JPEG snapshots to `8192` bytes, model labels to `8`, and each label or prediction string to `31` characters plus the terminator.
- Publish only fully successful inference results; failures retain the last complete image-result pair.
- Add no ESP Component Registry dependency.
- Do not implement OTA in this change; document application OTA before bootloader OTA, with bootloader OTA at the lowest priority.
- Preserve the user's unrelated `.gitignore` modification without staging it.

---

### Task 1: Publish versioned inference snapshots

**Files:**
- Modify: `components/INFERENCE/include/inference.h`
- Modify: `components/INFERENCE/inference.cpp`
- Modify: `components/INFERENCE/CMakeLists.txt`
- Modify: `tests/host/include/esp_err.h`
- Modify: `tests/host/inference_component_test.cpp`
- Modify: `tests/test_inference_component.py`

**Interfaces:**
- Consumes: existing `camera_capture_frame()`, `camera_release_frame()`, `fmt2rgb888()`, `run_classifier()`, `heap_caps_malloc()`, and FreeRTOS mutex APIs.
- Produces: `inference_snapshot_metadata_t`, `inference_get_latest_metadata()`, and `inference_copy_latest_jpeg()` without exposing Edge Impulse SDK types.

- [ ] **Step 1: Define the public snapshot contract in the failing host test**

Extend `tests/host/inference_component_test.cpp` so successful fake inference
uses JPEG bytes `{0xff, 0xd8, 0x11, 0x22, 0xff, 0xd9}`, a fake timer of
`52825000` microseconds, and calls these exact interfaces after
`inference_run_once()`:

```cpp
inference_snapshot_metadata_t metadata = {};
assert(inference_get_latest_metadata(&metadata) == ESP_OK);
assert(metadata.ready);
assert(metadata.sequence == 1);
assert(metadata.label_count == 3);
assert(strcmp(metadata.prediction, "recycleable") == 0);
assert(metadata.confidence == 0.90f);
assert(metadata.timing.dsp_ms == 11);
assert(metadata.timing.classification_ms == 22);
assert(metadata.timing.anomaly_ms == 0);
assert(metadata.published_ms == 52825);
assert(metadata.jpeg_bytes == sizeof(s_jpeg));
assert(strcmp(metadata.scores[0].label, "harmful") == 0);
assert(strcmp(metadata.scores[1].label, "recycleable") == 0);
assert(strcmp(metadata.scores[2].label, "wet") == 0);

uint8_t jpeg_copy[INFERENCE_MAX_JPEG_BYTES] = {};
size_t jpeg_bytes = 0;
assert(inference_copy_latest_jpeg(metadata.sequence,
                                  jpeg_copy,
                                  sizeof(jpeg_copy),
                                  &jpeg_bytes) == ESP_OK);
assert(jpeg_bytes == sizeof(s_jpeg));
assert(memcmp(jpeg_copy, s_jpeg, jpeg_bytes) == 0);
```

Before the first success assert both read APIs return `ESP_ERR_NOT_FOUND`.
After a second successful run assert sequence `2`. Assert sequence `1` then
returns `ESP_ERR_INVALID_STATE`, a five-byte destination returns
`ESP_ERR_INVALID_SIZE`, and null pointers return `ESP_ERR_INVALID_ARG`.

Add failure scenarios proving invalid frame, oversized JPEG, decode failure,
and classifier failure do not change metadata sequence or published JPEG after
one prior success. Extend allocation doubles to expect one 49,152-byte RGB
buffer and two 8192-byte JPEG buffers in PSRAM, and extend mutex doubles to
record create/take/give/delete calls and task-start rollback.

- [ ] **Step 2: Run the inference behavior test and verify RED**

Run:

```bash
python3 -m unittest tests.test_inference_component.InferenceComponentBehaviorTest.test_task_lifecycle_frame_ownership_and_classifier_bridge -v
```

Expected: compilation fails because the snapshot types, constants, and read
functions do not exist.

- [ ] **Step 3: Add the public C-compatible snapshot types**

Add these definitions to `components/INFERENCE/include/inference.h`:

```c
#define INFERENCE_MAX_JPEG_BYTES 8192U
#define INFERENCE_MAX_LABELS 8U
#define INFERENCE_LABEL_BYTES 32U

typedef struct {
    char label[INFERENCE_LABEL_BYTES];
    float value;
} inference_score_t;

typedef struct {
    int dsp_ms;
    int classification_ms;
    int anomaly_ms;
} inference_timing_t;

typedef struct {
    bool ready;
    uint32_t sequence;
    char prediction[INFERENCE_LABEL_BYTES];
    float confidence;
    size_t label_count;
    inference_score_t scores[INFERENCE_MAX_LABELS];
    inference_timing_t timing;
    uint64_t published_ms;
    size_t jpeg_bytes;
} inference_snapshot_metadata_t;

esp_err_t inference_get_latest_metadata(
    inference_snapshot_metadata_t *metadata);
esp_err_t inference_copy_latest_jpeg(uint32_t expected_sequence,
                                     uint8_t *destination,
                                     size_t capacity,
                                     size_t *jpeg_bytes);
```

Include `<stdbool.h>`, `<stddef.h>`, and `<stdint.h>`. Add
`ESP_ERR_INVALID_SIZE` and `ESP_ERR_NOT_FOUND` with their ESP-IDF values to the
host fake `esp_err.h`.

- [ ] **Step 4: Implement double-buffered publication and readers**

In `inference.cpp`, add compile-time validation:

```cpp
static_assert(EI_CLASSIFIER_LABEL_COUNT <= INFERENCE_MAX_LABELS,
              "Model label count exceeds inference snapshot capacity");
```

Create one mutex plus staging and published JPEG pointers. Allocate the mutex,
RGB buffer, and both JPEG buffers before task creation. On any startup failure,
free every successful allocation, delete the mutex, null all state, and leave
`s_started` false.

Before JPEG decode, reject `frame->len > INFERENCE_MAX_JPEG_BYTES`; otherwise
copy the camera JPEG into staging and release the frame after decode. Build a
local metadata record after classifier success. Reject a source label whose
length is at least `INFERENCE_LABEL_BYTES`. Apply `EI_CLASSIFIER_THRESHOLD` to
select `uncertain`, copy timings and scores, set publication time from
`esp_timer_get_time() / 1000`, then take the mutex with `portMAX_DELAY`, swap
the JPEG pointers, increment sequence while skipping zero, assign metadata,
and give the mutex.

Implement the two read APIs with argument checks and mutex-protected copies:

```cpp
// metadata not ready -> ESP_ERR_NOT_FOUND
// JPEG not ready -> ESP_ERR_NOT_FOUND
// stale sequence -> ESP_ERR_INVALID_STATE
// capacity too small -> ESP_ERR_INVALID_SIZE
```

Add `PRIV_REQUIRES esp_timer` to the inference component.

- [ ] **Step 5: Run the inference behavior test and verify GREEN**

Run the command from Step 2. Expected: all lifecycle, snapshot, threshold,
stale sequence, capacity, and failure-preservation scenarios pass.

- [ ] **Step 6: Commit the inference publication boundary**

Run `git diff --check`, stage only the files listed in Task 1, and commit:

```bash
git commit -m "feat: publish versioned inference snapshots"
```

---

### Task 2: Serve inference metadata and matching JPEG

**Files:**
- Modify: `components/HTTP_CAPTURE/http_capture.c`
- Modify: `components/HTTP_CAPTURE/CMakeLists.txt`
- Modify: `tests/host/include/esp_http_server.h`
- Modify: `tests/host/http_capture_component_test.c`
- Modify: `tests/test_http_capture_component.py`

**Interfaces:**
- Consumes: Task 1's `inference_get_latest_metadata()` and `inference_copy_latest_jpeg()`.
- Produces: `GET /api/inference` and `GET /api/inference/image?sequence=N` plus a lifecycle-owned HTTP JPEG copy buffer.

- [ ] **Step 1: Add failing endpoint and lifecycle scenarios**

Extend the HTTP host fake request with `const char *query_string`, and declare
and implement host doubles for:

```c
size_t httpd_req_get_url_query_len(httpd_req_t *request);
esp_err_t httpd_req_get_url_query_str(httpd_req_t *request,
                                      char *buffer,
                                      size_t buffer_length);
esp_err_t httpd_query_key_value(const char *query,
                                const char *key,
                                char *value,
                                size_t value_length);
```

Add fake response-status constants matching ESP-IDF:

```c
#define HTTPD_400 "400 Bad Request"
#define HTTPD_409 "409 Conflict"
#define HTTPD_503 "503 Service Unavailable"
```

Provide controlled inference doubles with a three-score snapshot. Assert the
control server registers `/api/inference` and `/api/inference/image`. Test:

```c
// Not ready metadata
assert(metadata_uri->handler(&request) == ESP_OK);
assert(strcmp(request.response_body, "{\"ready\":false}") == 0);

// Ready metadata contains versioned dynamic fields and escaped label text
assert(strstr(request.response_body, "\"sequence\":12") != NULL);
assert(strstr(request.response_body, "\"prediction\":\"recycleable\"") != NULL);
assert(strstr(request.response_body, "\"age_ms\":580") != NULL);
assert(strstr(request.response_body, "wet\\\"\\\\\\n") != NULL);

// Matching image
request.query_string = "sequence=12";
assert(image_uri->handler(&request) == ESP_OK);
assert(strcmp(request.response_type, "image/jpeg") == 0);
assert(request.response_length == fake_jpeg_size);
assert(memcmp(request.response_body, fake_jpeg, fake_jpeg_size) == 0);
assert(strcmp(find_header(&request, "X-Inference-Sequence"), "12") == 0);
```

Also assert missing/non-decimal/overflow sequences return 400, no snapshot
returns 503, stale sequence returns 409, and response-buffer allocation failure
prevents server startup. Verify the 8192-byte PSRAM buffer is freed only when
both server handles have stopped; if a stop fails, it remains allocated until
a later successful stop.

- [ ] **Step 2: Run the HTTP behavior test and verify RED**

Run:

```bash
python3 -m unittest tests.test_http_capture_component.HttpCaptureComponentBehaviorTest.test_routes_capture_ownership_and_preview_controls -v
```

Expected: failure because the new routes, query helpers, inference doubles, and
HTTP response buffer behavior are absent.

- [ ] **Step 3: Implement bounded JSON construction and JSON escaping**

In `http_capture.c`, add a 1536-byte local JSON response and two internal
helpers:

```c
static bool append_format(char *buffer,
                          size_t capacity,
                          size_t *used,
                          const char *format,
                          ...);
static bool append_json_string(char *buffer,
                               size_t capacity,
                               size_t *used,
                               const char *value);
```

`append_format` rejects negative or truncated `vsnprintf` results.
`append_json_string` emits quotes and escapes `"`, `\\`, backspace, form feed,
newline, carriage return, and tab; bytes below `0x20` use `\\u00XX`. Build the
ready JSON from caller-owned metadata, compute `age_ms` from the monotonic
`esp_timer_get_time()` without underflow, and return HTTP 500 on any overflow.

- [ ] **Step 4: Implement strict image sequence handling**

Parse the query into fixed buffers, require every character of `sequence` to be
ASCII decimal, use `strtoul` with `errno`, and reject values above
`UINT32_MAX`. Map inference errors exactly:

```c
ESP_ERR_NOT_FOUND     -> 503 Service Unavailable
ESP_ERR_INVALID_STATE -> 409 Conflict
other errors          -> 500 Internal Server Error
```

On success set `image/jpeg`, `Cache-Control: no-store`, and
`X-Inference-Sequence`, then send the copied bytes.

- [ ] **Step 5: Integrate response-buffer lifecycle and component dependency**

Allocate `INFERENCE_MAX_JPEG_BYTES` with
`MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT` before starting either server. Add both
routes to `register_control_handlers()` and keep `max_uri_handlers=8`. Release
the buffer after startup rollback or `http_capture_stop()` only when both
server handles are null. Add `INFERENCE` to `HTTP_CAPTURE`'s `REQUIRES` list.

- [ ] **Step 6: Run the HTTP behavior test and verify GREEN**

Run the command from Step 2. Expected: endpoint, escaping, status mapping,
image bytes/header, allocation rollback, and existing stream tests all pass.

- [ ] **Step 7: Commit the inference HTTP API**

Run `git diff --check`, stage only Task 2 files, and commit:

```bash
git commit -m "feat: serve inference snapshots over http"
```

---

### Task 3: Build the responsive live-and-inference dashboard

**Files:**
- Create: `components/HTTP_CAPTURE/dashboard_page.c`
- Create: `components/HTTP_CAPTURE/dashboard_page.h`
- Modify: `components/HTTP_CAPTURE/http_capture.c`
- Modify: `components/HTTP_CAPTURE/CMakeLists.txt`
- Modify: `tests/host/http_capture_component_test.c`

**Interfaces:**
- Consumes: Task 2 endpoints and the existing `:81/stream`, `/capture`, pause, and resume behavior.
- Produces: `const char *http_capture_dashboard_html(void)` used by the index handler.

- [ ] **Step 1: Write failing dashboard assertions**

Extend the index handler test to require these user-visible and behavioral
markers in the served HTML:

```c
assert(strstr(html, "id=\"livePreview\"") != NULL);
assert(strstr(html, "id=\"inferenceSnapshot\"") != NULL);
assert(strstr(html, "id=\"prediction\"") != NULL);
assert(strstr(html, "id=\"scores\"") != NULL);
assert(strstr(html, "id=\"timing\"") != NULL);
assert(strstr(html, "fetch('/api/inference'") != NULL);
assert(strstr(html, "/api/inference/image?sequence=") != NULL);
assert(strstr(html, "X-Inference-Sequence") != NULL);
assert(strstr(html, "URL.createObjectURL") != NULL);
assert(strstr(html, "URL.revokeObjectURL") != NULL);
assert(strstr(html, ".textContent=") != NULL);
assert(strstr(html, "innerHTML") == NULL);
assert(strstr(html, "grid-template-columns") != NULL);
assert(strstr(html, "@media") != NULL);
```

Keep assertions for stream URL, capture button, pause/resume behavior, and page
cache headers.

- [ ] **Step 2: Run the HTTP behavior test and verify RED**

Run the Task 2 targeted HTTP command. Expected: failure because the current
page has one image and no inference polling or result elements.

- [ ] **Step 3: Extract and implement the dashboard page**

Create an internal header declaring:

```c
const char *http_capture_dashboard_html(void);
```

Create `dashboard_page.c` with the static responsive HTML. Desktop layout uses
two equal panels; a media query stacks them on narrow screens. Keep the current
live-stream, pause/resume, and capture controls.

Implement a single non-overlapping one-second poll loop. When metadata is not
ready, show `Waiting for first inference`. For a new sequence, fetch
`/api/inference/image?sequence=${metadata.sequence}`, verify the response
header, convert the blob to an object URL, set the snapshot image, render the
prediction/timing/dynamic score rows with `createElement` and `textContent`,
then revoke the previous object URL. On 409, mismatched sequence, or network
failure, retain the last complete pair and retry on the next poll.

Update the index handler to call `http_capture_dashboard_html()` and add
`dashboard_page.c` to component sources and the HTTP host compile command.

- [ ] **Step 4: Run the HTTP behavior test and verify GREEN**

Run the Task 2 targeted HTTP command. Expected: all endpoint, lifecycle,
stream, and dashboard assertions pass.

- [ ] **Step 5: Commit the dashboard**

Run `git diff --check`, stage only Task 3 files, and commit:

```bash
git commit -m "feat: display live and inference images"
```

---

### Task 4: Record deployment experience and future upgrade path

**Files:**
- Create: `docs/experience/edge-impulse-esp32s3-deployment.md`
- Modify: `README.md`

**Interfaces:**
- Consumes: serial evidence, committed fixes, the new endpoints, and the approved design specification.
- Produces: a reusable deployment investigation record and current dashboard usage instructions.

- [ ] **Step 1: Write the deployment experience document**

Create the Chinese-language document with these exact top-level sections:

```markdown
# Edge Impulse 模型部署到 ESP32-S3：问题、根因与解决方案

## 最终结果
## 部署架构与资源边界
## 问题一：模型组件与固件资源配置
## 问题二：参考卷积导致约 13.6 秒推理与看门狗报警
## 问题三：EON overflow buffer 数量不足
## 问题四：ESP-NN scratch buffer 未按 16 字节对齐
## 为什么 CPU1 与 ESP-NN 要同时使用
## 硬件验收方法
## 后续重新训练与模型升级清单
## 后续路线图与优先级
```

For every problem include symptom/log evidence, backward-traced root cause,
selected change, rejected alternatives, why the selected change fixes the
cause, and regression/hardware verification. Record measured DSP `26 ms`,
classification `289–290 ms`, the prior `13.6 s`, approximately `47x`
classification acceleration, probabilities summing approximately to one after
INT8 quantization, and threshold behavior (`0.54688` becomes `uncertain` under
threshold `0.60`).

The final roadmap order is:

1. Current inference dashboard and endurance verification.
2. Retrain and replace the Edge Impulse export with label-count, arena,
   partition, accuracy, timing, and endurance gates.
3. Signed dual-slot application OTA using `ota_0`, `ota_1`, `otadata`,
   anti-rollback, first-boot health confirmation, and automatic rollback.
4. Bootloader OTA only after a recovery path and power-loss testing; explain
   that application OTA already upgrades the linked model and bootloader OTA
   can brick the device, so it stays last.

- [ ] **Step 2: Update user-facing dashboard documentation**

Update README's page instructions and endpoint list with the two-panel layout,
one-second result refresh, strict snapshot pairing, dynamic labels, and:

```text
GET /api/inference
GET /api/inference/image?sequence=N
```

State that serial and page values must match for the same inference sequence.

- [ ] **Step 3: Review documentation accuracy and commit**

Run `git diff --check` and scan both documents for unfinished-marker tokens,
ambiguous future promises, and contradictions with the design. Stage only the
experience document and README, then commit:

```bash
git commit -m "docs: record edge impulse deployment lessons"
```

---

### Task 5: Full verification, review, and PR update

**Files:**
- Verify all changed source, test, documentation, and build files.

**Interfaces:**
- Consumes: Tasks 1–4.
- Produces: a reviewed, pushed update to Draft PR #3 and a hardware acceptance checklist.

- [ ] **Step 1: Run the full host regression suite**

Run:

```bash
python3 -m unittest discover -s tests -v
```

Expected: all existing and new host tests pass with zero failures.

- [ ] **Step 2: Run a clean ESP-IDF 5.5.4 build**

Use an isolated temporary build directory and sdkconfig derived from
`sdkconfig.defaults`:

```bash
export IDF_PYTHON_ENV_PATH=/Users/stephenapollo/.espressif/tools/python/v5.5.4/venv
export IDF_PYTHON_CHECK_CONSTRAINTS=no
source /Users/stephenapollo/.espressif/v5.5.4/esp-idf/export.sh >/dev/null
idf.py -B /private/tmp/edgeDeploy-dashboard-build -D SDKCONFIG=/private/tmp/edgeDeploy-dashboard-sdkconfig build
```

Expected: C/C++ compile and link succeed, ESP-NN remains linked, and the image
fits the four-megabyte application partition.

- [ ] **Step 3: Audit and request independent code review**

Run `git diff --check`, `git status --short`, and inspect the branch commits
since `159829e`. Confirm `.gitignore` is the only unrelated worktree change.
Request read-only review against the design and this plan; fix every Critical
or Important issue and re-run affected tests.

- [ ] **Step 4: Push and verify Draft PR #3**

Push `feature/edge-impulse-inference`. Verify PR #3 remains open and draft,
targets `main`, and includes the dashboard and experience commits.

- [ ] **Step 5: Hand off hardware acceptance**

Ask the user to flash the pushed firmware and verify at least five consecutive
sequences where the right-hand JPEG and displayed scores correspond to the
serial result, classification remains below five seconds, live preview remains
usable, and no allocation, heap, panic, or watchdog errors appear. Do not claim
hardware acceptance before receiving that evidence.

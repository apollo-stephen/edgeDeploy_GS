# Runtime Health Dashboard Control Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a dashboard switch that dynamically starts and stops the ESP32-S3 runtime health task, automatically stops it after a 10-second dashboard lease expires, and displays current metrics plus two 60-second charts.

**Architecture:** The `HEALTH` component owns a serialized `OFF → STARTING → RUNNING → STOPPING` lifecycle and a dynamically allocated FreeRTOS task. The HTTP component exposes state/control endpoints and refreshes the lease, while the embedded dashboard keeps only 60 samples in browser memory and renders the selected collapsible layout without external libraries.

**Tech Stack:** ESP-IDF 5.5.4, ESP32-S3, FreeRTOS task notifications, `esp_http_server`, embedded C/C++, plain HTML/CSS/JavaScript/SVG, Python `unittest`, host C tests, Node.js dashboard behavior tests.

## Global Constraints

- Runtime health monitoring is disabled after every boot; no state is stored in NVS.
- A running dashboard refreshes a 10-second lease through `GET /api/health`; lease expiry follows the safe task-exit path.
- Explicit stop waits at most 2 seconds for task exit.
- Inference runtime counters remain active while health monitoring is off.
- The board stores only the latest snapshot; the browser stores at most 60 distinct sequences and writes nothing to Flash.
- Do not add external browser dependencies or change camera, stream, or inference behavior.
- Preserve the existing JSON snapshot fields and add `enabled` plus off/starting responses.
- Use the exact disabled copy: `健康任务未开启，网页不请求健康数据，不占用监控任务栈。`
- Do not modify or discard the user's main-workspace `.gitignore` change.

---

## File Structure

- `components/HEALTH/include/health.h`: public monitor lifecycle, status, lease-refresh, and snapshot interfaces.
- `components/HEALTH/health.c`: lifecycle state, dynamic task creation/exit, lease evaluation, periodic sampling, and snapshot synchronization.
- `components/HEALTH/health_internal.h`: host-test entry points for one sampling/lifecycle iteration only.
- `tests/host/health_component_test.c`: deterministic FreeRTOS fakes and lifecycle/lease/resource tests.
- `tests/host/include/freertos/task.h`: host declarations for task notification, deletion, and tick-delay APIs used by `HEALTH`.
- `tests/test_health_component.py`: builds the host health executable and runs every named scenario.
- `main/main.c`: no longer starts health during boot.
- `main/CMakeLists.txt`: removes the direct `HEALTH` dependency; `HTTP_CAPTURE` remains the component consumer.
- `tests/host/http_image_transfer_integration_test.c`: verifies startup succeeds without a health-start call.
- `tests/test_http_image_transfer_integration.py`: removes obsolete health-start failure coverage.
- `tests/host/include/esp_http_server.h`: host request-body and `HTTP_POST` support.
- `components/HTTP_CAPTURE/http_capture.c`: health state JSON, lease refresh, strict control-body parsing, and `/api/health/control` registration.
- `tests/host/http_capture_component_test.c`: health API state/control tests and host stubs.
- `components/HTTP_CAPTURE/dashboard_page.c`: collapsible A-layout panel, switch state machine, metric cards, bounded history, and SVG charts.
- `tests/test_http_capture_component.py`: Node.js DOM/fetch behavior tests for health enable, sampling, chart bounds, disable, and errors.
- `README.md`, `README.zh-CN.md`: runtime-switch usage, API contracts, lease behavior, and updated architecture.
- `docs/acceptance/v0.3.0-runtime-health-dashboard.md`: final automated and hardware acceptance evidence.
- `docs/EdgeDeploy项目进度.local.md`, `docs/EdgeDeploy重构开发方案.md`: local project status and verified-design notes updated only after hardware acceptance.

---

### Task 1: Dynamic Health Lifecycle and Default-off Startup

**Files:**
- Modify: `components/HEALTH/include/health.h`
- Modify: `components/HEALTH/health.c`
- Modify: `components/HEALTH/health_internal.h`
- Modify: `tests/host/health_component_test.c`
- Modify: `tests/host/include/freertos/task.h`
- Modify: `tests/test_health_component.py`
- Modify: `main/main.c`
- Modify: `main/CMakeLists.txt`
- Modify: `tests/host/http_image_transfer_integration_test.c`
- Modify: `tests/test_http_image_transfer_integration.py`

**Interfaces:**
- Produces:
  - `health_monitor_lifecycle_t` with `HEALTH_MONITOR_OFF`, `HEALTH_MONITOR_STARTING`, `HEALTH_MONITOR_RUNNING`, and `HEALTH_MONITOR_STOPPING`.
  - `health_monitor_status_t { bool enabled; bool ready; health_monitor_lifecycle_t lifecycle; }`.
  - `esp_err_t health_set_enabled(bool enabled)`.
  - `esp_err_t health_get_monitor_status(health_monitor_status_t *status)`.
  - `esp_err_t health_refresh_lease(void)`.
  - Existing `esp_err_t health_get_snapshot(health_snapshot_t *snapshot)` remains side-effect free.
- Consumes: existing inference runtime statistics and heap/stack sampling functions.

- [ ] **Step 1: Rewrite lifecycle tests to express the new default-off contract**

Add named scenarios `control`, `lease`, and `restart` to `tests/host/health_component_test.c`. The core assertions must include:

```c
health_monitor_status_t status = {0};
assert(health_get_monitor_status(&status) == ESP_OK);
assert(!status.enabled);
assert(!status.ready);
assert(status.lifecycle == HEALTH_MONITOR_OFF);

assert(health_set_enabled(true) == ESP_OK);
assert(s_task_create_calls == 1);
assert(health_set_enabled(true) == ESP_OK);
assert(s_task_create_calls == 1);

assert(health_refresh_lease() == ESP_OK);
s_now_us += 9000000;
assert(!health_test_lease_expired(s_now_us));
s_now_us += 1000001;
assert(health_test_lease_expired(s_now_us));
```

Extend the FreeRTOS fakes to record `xTaskNotifyGive()`, `ulTaskNotifyTake()`, and `vTaskDelete()` so an explicit stop can synchronously drive the captured task to its exit path. Preserve the existing transition and resource-snapshot assertions.

- [ ] **Step 2: Run the health tests and verify the new contract fails**

Run:

```bash
python3 -m unittest tests.test_health_component -v
```

Expected: compilation fails because the lifecycle/status types, control functions, lease test helper, and task-notification declarations do not exist.

- [ ] **Step 3: Add the public lifecycle API and host FreeRTOS declarations**

Add to `health.h`:

```c
typedef enum {
    HEALTH_MONITOR_OFF,
    HEALTH_MONITOR_STARTING,
    HEALTH_MONITOR_RUNNING,
    HEALTH_MONITOR_STOPPING,
} health_monitor_lifecycle_t;

typedef struct {
    bool enabled;
    bool ready;
    health_monitor_lifecycle_t lifecycle;
} health_monitor_status_t;

esp_err_t health_set_enabled(bool enabled);
esp_err_t health_get_monitor_status(health_monitor_status_t *status);
esp_err_t health_refresh_lease(void);
esp_err_t health_get_snapshot(health_snapshot_t *snapshot);
```

Add the matching host declarations to `tests/host/include/freertos/task.h`:

```c
BaseType_t xTaskNotifyGive(TaskHandle_t task);
uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit, TickType_t timeout);
void vTaskDelete(TaskHandle_t task);
```

Keep `health_test_lease_expired(uint64_t now_us)` in `health_internal.h`; it must evaluate production lease state without advancing the clock.

- [ ] **Step 4: Implement the lifecycle, lease, and self-exit path**

Replace unconditional `health_start()` behavior with:

```c
#define HEALTH_CLIENT_LEASE_US 10000000ULL
#define HEALTH_STOP_TIMEOUT_MS 2000U

static health_monitor_lifecycle_t s_lifecycle = HEALTH_MONITOR_OFF;
static bool s_stop_requested;
static uint64_t s_lease_refreshed_us;

esp_err_t health_refresh_lease(void)
{
    if (s_lifecycle != HEALTH_MONITOR_RUNNING &&
        s_lifecycle != HEALTH_MONITOR_STARTING) {
        return ESP_ERR_INVALID_STATE;
    }
    s_lease_refreshed_us = (uint64_t)esp_timer_get_time();
    return ESP_OK;
}
```

The task loop must sample immediately, then block with `ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000))`. Before every sample it checks the stop request and lease. On exit it resets readiness/snapshot state, sets lifecycle `OFF`, clears its handle, calls `vTaskDelete(NULL)`, and returns for host-test fakes. Explicit stop sets `STOPPING`, notifies the task, and waits in bounded 10 ms intervals until `OFF` or 2 seconds elapse. Keep all shared state copies protected by the component's existing synchronization mechanism; never call `vTaskDelete()` on the health task from the HTTP/control caller.

- [ ] **Step 5: Remove boot-time health startup and update startup integration expectations**

Delete the `health_start()` call and its fail-fast branch from `main/main.c`, remove the direct `HEALTH` entry from `main/CMakeLists.txt`, and change the successful startup order to:

```c
const enum call_id expected[] = {
    CALL_NVS_INIT,
    CALL_CAMERA_INIT,
    CALL_WIFI_INIT,
    CALL_INFERENCE_START,
    CALL_HTTP_START,
};
```

Remove `CALL_HEALTH_START`, the `health_start()` fake, and the `health-failure` scenario from the startup integration test and Python scenario list.

- [ ] **Step 6: Run focused lifecycle and startup tests**

Run:

```bash
python3 -m unittest tests.test_health_component tests.test_http_image_transfer_integration -v
```

Expected: all health lifecycle/resource scenarios and every startup fail-fast scenario pass.

- [ ] **Step 7: Commit the lifecycle slice**

```bash
git add components/HEALTH main tests/host/health_component_test.c tests/host/include/freertos/task.h tests/host/http_image_transfer_integration_test.c tests/test_health_component.py tests/test_http_image_transfer_integration.py
git commit -m "feat: control runtime health lifecycle"
```

---

### Task 2: HTTP Health Control and Lease Refresh

**Files:**
- Modify: `tests/host/include/esp_http_server.h`
- Modify: `tests/host/http_capture_component_test.c`
- Modify: `components/HTTP_CAPTURE/http_capture.c`
- Modify: `tests/test_http_capture_component.py`

**Interfaces:**
- Consumes: `health_set_enabled()`, `health_get_monitor_status()`, `health_refresh_lease()`, and `health_get_snapshot()` from Task 1.
- Produces: `POST /api/health/control` plus the revised `GET /api/health` contract.

- [ ] **Step 1: Add failing route and JSON contract tests**

Extend the host `httpd_req_t` with request-body storage and length, add `HTTP_POST`, and fake `httpd_req_recv()`. Register expectations for seven control routes and test:

```c
const httpd_uri_t *control_uri = find_uri(0, "/api/health/control");
assert(control_uri != NULL);
assert(control_uri->method == HTTP_POST);

httpd_req_t request = {
    .content_len = strlen("{\"enabled\":true}"),
    .request_body = "{\"enabled\":true}",
};
assert(control_uri->handler(&request) == ESP_OK);
assert(strcmp(request.response_body,
              "{\"enabled\":true,\"ready\":false,\"state\":\"starting\"}") == 0);
```

Also assert that off GET is exactly `{"enabled":false,"ready":false,"state":"off"}`, starting GET refreshes the lease, ready GET contains `"enabled":true`, invalid/oversized/trailing input returns 400, start failure returns 500, and `{"enabled":false}` invokes a bounded stop.

- [ ] **Step 2: Run the HTTP component test and verify failure**

Run:

```bash
python3 -m unittest tests.test_http_capture_component.HttpCaptureComponentBehaviorTest.test_routes_capture_ownership_and_preview_controls -v
```

Expected: compilation or assertions fail because POST request support, the control route, and enabled lifecycle JSON do not exist.

- [ ] **Step 3: Implement strict control parsing and lifecycle responses**

Add `health_control_post_handler()` using a fixed request buffer large enough for the two accepted payloads. Require the body, after permitted JSON whitespace, to contain exactly one boolean `enabled` member; reject extra keys, duplicate keys, non-booleans, truncated receives, and oversized bodies with `HTTPD_400`.

Use one response helper for off/starting state:

```c
static esp_err_t send_health_monitor_state(httpd_req_t *request,
                                           bool enabled,
                                           bool ready,
                                           const char *state_name)
{
    char response[96];
    const int length = snprintf(response, sizeof(response),
        "{\"enabled\":%s,\"ready\":%s,\"state\":\"%s\"}",
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
```

In `GET /api/health`, query monitor status first. Return off without refreshing. If enabled, refresh the lease, then return starting or the existing ready snapshot with `"enabled":true` added before `sequence`.

- [ ] **Step 4: Register the POST route and update handler capacity checks**

Register `/api/health/control` after `/api/health`, update host route-count expectations from 6 to 7, and retain `max_uri_handlers == 8` so no server configuration expansion is needed.

- [ ] **Step 5: Run the HTTP and startup integration tests**

Run:

```bash
python3 -m unittest tests.test_http_capture_component tests.test_http_image_transfer_integration -v
```

Expected: control JSON, snapshot JSON, registration rollback, camera/stream behavior, and startup behavior all pass.

- [ ] **Step 6: Commit the API slice**

```bash
git add components/HTTP_CAPTURE/http_capture.c tests/host/http_capture_component_test.c tests/host/include/esp_http_server.h tests/test_http_capture_component.py
git commit -m "feat: expose runtime health control"
```

---

### Task 3: Collapsible Dashboard Metrics and Charts

**Files:**
- Modify: `components/HTTP_CAPTURE/dashboard_page.c`
- Modify: `tests/test_http_capture_component.py`

**Interfaces:**
- Consumes: `GET /api/health` and `POST /api/health/control` from Task 2.
- Produces: the approved A-layout dashboard interaction with a 60-sample browser history.

- [ ] **Step 1: Add failing Node.js behavior tests for the health panel**

Extend the existing Node DOM fake with `hidden`, `disabled`, `dataset`, `setAttribute()`, `getAttribute()`, `classList.toggle()`, and SVG path attributes. Add a health fetch script that proves:

```javascript
await discoverHealthState();
if (element('healthPanelBody').hidden !== true) throw new Error('off panel expanded');

await setHealthEnabled(true);
if (requests[0].url !== '/api/health/control') throw new Error('missing control POST');

for (let sequence = 1; sequence <= 65; sequence += 1) {
  appendHealthSample(makeHealthSample(sequence));
}
if (healthHistory.length !== 60) throw new Error('history is not bounded');
appendHealthSample(makeHealthSample(65));
if (healthHistory.length !== 60) throw new Error('duplicate sequence appended');

await setHealthEnabled(false);
if (healthHistory.length !== 0) throw new Error('history not cleared');
```

Also assert the exact Chinese disabled copy, both chart titles, error-state text, and that no health interval is created when discovery reports off.

- [ ] **Step 2: Run dashboard tests and verify failure**

Run:

```bash
python3 -m unittest tests.test_http_capture_component.HttpCaptureComponentBehaviorTest.test_dashboard_keeps_results_current_when_image_decode_fails -v
```

Expected: the new DOM/behavior assertions fail because the health panel functions and elements are absent.

- [ ] **Step 3: Add the approved responsive panel markup and styles**

Append the `Runtime health` section below `Latest result`. Use these stable element IDs so tests and logic remain explicit:

```html
<section id="healthPanel" class="health-panel">
  <div class="health-head">
    <div><h2>Runtime health</h2><p id="healthSubtitle"></p></div>
    <span id="healthState" hidden></span>
    <button id="healthToggle" type="button" role="switch" aria-checked="false"></button>
  </div>
  <p id="healthOffCopy">健康任务未开启，网页不请求健康数据，不占用监控任务栈。</p>
  <div id="healthPanelBody" hidden></div>
</section>
```

Inside the body add the six approved metric values, `healthReasons`, `latencyChart`, and `memoryChart`. Use CSS Grid with three metric columns and two chart columns on desktop, two metric columns and one chart column below 560 px.

- [ ] **Step 4: Implement state discovery, control, bounded history, and SVG rendering**

Implement the named functions `discoverHealthState()`, `setHealthEnabled(enabled)`, `pollHealth()`, `appendHealthSample(snapshot)`, `renderHealthSnapshot(snapshot)`, `renderHealthCharts()`, `clearHealthHistory()`, and `setHealthConnectionError(message)`.

Use this state rule:

```javascript
const healthHistory = [];
let lastHealthSequence = 0;
function appendHealthSample(snapshot) {
  if (!snapshot.ready || snapshot.sequence === lastHealthSequence) return;
  lastHealthSequence = snapshot.sequence;
  healthHistory.push(snapshot);
  if (healthHistory.length > 60) healthHistory.splice(0, healthHistory.length - 60);
}
```

Render SVG paths from normalized sample coordinates without a library. Latency uses `last_duration_ms`; memory uses `internal.free_bytes / 1024` and `internal.largest_free_block_bytes / 1024`. Empty and one-point histories must produce valid, finite coordinates. Decode the four existing `reason_flags` bits into readable labels and show the raw mask only for unknown bits.

Page load calls `discoverHealthState()` once. The health interval exists only while the board reports enabled. Explicit disable stops the interval only after the POST succeeds, clears history, and collapses the panel. Fetch failure shows `连接中断 / 状态未知`; a later successful discovery restores the board's actual state.

- [ ] **Step 5: Run dashboard and HTTP tests**

Run:

```bash
python3 -m unittest tests.test_http_capture_component -v
```

Expected: Node syntax/behavior, dashboard rendering, API routes, stream behavior, and response serialization pass.

- [ ] **Step 6: Commit the dashboard slice**

```bash
git add components/HTTP_CAPTURE/dashboard_page.c tests/test_http_capture_component.py
git commit -m "feat: add runtime health dashboard panel"
```

---

### Task 4: Documentation and Complete Software Verification

**Files:**
- Modify: `README.md`
- Modify: `README.zh-CN.md`
- Modify: `docs/superpowers/specs/2026-08-10-runtime-health-dashboard-control-design.md`
- Create after verification: `docs/acceptance/v0.3.0-runtime-health-dashboard.md`

**Interfaces:**
- Consumes: completed lifecycle, HTTP API, and dashboard behavior from Tasks 1–3.
- Produces: build/test evidence and user-facing operation documentation.

- [ ] **Step 1: Update public documentation**

Document that monitoring is off after boot, the dashboard switch starts it, GET polling renews a 10-second lease, direct page closure causes automatic stop, and history is browser-only. Add both API endpoints and the three response forms (off, starting, ready). Update the architecture description so `main` no longer starts `HEALTH` directly.

- [ ] **Step 2: Run the complete host suite**

Run:

```bash
python3 -m unittest discover -s tests -v
```

Expected: every discovered test passes with no skips except an explicitly reported missing optional Node runtime; on this workstation Node is present, so the dashboard behavior test must run.

- [ ] **Step 3: Compile every Python test module**

Run:

```bash
python3 -m py_compile tests/*.py
```

Expected: exit status 0 and no output.

- [ ] **Step 4: Build the ESP32-S3 firmware**

Run from the feature worktree:

```bash
source /Users/stephenapollo/.espressif/v5.5.4/esp-idf/export.sh
idf.py -B build-esp32s3 -D SDKCONFIG=sdkconfig.esp32s3 build
```

Expected: ESP-IDF 5.5.4 build succeeds for ESP32-S3, the application fits the 4 MiB factory partition, and the size output is recorded in the acceptance document.

- [ ] **Step 5: Write software-verification evidence without claiming hardware completion**

Create `docs/acceptance/v0.3.0-runtime-health-dashboard.md` with exact test counts, build hash/version, binary size/free partition space, and a hardware checklist explicitly marked pending. Do not mark the release complete or create a tag yet.

- [ ] **Step 6: Commit documentation and software evidence**

```bash
git add README.md README.zh-CN.md docs/superpowers/specs/2026-08-10-runtime-health-dashboard-control-design.md docs/acceptance/v0.3.0-runtime-health-dashboard.md
git commit -m "docs: explain runtime health dashboard control"
```

---

### Task 5: Hardware Acceptance, Local Project Records, and Main Integration

**Files:**
- Modify: `docs/acceptance/v0.3.0-runtime-health-dashboard.md`
- Modify outside tracked feature scope: `docs/EdgeDeploy项目进度.local.md`
- Modify outside tracked feature scope: `docs/EdgeDeploy重构开发方案.md`

**Interfaces:**
- Consumes: the verified ESP32-S3 firmware from Task 4.
- Produces: hardware evidence, final documentation, merge-ready branch, and release tag.

- [ ] **Step 1: Ask the user to power the ESP32-S3 only when flashing is ready**

Keep the board off during software work. When the build artifact and serial port command are ready, explicitly tell the user that flashing is about to begin and wait for the user to confirm power-on.

- [ ] **Step 2: Flash and confirm the exact application version**

Run:

```bash
idf.py -B build-esp32s3 -p /dev/cu.usbmodem5B900039351 flash monitor
```

Expected: flash hashes verify, boot log reports the new feature-branch application hash, inference starts, HTTP starts, and no health task starts during boot.

- [ ] **Step 3: Execute the runtime acceptance matrix**

Record exact observations for:

1. `GET /api/health` is off after boot while inference counters/results continue.
2. Toggle on reaches starting then healthy; metrics and both charts update once per distinct sequence.
3. Toggle off immediately stops monitoring and leaves inference running.
4. Toggle on, close the page, wait 10–12 seconds, reopen, and confirm off due to lease expiry.
5. Refresh while enabled and toggle rapidly; confirm one health task and correct final state.
6. Verify desktop and phone layouts do not overflow and the disabled Chinese copy is exact.

- [ ] **Step 4: Update acceptance and local project documents with observed facts**

Replace pending hardware items in the tracked acceptance document with exact pass/fail evidence. Update `EdgeDeploy项目进度.local.md` and `EdgeDeploy重构开发方案.md` to distinguish software verification from the completed real-board checks and to describe the runtime-on-demand architecture. Do not stage the local ignored documents unless the repository's established policy changes.

- [ ] **Step 5: Re-run final verification before integration**

Run:

```bash
python3 -m unittest discover -s tests -v
python3 -m py_compile tests/*.py
git diff --check
git status --short
```

Expected: all tests pass, Python compilation succeeds, no whitespace errors exist, and only known generated `build-esp32s3/` plus `sdkconfig.esp32s3` remain untracked.

- [ ] **Step 6: Commit hardware acceptance evidence**

```bash
git add docs/acceptance/v0.3.0-runtime-health-dashboard.md
git commit -m "docs: accept runtime health dashboard"
```

- [ ] **Step 7: Integrate without disturbing the user's dirty main-workspace file**

Verify that the main workspace still differs only by the user's `.gitignore` edit plus known generated files. Merge `feature/runtime-health-monitor` into `main` only if Git reports no overlap with that edit, then tag the accepted merge as:

```bash
git tag -a v0.3.0-runtime-health-dashboard -m "Runtime health dashboard"
```

Do not push or publish the tag unless the user explicitly requests it.

# Runtime Health Monitor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an application-owned health task that detects inference freshness and failures, samples task-stack and heap resources, logs health transitions, and exposes a read-only `/api/health` endpoint without automatic recovery.

**Architecture:** The `INFERENCE` component publishes a fixed-size runtime-statistics snapshot protected by a short critical section. A new unpinned, priority-1 `HEALTH` task samples that snapshot and ESP-IDF heap metrics once per second, derives `STARTING`/`HEALTHY`/`DEGRADED`, and publishes a mutex-protected health snapshot for the HTTP component. HTTP serializes the latest snapshot but never performs sampling or recovery itself.

**Tech Stack:** C11, C++17, ESP-IDF 5.5.4, FreeRTOS SMP, ESP32-S3, `esp_timer`, ESP-IDF heap capabilities, Python `unittest`, host C/C++ test doubles.

## Global Constraints

- Preserve the existing OV5640 capture, MJPEG stream, dataset tool, MobileNetV1 inference, inference-snapshot, and dashboard behavior.
- Monitor only the application-owned `ei_inference` and `runtime_health` tasks; do not manufacture heartbeats for ESP-IDF-owned Wi-Fi, HTTP, idle, or camera-driver tasks.
- Do not add queues, Trace Hooks, CPU-utilization accounting, task-migration counters, ISR timing, or strict Deadline claims.
- Do not subscribe tasks to a watchdog, restart tasks/components, reboot, persist fault records, or inject faults in this increment.
- Use a 1,000 ms health sample interval, priority 1, 4,096-byte task stack, and no core affinity.
- Use a 7,000 ms startup grace, 6,000 ms inference freshness limit, and three-consecutive-failure threshold.
- Treat memory and stack data as observational; do not invent low-memory or low-stack alarm thresholds before hardware baselines exist.
- Log only health-state transitions, never one line per sample.
- Keep the existing user-owned `.gitignore` modification unstaged and unchanged.
- Implement on an isolated `feature/runtime-health-monitor` branch/worktree; do not mix the feature into the intended `v0.2.0-inference-dashboard` tag.
- Do not add dashboard UI in this increment; `/api/health` is inspected through a browser or `curl`.

## File Structure

### Create

- `components/HEALTH/CMakeLists.txt` — registers the health component and its dependencies.
- `components/HEALTH/include/health.h` — public state, reason flags, snapshot, and read-only lifecycle API.
- `components/HEALTH/health_internal.h` — component-private single-sample function used by the task and host tests.
- `components/HEALTH/health.c` — task lifecycle, state derivation, resource sampling, snapshot publication, and transition logging.
- `tests/host/health_component_test.c` — deterministic C host scenarios for health lifecycle, state transitions, and resource data.
- `tests/test_health_component.py` — builds and runs the health host test executable.

### Modify

- `components/INFERENCE/include/inference.h` — adds `inference_runtime_stats_t` and its read-only getter.
- `components/INFERENCE/inference.cpp` — retains the task handle and records every inference attempt.
- `tests/host/inference_component_test.cpp` — verifies runtime statistics across existing success/failure paths.
- `tests/host/include/freertos/task.h` — adds host declarations for unpinned task creation and stack-watermark sampling.
- `tests/host/include/esp_heap_caps.h` — adds internal-memory capability and minimum/largest heap probe declarations.
- `tests/test_inference_component.py` — continues compiling the expanded inference host test.
- `components/HTTP_CAPTURE/CMakeLists.txt` — declares the `HEALTH` dependency.
- `components/HTTP_CAPTURE/http_capture.c` — registers and serves `/api/health`.
- `tests/host/http_capture_component_test.c` — stubs health snapshots and verifies bounded JSON behavior.
- `tests/test_http_capture_component.py` — adds the public health include path to host compilation.
- `main/CMakeLists.txt` — declares the application dependency on `HEALTH`.
- `main/main.c` — starts health after inference and before HTTP.
- `tests/host/http_image_transfer_integration_test.c` — verifies startup order and health failure behavior.
- `tests/test_http_image_transfer_integration.py` — adds the health include path and scenario.
- `README.md` — documents the health task and API in English.
- `README.zh-CN.md` — documents the same behavior in Chinese.
- `docs/EdgeDeploy项目进度.local.md` — records the actual implementation/test/hardware status after verification; remains local and uncommitted.
- `docs/EdgeDeploy重构开发方案.md` — changes health-monitor items from planned to verified only after hardware acceptance; remains locally excluded.

---

### Task 1: Publish Inference Runtime Statistics

**Files:**
- Modify: `components/INFERENCE/include/inference.h`
- Modify: `components/INFERENCE/inference.cpp`
- Modify: `tests/host/inference_component_test.cpp`
- Modify: `tests/host/include/freertos/task.h`
- Test: `tests/test_inference_component.py`

**Interfaces:**
- Consumes: existing `inference_start()`, `inference_run_once()`, `esp_timer_get_time()`, and `uxTaskGetStackHighWaterMark(TaskHandle_t)`.
- Produces: `inference_runtime_stats_t` and `esp_err_t inference_get_runtime_stats(inference_runtime_stats_t *stats)` for Task 2.

- [ ] **Step 1: Add failing lifecycle and success-statistics assertions**

Extend the host task stub so the production component must retain a non-null
task handle and can query its stack watermark:

```cpp
static int s_fake_inference_task;
static TaskHandle_t s_created_task_handle = &s_fake_inference_task;
static UBaseType_t s_stack_high_water_mark = 3072;

extern "C" BaseType_t xTaskCreatePinnedToCore(
    TaskFunction_t task,
    const char *name,
    configSTACK_DEPTH_TYPE stack_depth,
    void *argument,
    UBaseType_t priority,
    TaskHandle_t *task_handle,
    BaseType_t core_id)
{
    assert(task != nullptr);
    assert(strcmp(name, "ei_inference") == 0);
    assert(argument == nullptr);
    assert(task_handle != nullptr);
    *task_handle = s_created_task_handle;
    ++s_task_create_calls;
    s_task_stack_depth = stack_depth;
    s_task_priority = priority;
    s_task_core_id = core_id;
    return s_task_result;
}

extern "C" UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task)
{
    assert(task == s_created_task_handle);
    return s_stack_high_water_mark;
}
```

Add pre-start, post-start, and first-success assertions:

```cpp
inference_runtime_stats_t stats = {};
assert(inference_get_runtime_stats(nullptr) == ESP_ERR_INVALID_ARG);
assert(inference_get_runtime_stats(&stats) == ESP_ERR_NOT_FOUND);

assert(inference_start() == ESP_OK);
assert(inference_get_runtime_stats(&stats) == ESP_OK);
assert(stats.task_started);
assert(!stats.attempt_running);
assert(stats.attempt_count == 0);
assert(stats.stack_high_water_mark_bytes == s_stack_high_water_mark);

s_fake_time_us = 1000000;
assert(inference_run_once() == ESP_OK);
assert(inference_get_runtime_stats(&stats) == ESP_OK);
assert(stats.attempt_count == 1);
assert(stats.success_count == 1);
assert(stats.failure_count == 0);
assert(stats.consecutive_failure_count == 0);
assert(stats.last_error == ESP_OK);
assert(!stats.attempt_running);
assert(stats.last_success_us == stats.last_attempt_finished_us);
assert(stats.last_duration_us <= stats.max_duration_us);
```

Add this declaration to the host task header so compilation reaches the new
assertions:

```c
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task);
```

- [ ] **Step 2: Run the focused test and verify the red state**

Run:

```bash
python3 -m unittest \
  tests.test_inference_component.InferenceComponentBehaviorTest.test_task_lifecycle_frame_ownership_and_classifier_bridge \
  -v
```

Expected: compilation fails because `inference_runtime_stats_t` and
`inference_get_runtime_stats()` do not exist, or the task-handle assertion
fails because production currently passes `nullptr`.

- [ ] **Step 3: Add the public runtime-statistics interface**

Add this exact shape to `inference.h`:

```c
typedef struct {
    bool task_started;
    bool attempt_running;
    uint32_t attempt_count;
    uint32_t success_count;
    uint32_t failure_count;
    uint32_t consecutive_failure_count;
    esp_err_t last_error;
    uint64_t last_attempt_started_us;
    uint64_t last_attempt_finished_us;
    uint64_t last_success_us;
    uint64_t last_duration_us;
    uint64_t max_duration_us;
    uint32_t stack_high_water_mark_bytes;
} inference_runtime_stats_t;

esp_err_t inference_get_runtime_stats(inference_runtime_stats_t *stats);
```

Keep classification metadata and runtime statistics as separate types.

- [ ] **Step 4: Refactor one inference attempt behind a recording wrapper**

In `inference.cpp`, add dedicated state that is never protected by the
classification snapshot mutex:

```cpp
TaskHandle_t s_task_handle;
portMUX_TYPE s_runtime_lock = portMUX_INITIALIZER_UNLOCKED;
inference_runtime_stats_t s_runtime_stats;
```

Move the existing capture-to-publication body into a private helper:

```cpp
esp_err_t run_inference_attempt();
```

Keep lifecycle validation in the public wrapper and record exactly one finish
for every recorded start:

```cpp
extern "C" esp_err_t inference_run_once(void)
{
    if (!s_started || s_capture_rgb_buffer == nullptr ||
        s_model_rgb_buffer == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint64_t started_us =
        static_cast<uint64_t>(esp_timer_get_time());
    portENTER_CRITICAL(&s_runtime_lock);
    s_runtime_stats.attempt_running = true;
    ++s_runtime_stats.attempt_count;
    s_runtime_stats.last_attempt_started_us = started_us;
    portEXIT_CRITICAL(&s_runtime_lock);

    const esp_err_t result = run_inference_attempt();
    const uint64_t finished_us =
        static_cast<uint64_t>(esp_timer_get_time());
    const uint64_t duration_us = finished_us >= started_us
                                     ? finished_us - started_us
                                     : 0U;

    portENTER_CRITICAL(&s_runtime_lock);
    s_runtime_stats.attempt_running = false;
    s_runtime_stats.last_attempt_finished_us = finished_us;
    s_runtime_stats.last_duration_us = duration_us;
    if (duration_us > s_runtime_stats.max_duration_us) {
        s_runtime_stats.max_duration_us = duration_us;
    }
    s_runtime_stats.last_error = result;
    if (result == ESP_OK) {
        ++s_runtime_stats.success_count;
        s_runtime_stats.consecutive_failure_count = 0;
        s_runtime_stats.last_success_us = finished_us;
    }
    else {
        ++s_runtime_stats.failure_count;
        ++s_runtime_stats.consecutive_failure_count;
    }
    portEXIT_CRITICAL(&s_runtime_lock);
    return result;
}
```

Pass `&s_task_handle` to `xTaskCreatePinnedToCore()`. Set
`s_runtime_stats.task_started = true` only after successful task creation.
Reset the task handle and runtime snapshot during failed-start rollback.

Implement the getter by copying under the short critical section and querying
the stable task handle outside it:

```cpp
extern "C" esp_err_t inference_get_runtime_stats(
    inference_runtime_stats_t *stats)
{
    if (stats == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_started || s_task_handle == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }
    portENTER_CRITICAL(&s_runtime_lock);
    *stats = s_runtime_stats;
    portEXIT_CRITICAL(&s_runtime_lock);
    stats->stack_high_water_mark_bytes =
        static_cast<uint32_t>(uxTaskGetStackHighWaterMark(s_task_handle));
    return ESP_OK;
}
```

- [ ] **Step 5: Add failure, recovery, running-state, and max-duration assertions**

Reuse the existing invalid-frame, oversized-frame, decode, resize, and
classifier failure scenarios. After each failed `inference_run_once()`, assert:

```cpp
assert(inference_get_runtime_stats(&stats) == ESP_OK);
assert(stats.failure_count == expected_failure_count);
assert(stats.consecutive_failure_count == expected_consecutive_count);
assert(stats.last_error == expected_error);
assert(!stats.attempt_running);
assert(stats.last_attempt_finished_us >= stats.last_attempt_started_us);
```

Inside the classifier stub, call `inference_get_runtime_stats()` and assert
`attempt_running == true`. Advance `s_fake_time_us` by a controlled amount on
success and failure so the test verifies last duration, monotonic maximum
duration, and a later successful attempt resetting only the consecutive count.

- [ ] **Step 6: Run the focused and complete inference tests**

Run:

```bash
python3 -m unittest tests.test_inference_component -v
```

Expected: all inference tests pass, including every existing scenario and the
new runtime-statistics assertions.

- [ ] **Step 7: Commit Task 1**

Run `git diff --check`, verify `.gitignore` remains unstaged, then stage only
the Task 1 files and commit:

```bash
git add \
  components/INFERENCE/include/inference.h \
  components/INFERENCE/inference.cpp \
  tests/host/inference_component_test.cpp \
  tests/host/include/freertos/task.h
git commit -m "feat: record inference runtime health"
```

---

### Task 2: Add the Health Monitor Component

**Files:**
- Create: `components/HEALTH/CMakeLists.txt`
- Create: `components/HEALTH/include/health.h`
- Create: `components/HEALTH/health_internal.h`
- Create: `components/HEALTH/health.c`
- Create: `tests/host/health_component_test.c`
- Create: `tests/test_health_component.py`
- Modify: `tests/host/include/freertos/task.h`
- Modify: `tests/host/include/esp_heap_caps.h`

**Interfaces:**
- Consumes: `esp_err_t inference_get_runtime_stats(inference_runtime_stats_t *stats)` from Task 1; `esp_timer_get_time()`; ESP-IDF heap-capability probes; FreeRTOS task, mutex, delay, and stack-watermark APIs.
- Produces: `health_start()`, `health_get_snapshot()`, and `health_state_name()` for Tasks 3 and 4. The component-private `health_sample_once()` is used only by the health task and its host test.

- [ ] **Step 1: Write a failing host test for lifecycle and state transitions**

Create `tests/host/health_component_test.c` with deterministic stubs and four
command-line scenarios: `lifecycle`, `transitions`, `resources`, and
`stats-unavailable`.

The transition scenario must exercise these exact boundaries:

```c
s_now_us = 0;
s_inference_result = ESP_OK;
memset(&s_inference_stats, 0, sizeof(s_inference_stats));
assert(health_start() == ESP_OK);
assert(health_sample_once() == ESP_OK);
assert_snapshot_state(HEALTH_STATE_STARTING, 0U);

s_now_us = 7000001;
assert(health_sample_once() == ESP_OK);
assert_snapshot_state(HEALTH_STATE_DEGRADED,
                      HEALTH_REASON_STARTUP_TIMEOUT);

s_inference_stats.success_count = 1;
s_inference_stats.last_success_us = s_now_us;
assert(health_sample_once() == ESP_OK);
assert_snapshot_state(HEALTH_STATE_HEALTHY, 0U);

s_now_us += 6000001;
assert(health_sample_once() == ESP_OK);
assert_snapshot_state(HEALTH_STATE_DEGRADED,
                      HEALTH_REASON_INFERENCE_STALE);

s_inference_stats.last_success_us = s_now_us;
s_inference_stats.consecutive_failure_count = 3;
assert(health_sample_once() == ESP_OK);
assert_snapshot_state(HEALTH_STATE_DEGRADED,
                      HEALTH_REASON_CONSECUTIVE_FAILURES);

s_inference_stats.consecutive_failure_count = 0;
s_inference_stats.last_success_us = s_now_us;
assert(health_sample_once() == ESP_OK);
assert_snapshot_state(HEALTH_STATE_HEALTHY, 0U);
```

Count warning/info log calls and assert repeated samples in the same state do
not add log lines.

- [ ] **Step 2: Add the Python compile/run harness and verify the red state**

Create `tests/test_health_component.py` so it compiles with:

```python
compile_command = [
    "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
    "-DESP_LOG_CAPTURE",
    "-I", str(ROOT / "tests/host/include"),
    "-I", str(ROOT / "components/INFERENCE/include"),
    "-I", str(ROOT / "components/HEALTH/include"),
    "-I", str(ROOT / "components/HEALTH"),
    str(ROOT / "tests/host/health_component_test.c"),
    str(ROOT / "components/HEALTH/health.c"),
    "-o", str(executable),
]
```

Run:

```bash
python3 -m unittest tests.test_health_component -v
```

Expected: failure because the `HEALTH` component and public types do not exist.

- [ ] **Step 3: Define the public and private health interfaces**

Create `components/HEALTH/include/health.h` with the approved API:

```c
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "inference.h"

typedef enum {
    HEALTH_STATE_STARTING,
    HEALTH_STATE_HEALTHY,
    HEALTH_STATE_DEGRADED,
} health_state_t;

#define HEALTH_REASON_STARTUP_TIMEOUT (1U << 0)
#define HEALTH_REASON_INFERENCE_STALE (1U << 1)
#define HEALTH_REASON_CONSECUTIVE_FAILURES (1U << 2)
#define HEALTH_REASON_STATS_UNAVAILABLE (1U << 3)

typedef struct {
    bool ready;
    uint32_t sequence;
    health_state_t state;
    uint32_t reason_flags;
    uint64_t sampled_us;
    uint64_t uptime_us;
    uint64_t inference_age_us;
    inference_runtime_stats_t inference;
    uint32_t health_stack_high_water_mark_bytes;
    size_t internal_free_bytes;
    size_t internal_minimum_free_bytes;
    size_t internal_largest_free_block_bytes;
    size_t psram_free_bytes;
    size_t psram_minimum_free_bytes;
    size_t psram_largest_free_block_bytes;
} health_snapshot_t;

esp_err_t health_start(void);
esp_err_t health_get_snapshot(health_snapshot_t *snapshot);
const char *health_state_name(health_state_t state);
```

Create `health_internal.h` with only:

```c
#pragma once
#include "esp_err.h"
esp_err_t health_sample_once(void);
```

- [ ] **Step 4: Add the host declarations needed by production health code**

Extend `tests/host/include/freertos/task.h`:

```c
BaseType_t xTaskCreate(TaskFunction_t task,
                       const char *name,
                       configSTACK_DEPTH_TYPE stack_depth,
                       void *argument,
                       UBaseType_t priority,
                       TaskHandle_t *task_handle);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task);
```

Extend `tests/host/include/esp_heap_caps.h`:

```c
#define MALLOC_CAP_INTERNAL (1U << 11)
size_t heap_caps_get_minimum_free_size(unsigned int capabilities);
size_t heap_caps_get_largest_free_block(unsigned int capabilities);
```

- [ ] **Step 5: Implement lifecycle, sampling, derivation, and publication**

Register the component:

```cmake
idf_component_register(
    SRCS "health.c"
    INCLUDE_DIRS "include"
    REQUIRES INFERENCE heap
    PRIV_REQUIRES esp_timer
)
```

Use these exact constants and state holders in `health.c`:

```c
#define HEALTH_SAMPLE_INTERVAL_MS 1000U
#define HEALTH_STARTUP_GRACE_US 7000000ULL
#define HEALTH_INFERENCE_STALE_US 6000000ULL
#define HEALTH_CONSECUTIVE_FAILURE_LIMIT 3U
#define HEALTH_TASK_STACK_BYTES 4096U
#define HEALTH_TASK_PRIORITY 1U

static SemaphoreHandle_t s_snapshot_mutex;
static TaskHandle_t s_task_handle;
static health_snapshot_t s_snapshot;
static uint64_t s_started_us;
static bool s_started;
```

Derive only approved inference reasons:

```c
static health_state_t derive_state(
    uint64_t now_us,
    esp_err_t stats_result,
    const inference_runtime_stats_t *stats,
    uint32_t *reason_flags,
    uint64_t *inference_age_us)
{
    *reason_flags = 0U;
    *inference_age_us = 0U;
    if (stats_result != ESP_OK || stats == NULL) {
        *reason_flags = HEALTH_REASON_STATS_UNAVAILABLE;
        return HEALTH_STATE_DEGRADED;
    }
    if (stats->success_count == 0U) {
        if (now_us - s_started_us > HEALTH_STARTUP_GRACE_US) {
            *reason_flags |= HEALTH_REASON_STARTUP_TIMEOUT;
            return HEALTH_STATE_DEGRADED;
        }
        return HEALTH_STATE_STARTING;
    }

    *inference_age_us = now_us >= stats->last_success_us
                            ? now_us - stats->last_success_us
                            : 0U;
    if (*inference_age_us > HEALTH_INFERENCE_STALE_US) {
        *reason_flags |= HEALTH_REASON_INFERENCE_STALE;
    }
    if (stats->consecutive_failure_count >=
        HEALTH_CONSECUTIVE_FAILURE_LIMIT) {
        *reason_flags |= HEALTH_REASON_CONSECUTIVE_FAILURES;
    }
    return *reason_flags == 0U ? HEALTH_STATE_HEALTHY
                               : HEALTH_STATE_DEGRADED;
}
```

`health_sample_once()` must:

1. Read `esp_timer_get_time()` once for the sample timestamp.
2. Call `inference_get_runtime_stats()` once.
3. Populate internal and PSRAM current/minimum/largest fields using
   `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT` and
   `MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT`.
4. Query `uxTaskGetStackHighWaterMark(s_task_handle)`.
5. Set `uptime_us` to the nonnegative difference between the sample timestamp
   and `s_started_us`, then derive state, flags, and `inference_age_us`.
6. Increment a nonzero sequence, publish the complete fixed snapshot under the
   health mutex, then log only if the state changed.

The task loop is intentionally small:

```c
static void health_task(void *argument)
{
    (void)argument;
    while (true) {
        (void)health_sample_once();
        vTaskDelay(pdMS_TO_TICKS(HEALTH_SAMPLE_INTERVAL_MS));
    }
}
```

`health_start()` creates the mutex, records `s_started_us`, creates exactly one
unpinned task named `runtime_health`, and rolls back the mutex and state if task
creation fails. `health_get_snapshot()` returns `ESP_ERR_INVALID_ARG` for null,
`ESP_ERR_NOT_FOUND` before the first sample, and otherwise copies under the
mutex. `health_state_name()` returns `starting`, `healthy`, or `degraded` for
the three public states and `NULL` for an invalid enum value.

- [ ] **Step 6: Run health tests and fix only behavior covered by the design**

Run:

```bash
python3 -m unittest tests.test_health_component -v
```

Expected: lifecycle rollback, transition thresholds, combined flags,
transition-only logs, stack data, all six heap fields, and unavailable-stats
behavior pass.

- [ ] **Step 7: Commit Task 2**

Run `git diff --check`, verify `.gitignore` remains unstaged, then stage only:

```bash
git add \
  components/HEALTH/CMakeLists.txt \
  components/HEALTH/include/health.h \
  components/HEALTH/health_internal.h \
  components/HEALTH/health.c \
  tests/host/health_component_test.c \
  tests/test_health_component.py \
  tests/host/include/freertos/task.h \
  tests/host/include/esp_heap_caps.h
git commit -m "feat: add runtime health monitor"
```

---

### Task 3: Expose the Read-Only Health API

**Files:**
- Modify: `components/HTTP_CAPTURE/CMakeLists.txt`
- Modify: `components/HTTP_CAPTURE/http_capture.c`
- Modify: `tests/host/http_capture_component_test.c`
- Modify: `tests/test_http_capture_component.py`

**Interfaces:**
- Consumes: `health_get_snapshot(health_snapshot_t *)` and `health_state_name(health_state_t)` from Task 2.
- Produces: `GET /api/health`, returning `{"ready":false}` before publication and bounded JSON afterward.

- [ ] **Step 1: Add failing route and JSON assertions**

Add `components/HEALTH/include` to the host compile command. In the C host test,
stub:

```c
static esp_err_t s_health_result = ESP_ERR_NOT_FOUND;
static health_snapshot_t s_health_snapshot;

esp_err_t health_get_snapshot(health_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_health_result != ESP_OK) {
        return s_health_result;
    }
    *snapshot = s_health_snapshot;
    return ESP_OK;
}

const char *health_state_name(health_state_t state)
{
    switch (state) {
        case HEALTH_STATE_STARTING: return "starting";
        case HEALTH_STATE_HEALTHY: return "healthy";
        case HEALTH_STATE_DEGRADED: return "degraded";
        default: return NULL;
    }
}
```

Increase the expected control-handler count from 5 to 6, locate
`/api/health`, and first assert:

```c
s_health_result = ESP_ERR_NOT_FOUND;
assert(health_uri->handler(&request) == ESP_OK);
assert(strcmp(request.response_type, "application/json") == 0);
assert(strcmp(request.response_body, "{\"ready\":false}") == 0);
assert(strcmp(find_header(&request, "Cache-Control"), "no-store") == 0);
```

Then populate a complete snapshot and assert exact representative fields:

```c
s_health_snapshot.ready = true;
s_health_snapshot.sequence = 7;
s_health_snapshot.state = HEALTH_STATE_HEALTHY;
s_health_snapshot.sampled_us = 12000000;
s_health_snapshot.uptime_us = 11000000;
s_health_snapshot.inference_age_us = 250000;
s_health_snapshot.inference.attempt_count = 5;
s_health_snapshot.inference.success_count = 4;
s_health_snapshot.inference.failure_count = 1;
s_health_snapshot.inference.last_duration_us = 132000;
s_health_snapshot.inference.max_duration_us = 149000;
s_health_snapshot.internal_free_bytes = 123456;
s_health_snapshot.internal_minimum_free_bytes = 120000;
s_health_snapshot.internal_largest_free_block_bytes = 65536;
s_health_snapshot.psram_free_bytes = 654321;
s_health_snapshot.psram_minimum_free_bytes = 640000;
s_health_snapshot.psram_largest_free_block_bytes = 524288;
s_health_result = ESP_OK;
```

Verify state, reason flags, millisecond conversions, counters, stack fields,
and all memory values appear in valid JSON.

- [ ] **Step 2: Run the focused HTTP test and verify the red state**

Run:

```bash
python3 -m unittest \
  tests.test_http_capture_component.HttpCaptureComponentBehaviorTest.test_routes_capture_ownership_and_preview_controls \
  -v
```

Expected: compile failure for the missing `health.h` include/dependency or an
assertion failure because `/api/health` is not registered.

- [ ] **Step 3: Register and implement `/api/health`**

Add `HEALTH` to the HTTP component dependency list and include `health.h`.
Implement a handler with a fixed local buffer and no dynamic allocation:

```c
static esp_err_t health_get_handler(httpd_req_t *request)
{
    health_snapshot_t snapshot = {0};
    const esp_err_t err = health_get_snapshot(&snapshot);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    if (err == ESP_ERR_NOT_FOUND) {
        return httpd_resp_sendstr(request, "{\"ready\":false}");
    }
    if (err != ESP_OK || !snapshot.ready) {
        return httpd_resp_send_err(request,
                                   HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Health snapshot unavailable");
    }

    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    const uint64_t sample_age_ms =
        now_us >= snapshot.sampled_us
            ? (now_us - snapshot.sampled_us) / 1000U
            : 0U;
    const char *const state_name = health_state_name(snapshot.state);
    if (state_name == NULL) {
        return httpd_resp_send_err(request,
                                   HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Health snapshot has invalid state");
    }
    char response[1536];
    const int length = snprintf(
        response,
        sizeof(response),
        "{\"ready\":true,\"sequence\":%" PRIu32
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
        (int)snapshot.inference.last_error,
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
```

Register this `httpd_uri_t` after `/api/status` and before inference metadata.
The existing `max_uri_handlers = 8` remains sufficient for six control routes.

- [ ] **Step 4: Verify normal, not-ready, and error responses**

Add assertions for:

- `ESP_ERR_NOT_FOUND` returning 200 and `{"ready":false}`;
- a complete snapshot returning 200, `application/json`, and `no-store`;
- `ESP_FAIL` returning HTTP 500;
- a snapshot with `ready == false` returning HTTP 500;
- bounded formatting with the existing request-buffer test machinery.

Run:

```bash
python3 -m unittest tests.test_http_capture_component -v
```

Expected: all HTTP and dashboard tests pass; the dashboard HTML remains
unchanged.

- [ ] **Step 5: Commit Task 3**

Run `git diff --check`, then stage only:

```bash
git add \
  components/HTTP_CAPTURE/CMakeLists.txt \
  components/HTTP_CAPTURE/http_capture.c \
  tests/host/http_capture_component_test.c \
  tests/test_http_capture_component.py
git commit -m "feat: expose runtime health api"
```

---

### Task 4: Wire Health Into Application Startup

**Files:**
- Modify: `main/CMakeLists.txt`
- Modify: `main/main.c`
- Modify: `tests/host/http_image_transfer_integration_test.c`
- Modify: `tests/test_http_image_transfer_integration.py`

**Interfaces:**
- Consumes: `health_start()` from Task 2 and existing component start functions.
- Produces: fail-fast startup order `NVS -> CAMERA -> WIFIAP -> INFERENCE -> HEALTH -> HTTP_CAPTURE`.

- [ ] **Step 1: Add a failing startup-order scenario**

Add `components/HEALTH/include` to the integration test compile command. Extend
the call enum and stub:

```c
CALL_INFERENCE_START,
CALL_HEALTH_START,
CALL_HTTP_START,

static esp_err_t s_health_result = ESP_OK;

esp_err_t health_start(void)
{
    record(CALL_HEALTH_START);
    return s_health_result;
}
```

Require success order:

```c
const enum call_id expected[] = {
    CALL_NVS_INIT,
    CALL_CAMERA_INIT,
    CALL_WIFI_INIT,
    CALL_INFERENCE_START,
    CALL_HEALTH_START,
    CALL_HTTP_START,
};
```

Add `health-failure`: inference succeeds, health returns `ESP_FAIL`, and HTTP
must not start.

- [ ] **Step 2: Run the startup test and verify the red state**

Run:

```bash
python3 -m unittest tests.test_http_image_transfer_integration -v
```

Expected: failure because `main.c` does not include or call `health_start()`.

- [ ] **Step 3: Add the component dependency and fail-fast startup call**

Add `HEALTH` to `main/CMakeLists.txt`, include `health.h`, and insert:

```c
err = health_start();
if (err != ESP_OK) {
    ESP_LOGE(TAG,
             "Health monitor startup failed: %s",
             esp_err_to_name(err));
    return;
}
```

Place it after successful `inference_start()` and before
`http_capture_start()`.

- [ ] **Step 4: Run startup and cross-component regressions**

Run:

```bash
python3 -m unittest \
  tests.test_http_image_transfer_integration \
  tests.test_health_component \
  tests.test_inference_component \
  tests.test_http_capture_component \
  -v
```

Expected: all focused tests pass and each start failure stops later components.

- [ ] **Step 5: Commit Task 4**

Run `git diff --check`, then stage only:

```bash
git add \
  main/CMakeLists.txt \
  main/main.c \
  tests/host/http_image_transfer_integration_test.c \
  tests/test_http_image_transfer_integration.py
git commit -m "feat: start runtime health monitor"
```

---

### Task 5: Document and Verify the Software Increment

**Files:**
- Modify: `README.md`
- Modify: `README.zh-CN.md`

**Interfaces:**
- Consumes: final task/API names and JSON fields from Tasks 1–4.
- Produces: bilingual operator documentation and fresh host/build evidence.

- [ ] **Step 1: Update the English project guide**

Add a concise `Runtime health monitoring` subsection that states:

```text
The unpinned `runtime_health` task runs once per second at priority 1. It
observes inference progress, execution time, failure counters, task-stack
high-water marks, and internal/PSRAM heap values. A result older than six
seconds or three consecutive inference failures marks the snapshot degraded.
The first version reports and logs state transitions but does not reset the
device or restart tasks.
```

Add `GET /api/health` to the API table and show:

```bash
curl http://192.168.4.1/api/health
```

Explicitly distinguish freshness from a hard real-time Deadline.

- [ ] **Step 2: Update the Chinese project guide with equivalent facts**

Document the same constants, monitored values, endpoint, and no-recovery
boundary. Use “结果新鲜度超时” rather than “Deadline miss” or “硬实时违约”.

- [ ] **Step 3: Run fresh host and source checks**

Run:

```bash
python3 -m unittest discover -s tests -v
python3 -m py_compile dataset_capture.py dataset_capture_server.py
git diff --check
```

Expected: the complete suite passes with zero failures, both Python files
compile, and `git diff --check` emits no output.

- [ ] **Step 4: Build the ESP32-S3 firmware**

With ESP-IDF 5.5.4 activated, run:

```bash
idf.py build
```

Expected: build succeeds, the application fits the existing 4 MB factory
partition, and no undefined `HEALTH`, heap-capability, or FreeRTOS symbols
remain.

- [ ] **Step 5: Inspect final scope before committing docs**

Run:

```bash
git status --short
git diff --stat
git diff -- README.md README.zh-CN.md
```

Expected: implementation files are already committed, only the two README
files are staged for this task, and the pre-existing `.gitignore` modification
remains unstaged.

- [ ] **Step 6: Commit Task 5**

```bash
git add README.md README.zh-CN.md
git commit -m "docs: explain runtime health monitoring"
```

---

### Task 6: Perform Hardware Acceptance and Update Local Handoff

**Files:**
- Modify locally: `docs/EdgeDeploy项目进度.local.md`
- Modify locally: `docs/EdgeDeploy重构开发方案.md`
- Optionally create after acceptance: `docs/acceptance/v0.3.0-runtime-health-monitor.md`

**Interfaces:**
- Consumes: firmware produced by Task 5 and `/api/health`.
- Produces: hardware evidence and accurate project-status claims; no feature-code changes.

- [ ] **Step 1: Flash and confirm normal startup**

Run with the board port substituted:

```bash
idf.py -p /dev/cu.YOUR_PORT flash monitor
```

Confirm serial startup order includes inference and health before the final
dashboard-ready message, with no watchdog warning or repeated health warning.

- [ ] **Step 2: Observe the health endpoint under normal load**

Connect a browser to the dashboard and keep MJPEG preview plus inference polling
active. In another terminal sample:

```bash
while true; do
  curl -s http://192.168.4.1/api/health
  sleep 5
done
```

Observe at least 30 minutes and retain representative responses. Acceptance
requires:

- state reaches and remains `healthy` during normal operation;
- attempt and success counters continue increasing;
- failure and consecutive-failure counters remain explainable;
- last and maximum inference durations are nonzero;
- both task stack high-water marks remain nonzero;
- current/minimum internal heap and PSRAM stabilize;
- largest free blocks do not show continuous one-way decline;
- no one-second log spam occurs.

- [ ] **Step 3: Exercise non-destructive degradation and recovery**

Use only an existing reversible condition that can temporarily prevent a
successful inference, such as sustained camera contention through supported
HTTP behavior. Do not corrupt pointers, starve a core, or alter watchdogs.

Confirm that three consecutive failures or a result age over six seconds
produces one `degraded` transition warning and corresponding reason flag, then
one `healthy` recovery log after a successful fresh inference. If normal
supported operation cannot reliably create this condition, record the gap
instead of inventing a destructive injection.

- [ ] **Step 4: Update the local status documents with observed facts**

Record the exact branch/HEAD, test count, build result, board configuration,
observation duration, endpoint samples, stack minima, memory minima, maximum
inference duration, state transitions, and any unverified condition. Change
health-monitor status to verified only for behavior actually observed.

Do not stage the two locally excluded handoff documents.

- [ ] **Step 5: Create a tracked acceptance record only after acceptance**

If all required hardware checks pass, create
`docs/acceptance/v0.3.0-runtime-health-monitor.md` containing the exact commands,
HEAD, hardware, results, raw-evidence locations, and limitations. Run
`git diff --check`, stage only that record, and commit:

```bash
git add docs/acceptance/v0.3.0-runtime-health-monitor.md
git commit -m "docs: record runtime health acceptance"
```

If hardware acceptance has a failure or missing evidence, do not create the
acceptance commit or version tag; leave the feature status as software-verified
and record the blocker locally.

## Final Verification Checklist

- [ ] `python3 -m unittest discover -s tests -v` passes with zero failures.
- [ ] `python3 -m py_compile dataset_capture.py dataset_capture_server.py` succeeds.
- [ ] `idf.py build` succeeds under ESP-IDF 5.5.4.
- [ ] `git diff --check` prints no errors.
- [ ] Existing capture, stream, inference, dashboard, and dataset tests remain green.
- [ ] `/api/health` returns bounded valid JSON and never triggers sampling or recovery.
- [ ] Normal hardware operation remains `healthy` for at least 30 minutes.
- [ ] No automatic-reset, hard-Deadline, queue-monitoring, or watchdog-subscription claim is added.
- [ ] The user-owned `.gitignore` change remains untouched and unstaged.

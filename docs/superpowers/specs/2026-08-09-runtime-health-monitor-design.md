# Runtime Health Monitor Design

## Goal

Add a small, application-owned health monitor that answers three questions:

1. Is periodic inference still making progress?
2. Are inference failures or execution time getting worse?
3. Are task-stack and heap resources approaching exhaustion?

The first version detects, records, exposes, and logs health changes. It does
not restart tasks, reboot the device, persist logs to flash, inject faults, or
change watchdog configuration.

## Existing Constraints

- `ei_inference` is the only application-created long-running task. It runs at
  priority 5 on CPU1 and attempts inference after each two-second delay.
- Camera ownership is serialized by the `CAMERA` mutex. There is no
  application-level capture queue or frame-processing queue.
- Wi-Fi, HTTP, and camera-driver tasks are owned by ESP-IDF and third-party
  components. The application must not pretend to receive heartbeats from
  tasks it does not own.
- The current `/api/status` route reports camera and stream state plus current
  free heap and PSRAM. It does not report inference progress, minimum free
  memory, largest free blocks, or task stack high-water marks.
- ESP-IDF currently enables the interrupt watchdog and a five-second Task
  Watchdog. The Task Watchdog automatically monitors the two idle tasks, but
  the inference task is not explicitly subscribed.
- The project has no formal release/deadline model. Inference uses
  `vTaskDelay`, so this feature must describe a stale result as a freshness
  violation, not as a proven real-time deadline miss.

## Approaches Considered

### Extend the existing status handler only

The HTTP handler could calculate memory data and derive inference age whenever
a client requests `/api/status`. This adds no task, but health exists only while
a client polls. It also cannot independently notice and log a transition when
inference stops making progress.

### Add a minimal health monitor task

The inference component publishes fixed-size runtime statistics. A low-priority
health task samples those statistics and system-resource probes once per
second, derives a state, records a consistent snapshot, and logs only state
transitions. HTTP reads the snapshot through `/api/health`.

This is the selected approach because it adds active liveness detection without
introducing recovery behavior or a general-purpose supervision framework.

### Add a full supervisor and automatic recovery

A supervisor could subscribe tasks to watchdogs, persist fault records, restart
components, and reboot on policy violations. This requires recovery semantics,
flash-wear rules, false-positive validation, and failure injection. Those are
not justified by a demonstrated current failure and are deferred.

## Scope

### Included

- Per-attempt inference start, finish, result, duration, and counters.
- Last-success time and result freshness.
- Current and maximum inference execution time.
- Consecutive and total inference failure counts plus the latest `esp_err_t`.
- Inference-task and health-task stack high-water marks.
- Current and minimum free internal heap and PSRAM.
- Largest current free internal-memory and PSRAM blocks.
- A versioned, mutex-protected health snapshot.
- A read-only `/api/health` JSON endpoint.
- Serial logs when the derived health state changes.

### Excluded

- Application queue metrics, because the current application has no queues.
- Heartbeats from ESP-IDF-owned Wi-Fi, HTTP, or camera-driver tasks.
- Trace hooks, CPU utilization, task migration, ISR latency, or strict deadline
  accounting.
- Watchdog subscription, task restart, component restart, or device reboot.
- Persistent fault logs, coredumps, reset-history storage, or fault injection.
- New dashboard UI. The endpoint is initially inspected through a browser or
  `curl`; UI work can be a separate increment if it proves useful.

## Components and Interfaces

### Inference runtime statistics

Extend the `INFERENCE` component with a fixed-size runtime snapshot separate
from classification result metadata:

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
```

Public access is read-only:

```c
esp_err_t inference_get_runtime_stats(inference_runtime_stats_t *stats);
```

The inference component retains its own task handle and converts the
FreeRTOS stack high-water result to bytes according to ESP-IDF semantics. It
does not expose `TaskHandle_t` through the component API.

Runtime fields are protected by a dedicated short critical section, not the
classification snapshot mutex. No image copy, classifier call, formatting, or
log operation occurs while that critical section is held.

Every call to `inference_run_once()` after lifecycle validation performs these
updates:

1. Record attempt start and set `attempt_running`.
2. Run the existing capture, validation, decode, resize, classifier, and
   publication flow.
3. On every exit path, record finish time, duration, result, counters, and
   `attempt_running = false`.
4. Update `last_success_us` and clear consecutive failures only after the
   inference snapshot has been published successfully.

The existing classification metadata and sequence behavior remain unchanged.

### Health component

Create a `HEALTH` ESP-IDF component with a C-compatible API:

```c
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

`health_get_snapshot()` returns `ESP_ERR_NOT_FOUND` until the first sample has
been published. Null output pointers return `ESP_ERR_INVALID_ARG`.

`health_start()` creates exactly one unpinned FreeRTOS task. The initial
configuration is:

- sample interval: 1,000 ms;
- priority: 1, below the inference task;
- stack allocation: 4,096 bytes;
- no core affinity;
- no periodic info log.

The task stores its own handle for stack-watermark sampling. It reads inference
statistics and heap-capability APIs, derives the state, and replaces a
mutex-protected health snapshot. HTTP readers copy only the fixed-size snapshot
while the mutex is held.

### Application startup

Start components in this order:

```text
NVS -> CAMERA -> WIFIAP -> INFERENCE -> HEALTH -> HTTP_CAPTURE
```

Health depends on a successfully created inference task. HTTP starts last so
the health endpoint is available immediately after the dashboard becomes
reachable. A health allocation or task-creation failure follows the existing
fail-fast startup behavior and prevents the final ready log.

## Health-State Rules

The state machine is deliberately small:

```text
STARTING -- first successful inference --> HEALTHY
STARTING -- startup grace expires ------> DEGRADED
HEALTHY  -- stale/failure rule ----------> DEGRADED
DEGRADED -- next successful fresh result -> HEALTHY
```

Initial constants:

- startup grace: 7,000 ms after the health task starts;
- stale-result threshold: 6,000 ms since the last successful inference;
- consecutive-failure threshold: 3 attempts.

`DEGRADED` reason flags are independent and may be combined:

- no successful inference before startup grace expired;
- last successful inference is stale;
- three or more consecutive inference attempts failed;
- inference runtime statistics are unavailable.

Memory and stack values are observational in this first version. They are not
used to mark the device degraded until real board baselines justify explicit
thresholds. This avoids inventing arbitrary limits for a particular board and
model.

The health task logs one warning when entering `DEGRADED`, including the reason
flags and latest inference error. It logs one informational message when
recovering to `HEALTHY`. It does not repeat the same warning every second.

## `/api/health`

Register a `GET /api/health` endpoint on the main HTTP server. It returns
`Cache-Control: no-store` and a bounded JSON object.

Before the first health sample:

```json
{"ready":false}
```

After sampling, the response includes:

- health sequence, state, reason flags, uptime, and sample age;
- inference attempt/success/failure/consecutive-failure counts;
- whether an attempt is currently running;
- last error, last and maximum duration, and last-success age;
- inference and health task stack high-water marks;
- current/minimum/largest-block values for internal memory and PSRAM.

Durations and ages use milliseconds in JSON for readability. Internal storage
uses microseconds to avoid losing measurement precision. The endpoint reports
facts and state; it does not trigger a new sample or recovery action.

## Concurrency and Failure Handling

- Inference runtime writes use a dedicated short critical section.
- Health snapshot publication uses its own mutex because HTTP copies a larger
  multi-field structure.
- The health task never acquires the camera mutex or inference snapshot mutex.
- Health sampling must not block inference or perform network I/O.
- Counter overflow is allowed to wrap naturally and is documented; timestamps
  use 64-bit microseconds.
- If one sample cannot obtain inference statistics, publish a degraded snapshot
  with the statistics-unavailable reason instead of keeping a falsely healthy
  state.
- If the health snapshot mutex cannot be created or the health task cannot be
  started, release all health resources and return an error.
- This increment deliberately performs no automatic recovery. A degraded state
  therefore cannot create a reboot loop or hide a repeatable failure before it
  is understood.

## Testing

### Inference host tests

Extend the existing inference component test to verify:

- the created task handle is retained without changing its configured name,
  stack, priority, or CPU1 affinity;
- statistics are unavailable before startup and initialized after startup;
- success updates all attempt and success fields and clears consecutive
  failures;
- every existing failure scenario records its error, duration, and failure
  counters while preserving frame and buffer cleanup;
- maximum duration is monotonic;
- `attempt_running` is true during an instrumented in-progress classifier call
  and false on every exit path;
- a successful attempt after failures resets only the consecutive count.

### Health component host tests

Use host stubs for inference statistics, time, heap capabilities, task creation,
stack watermark, logging, and mutexes. Verify:

- single-start behavior and rollback after mutex/task creation failures;
- `STARTING`, first-success `HEALTHY`, grace-expired `DEGRADED`, stale-result
  `DEGRADED`, and recovery transitions;
- combined reason flags and transition-only logging;
- exact copying of resource and inference fields into a versioned snapshot;
- statistics-unavailable behavior;
- null arguments and pre-start snapshot access.

### HTTP and startup tests

Extend HTTP host tests to verify:

- `/api/health` registration and `{"ready":false}` before a snapshot;
- complete valid JSON with bounded formatting after a snapshot;
- health-state names and millisecond conversions;
- safe handling of unavailable or malformed snapshot data;
- health response-buffer overflow handling.

Extend the startup integration test to require `HEALTH` after `INFERENCE` and
before `HTTP_CAPTURE`, including fail-fast behavior.

### Verification

- Run focused inference, health, HTTP, and startup host tests.
- Run the complete Python suite.
- Run Python source compilation and whitespace checks.
- Build the ESP32-S3 firmware with ESP-IDF 5.5.4.
- On hardware, observe at least 30 minutes with the dashboard active and record:
  health state transitions, attempt/success/failure counters, maximum inference
  duration, both task stack high-water marks, current/minimum heap values, and
  largest free blocks.
- Acceptance requires normal operation to remain `HEALTHY`, counters to advance,
  stack values to stay nonzero, memory minima to stabilize, and no repeated
  transition log spam. No automatic-reset claim is made.

## Deferred Follow-up

After real hardware baselines and deliberate non-destructive failure tests are
available, evaluate a separate watchdog/recovery increment. That design must
define false-positive tolerances, watchdog ownership, reset evidence, and which
faults are safe to recover locally. It must not be enabled merely because the
monitor component exists.

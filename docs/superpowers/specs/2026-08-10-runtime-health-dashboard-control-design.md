# Runtime Health Dashboard Control Design

**Date:** 2026-08-10

**Status:** Approved for implementation

**Branch:** `feature/runtime-health-monitor`

## 1. Context

The runtime health monitor currently starts unconditionally during boot and exposes its latest snapshot at `GET /api/health`. Hardware acceptance confirmed that the existing monitor reports inference counters, execution duration, task stack high-water marks, and internal-memory/PSRAM statistics correctly.

The next iteration makes this monitor an on-demand dashboard feature. The user must be able to enable it from the existing inference page, view current values and short trends, then disable it so the board no longer runs the periodic health task. The page has enough vertical space to add a collapsible section without replacing the camera or inference result views.

## 2. Goals

- Boot with runtime health monitoring disabled.
- Let the dashboard start and stop the board-side health task at runtime.
- Stop the health task automatically when its dashboard client disappears.
- Preserve the existing camera, streaming, and inference behavior in every health state.
- Display the most useful current values and two short trend charts.
- Release the health task stack and eliminate periodic sampling work while disabled.
- Keep the control behavior safe under repeated requests, refreshes, multiple tabs, and network loss.

## 3. Non-goals

- Persisting the switch state in NVS.
- Keeping health history on the board or writing it to Flash.
- Adding FreeRTOS trace hooks, deadline accounting, fault injection, or a general telemetry framework.
- Stopping monitoring automatically merely because health becomes degraded.
- Adding a compile-time configuration switch; this iteration provides a runtime switch on the dashboard.

## 4. User Experience

The existing dashboard remains the primary page. A `Runtime health` section appears below `Latest result`.

### Disabled state

- The section is collapsed to a title, the message `健康任务未开启，网页不请求健康数据，不占用监控任务栈。`, and an off switch.
- No health polling timer runs in the browser.
- No periodic health task runs on the board.

### Starting state

- The switch is on but temporarily disabled to prevent repeated clicks.
- The section shows `Starting` until the first sample is ready.
- A failed start returns the UI to the disabled state and shows an error.

### Running state

- The header shows `Healthy` or `Degraded`.
- Six compact values are shown:
  1. inference successes / attempts;
  2. latest / maximum inference duration;
  3. consecutive inference failures;
  4. inference-task stack high-water mark;
  5. health-task stack high-water mark;
  6. current / minimum PSRAM free space.
- Two charts show the most recent 60 distinct samples:
  1. inference duration in milliseconds, including visible peaks;
  2. internal free memory and largest contiguous internal free block in KiB.
- A degraded state shows its decoded reasons below the charts.

### Stopping state

- The switch is temporarily disabled while the board acknowledges the stop.
- After acknowledgement, the browser stops polling, clears its 60-point history, and collapses the section.

The layout is responsive. Values and charts use multiple columns on desktop and one column where required on a narrow phone viewport.

## 5. Runtime State Machine

The health component has four lifecycle states:

```text
OFF --enable--> STARTING --task-created--> RUNNING
 ^                 |                         |
 |                 +--start-failed----------+
 |                                           |
 +<--task-exited-- STOPPING <--disable-------+
                         ^
                         +--10 s lease expiry-
```

- `OFF` is the boot state.
- `STARTING` serializes resource initialization and task creation.
- `RUNNING` samples once per second.
- `STOPPING` coalesces duplicate stop requests while the task is being awakened and allowed to exit safely; callers observe the same completed transition.
- Enabling an already running monitor and disabling an already stopped monitor are idempotent successes.
- Health degradation changes the snapshot state, not the lifecycle state.

## 6. Board-side Architecture

### 6.1 Health lifecycle ownership

The `HEALTH` component owns its task handle, lifecycle state, latest snapshot, stop request, and lease timestamp. Lifecycle changes are serialized. The health task is the only code that performs periodic sampling.

The task is created dynamically when monitoring is enabled. Disabling sends a wake-up/stop notification rather than deleting the task externally. The task observes the request, exits its loop, marks itself stopped, and deletes itself. This allows FreeRTOS to reclaim its dynamically allocated task stack safely.

A small always-available control/snapshot synchronization object may remain because the HTTP control path must be able to enable monitoring again. While `OFF`, it performs no periodic work and there is no health task stack.

### 6.2 Sampling and lease

- The sample period remains 1 second.
- Enabling starts a 10-second client lease.
- Every successful `GET /api/health` request while enabled refreshes the lease, including requests made before the first snapshot is ready.
- If the task observes that the lease has not been refreshed for 10 seconds, it follows the same safe exit path as an explicit stop.
- A browser tab moved to the background may therefore allow monitoring to stop. When it becomes active again, it queries the board and renders the actual state.
- Any open dashboard tab can refresh the one shared lease. Any tab can explicitly stop the one shared monitor.

### 6.3 Inference instrumentation

The existing lightweight inference runtime counters remain active because they are updated inside normal inference execution and are also useful immediately after monitoring starts. Disabling health removes the separate task and sampling work; it does not alter the inference pipeline.

## 7. HTTP API

### 7.1 Control

`POST /api/health/control`

Request:

```json
{"enabled": true}
```

or:

```json
{"enabled": false}
```

Successful responses describe the actual board state:

```json
{"enabled":true,"ready":false,"state":"starting"}
```

```json
{"enabled":false,"ready":false,"state":"off"}
```

Invalid methods or request bodies return a client error. Resource or task-creation failures return a server error after rolling the lifecycle back to `OFF`. The control path waits at most 2 seconds for an explicit stop; a timeout returns an error and the UI subsequently re-queries the actual state.

### 7.2 Snapshot and lease refresh

`GET /api/health`

When disabled:

```json
{"enabled":false,"ready":false,"state":"off"}
```

After enabling but before the first sample:

```json
{"enabled":true,"ready":false,"state":"starting"}
```

Once ready, the existing snapshot payload is preserved and gains `"enabled":true`. A successful request refreshes the lease.

## 8. Browser Data Flow

1. Page load calls `GET /api/health` once to discover the board's real state.
2. If monitoring is off, the section remains collapsed and no timer is created.
3. Enabling posts the control request, disables the switch during the request, then begins one-second polling.
4. Only a new health `sequence` appends a chart point.
5. JavaScript arrays retain at most 60 points. No health history is stored on the board.
6. Disabling posts the control request. Only after acknowledgement does the page stop polling, clear history, and collapse.
7. A refresh or foreground resume performs state discovery again.

The charts use plain browser SVG/DOM code already embedded in the firmware page. No external JavaScript library or network dependency is introduced.

## 9. Error Handling

- A start failure leaves no health task and returns the switch to off.
- A request failure displays `连接中断 / 状态未知` and retains only the last known presentation until a state query succeeds.
- Network loss requires no reliable browser unload event; the lease guarantees eventual board-side shutdown.
- A degraded health snapshot remains visible and continues renewing the lease.
- The switch is disabled during transitions, and lifecycle operations are serialized on the board to handle requests from multiple tabs safely.
- JSON rendering continues to use fixed-size buffers with overflow checks.

## 10. Verification Plan

### Host tests

- Boot/default lifecycle is `OFF`.
- Start succeeds, creates exactly one task, and reaches `RUNNING`.
- Repeated enable and disable requests are idempotent.
- Task-creation failure rolls back every start resource.
- Explicit stop wakes the task and reaches `OFF` within a bounded time.
- Lease refresh prevents expiry; 10 seconds without refresh causes exit.
- A new enable session resets the health snapshot sequence and readiness.
- Snapshot reads remain safe during start and stop transitions.
- HTTP control validates bodies and returns the documented lifecycle JSON.
- `GET /api/health` covers off, starting, ready, and error states.
- Dashboard source includes state discovery, a bounded 60-point history, duplicate-sequence filtering, and responsive health-panel elements.

### Firmware verification

- Run the complete host suite.
- Compile Python test files.
- Build the ESP32-S3 firmware and inspect binary/partition size.

### Hardware acceptance

1. Boot and confirm monitoring is off while inference remains operational.
2. Enable monitoring and confirm a first sample appears, counters advance, and both charts update.
3. Disable monitoring and confirm the task stops while inference remains operational.
4. Enable again, close the page without using the switch, wait approximately 10 seconds, then reconnect and confirm monitoring stopped automatically.
5. Exercise rapid switch interaction and a page refresh; confirm there is never more than one health task.
6. Verify the desktop and phone layouts and record exact observations in the project acceptance documentation.

## 11. Acceptance Criteria

The feature is complete when all automated tests and the ESP32-S3 build pass, the hardware checks above succeed, the two project progress/design documents describe the verified runtime behavior accurately, and no regression is observed in camera streaming or periodic inference.

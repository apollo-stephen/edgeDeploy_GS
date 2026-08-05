# Inference Web Dashboard Design

## Purpose

The ESP32-S3 page will show the existing MJPEG preview on the left and the
exact JPEG consumed by the most recent successful inference on the right.
Results beneath the images will identify the prediction, confidence, all model
labels and scores, inference timing, sequence number, and update age. Image and
result data must never be presented as if they belong to the same inference
when they do not.

This change also records the completed Edge Impulse deployment investigation
under `docs/experience/`. That document will explain each observed failure,
its root cause, the selected solution, why the solution addresses the cause,
the measured hardware outcome, the model-retraining workflow, and a future OTA
roadmap whose lowest priority is bootloader replacement.

## Current State and Constraint

The control page currently displays a camera stream produced by the HTTP
component. The inference task independently captures one frame every two
seconds, releases it after JPEG-to-RGB conversion, runs the classifier on CPU1,
and only logs the result. Therefore the live stream cannot be labeled as the
frame that produced a prediction.

The classifier now completes in 289–290 ms with 26 ms DSP time during repeated
hardware inference. The new publication path must preserve that performance,
camera ownership, CPU1 affinity, the two-second inference period, ESP-NN,
`EI_MAX_OVERFLOW_BUFFER_COUNT=256`, and the five-second task-watchdog policy.
It must add no Component Registry dependency.

## Considered Approaches

### Selected: publish a versioned inference snapshot

The inference component retains the JPEG that actually produced the latest
successful result and publishes it with versioned metadata. HTTP serves the
metadata and the matching image through separate endpoints. A requested image
sequence must equal the published sequence; otherwise HTTP rejects it and the
browser retries from fresh metadata.

This approach keeps autonomous inference independent from browsers, guarantees
the displayed pairing, and exposes a small reusable component boundary.

### Rejected: combine live preview with the latest result

This requires less memory but the result generally belongs to a different
frame than the visible MJPEG frame. It is unsuitable for visually evaluating
classification accuracy.

### Rejected: run inference from an HTTP request

This would pair the response with a frame but couples model execution to a
client connection, blocks the control server for roughly 300 ms, and stops
autonomous inference when no page is open.

## Inference Snapshot Ownership

The inference component will allocate two JPEG buffers in PSRAM in addition to
its existing RGB888 buffer. Each JPEG buffer is sized to the camera component's
explicit 8192-byte native-frame capacity.

One buffer is staging storage for the newly captured JPEG. Before releasing the
camera frame, the inference task validates its length and copies it into the
staging buffer. It then decodes and classifies as it does today. Only a fully
successful classification publishes data: while holding a FreeRTOS mutex, the
component swaps the staging and published JPEG pointers and replaces all
published metadata as one transaction. A failed capture, decode, classifier
call, or oversized JPEG leaves the previous successful snapshot unchanged.

The public `inference.h` boundary will define fixed-capacity metadata with a
runtime label count. It will not expose Edge Impulse SDK types to the HTTP
component. Capacity will be eight labels, with compile-time validation that the
exported model label count fits. Each published label has its model-provided
name and score. The record also contains:

- `ready` state;
- monotonically increasing 32-bit sequence number;
- prediction label and confidence after applying the exported threshold;
- DSP, classification, and anomaly timing in milliseconds;
- publication timestamp in milliseconds;
- JPEG length.

The current generated label spelling `recycleable` remains unchanged. Future
exports may change the label names or label count without requiring page
JavaScript changes, as long as the count does not exceed eight.

Two read APIs will be provided:

1. Copy the latest metadata into caller-owned storage.
2. Copy the published JPEG into caller-owned storage only when the caller's
   expected sequence matches the current publication.

Both APIs return explicit ESP-IDF errors for invalid arguments, not-ready
state, insufficient destination capacity, or stale sequence. The mutex protects
metadata replacement and bounded JPEG copies; an interrupt-disabling spinlock
will not be held while copying up to 8192 bytes.

## HTTP API

`HTTP_CAPTURE` will consume the inference component's public API and allocate
one 8192-byte PSRAM response buffer when the control server starts. Allocation
failure aborts HTTP startup with existing rollback semantics. The buffer is
released only after both HTTP servers have stopped successfully.

### `GET /api/inference`

Before the first successful inference, the endpoint returns HTTP 200 with:

```json
{"ready":false}
```

Once ready, it returns `Cache-Control: no-store` and a bounded JSON object:

```json
{
  "ready": true,
  "sequence": 12,
  "prediction": "recycleable",
  "confidence": 0.90234,
  "published_ms": 52825,
  "age_ms": 580,
  "jpeg_bytes": 4567,
  "timing": {"dsp_ms": 26, "classification_ms": 289, "anomaly_ms": 0},
  "scores": [
    {"label": "harmful", "value": 0.02344},
    {"label": "recycleable", "value": 0.90234},
    {"label": "wet", "value": 0.07422}
  ]
}
```

Model label strings will be JSON-escaped before formatting. Any formatting
overflow returns HTTP 500 rather than serving truncated JSON. `age_ms` is
computed by the device from its monotonic timer, so it does not mix ESP32
uptime with the browser's wall clock.

### `GET /api/inference/image?sequence=N`

The endpoint parses exactly one unsigned decimal sequence. Invalid or missing
queries return HTTP 400. No published snapshot returns HTTP 503. A sequence
that is no longer current returns HTTP 409 so the page can fetch new metadata.
A matching request returns the exact published JPEG with `image/jpeg`,
`Cache-Control: no-store`, and an `X-Inference-Sequence` response header.

## Browser Behavior

The existing page becomes a responsive dashboard. At desktop width it has two
columns: the existing 128×128 MJPEG preview on the left and a 128×128 static
inference snapshot on the right. At narrow width the panels stack vertically.
Existing pause/resume and capture controls remain available.

The page polls `/api/inference` once per second. While `ready` is false it shows
"Waiting for first inference". When a new sequence appears, it first loads the
versioned inference image with `fetch`, verifies `X-Inference-Sequence`, creates
an object URL from the JPEG blob, and only then renders the accompanying
metadata. It revokes the previous object URL after a successful replacement.
An HTTP 409 or a sequence header mismatch discards that update and lets the
next poll retry; the previous complete image-result pair remains visible.

The result section emphasizes the prediction and confidence, renders every
score returned by the API, shows DSP/classification/anomaly timings, and shows
how long ago the snapshot was published. Network errors change a status message
without clearing the last valid result. All model and server strings are
inserted with text nodes rather than HTML.

## Deployment Experience Document

Create `docs/experience/edge-impulse-esp32s3-deployment.md` as an evidence-based
project record. It will cover:

1. Model component registration, generated source collection, application
   partition sizing, and PSRAM-backed tensor/RGB storage.
2. The 13.6-second reference-kernel inference and watchdog starvation: ESP-NN
   was present in the export but disabled or not compiled; CPU affinity alone
   could isolate services but could not provide the needed acceleration.
3. Bundled ESP-NN enablement and CPU1 task placement, including why no Registry
   download was required.
4. Generated EON runtime setup failure after exhausting the default 30
   overflow-buffer bookkeeping slots, and why raising the pointer capacity to
   256 costs only a small fixed table while buffers remain demand-allocated.
5. Heap metadata corruption and `StoreProhibited` during model reset: the
   ESP-IDF 5.x S3 `ei_calloc()` path returned insufficiently aligned scratch
   memory; replacing it with 16-byte `heap_caps_aligned_calloc()` preserved
   zero-initialization and the valid normal free path.
6. Hardware acceptance: repeated DSP time of 26 ms, classification time of
   289–290 ms, valid probabilities and threshold behavior, with no allocation
   failure, heap corruption, panic, or watchdog warning in the supplied run.
   Compared with 13.6 seconds, classification is approximately 47 times faster.
7. Model upgrade procedure: preserve the component boundary, replace the Edge
   Impulse export, re-run host tests and a clean firmware build, check label
   capacity and arena/partition size, then repeat hardware accuracy, timing,
   memory, and endurance acceptance.

The final roadmap section orders future work by priority. Web inference
visibility comes first, followed by model retraining/export replacement,
application OTA, and only then bootloader OTA. Because the model is linked into
the application image, ordinary dual-slot application OTA can update both code
and model without rewriting the bootloader. The recommended OTA foundation is
an `ota_0`/`ota_1` partition table, `otadata`, signed application images,
anti-rollback/version checks, first-boot health confirmation, and automatic
rollback. Bootloader replacement remains a separately gated last-priority
capability because power loss or validation mistakes can make the device
unbootable; it will require a recovery path and hardware validation before any
implementation is authorized.

## Error Handling and Resource Limits

- Inference startup fails and rolls back all newly allocated JPEG buffers and
  the mutex if any allocation or synchronization object cannot be created.
- JPEG length is validated before copying; oversized frames are rejected and
  the last successful publication remains intact.
- Snapshot reads validate every pointer, capacity, readiness state, and
  expected sequence.
- HTTP startup and stop retain their existing partial-start cleanup guarantees.
- The JSON response is bounded and checked after every append.
- The dashboard retains its last consistent pair during transient camera,
  inference, or network failures.

## Testing and Acceptance

Inference host behavior tests will verify allocation rollback, exact JPEG
publication, metadata values, dynamic labels, threshold-derived `uncertain`,
monotonic sequence changes, stale-sequence rejection, insufficient-capacity
errors, and preservation of the previous snapshot after every inference
failure path.

HTTP host tests will verify handler registration, not-ready JSON, complete
ready JSON, JSON escaping, image headers and bytes, invalid sequence parsing,
stale-sequence conflict, PSRAM response-buffer cleanup, and existing server
rollback behavior. Page assertions will verify both panels, dynamic score
rendering, versioned image requests, safe text insertion, retry behavior, and
preservation of existing preview controls.

All host tests and an ESP-IDF 5.5.4 firmware build must pass. Hardware
acceptance requires repeated page updates where each right-hand image is the
published inference frame, the displayed scores match serial output for the
same sequence, classification remains below five seconds, the live preview
continues to recover from camera contention, and no heap, PSRAM, panic, or
watchdog errors appear.

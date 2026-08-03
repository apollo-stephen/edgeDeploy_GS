# Edge Impulse Camera Inference Design

## Goal

Run the exported Edge Impulse image-classification model on the ESP32-S3 in a
dedicated FreeRTOS task. Capture one 128x128 JPEG frame every two seconds,
classify it, and print timing plus all class probabilities to the serial log.
This first increment does not change the HTTP API or browser UI.

## Existing Constraints

- The target is ESP32-S3 with ESP-IDF 5.5.4.
- The camera component produces 128x128 JPEG frames and serializes access with
  its existing capture mutex.
- The model consumes a 128x128 grayscale image, uses an INT8 EON-compiled
  TensorFlow Lite model, and returns three classification probabilities:
  `harmful`, `recycleable`, and `wet`.
- The model tensor arena is approximately 385 KB. The board has PSRAM enabled.
- Existing HTTP preview and capture behavior must remain available.

## Chosen Approach

Use a dedicated `INFERENCE` ESP-IDF component with a C-compatible public API
and a C++ implementation. `app_main` starts the task only after the camera,
Wi-Fi AP, and HTTP capture server have initialized successfully.

Alternatives considered:

1. Run inference directly in `app_main`. This is simplest but blocks startup
   and does not provide a clean path to periodic inference.
2. Use a dedicated FreeRTOS task. This isolates inference work, keeps startup
   responsive, and makes scheduling and future API integration straightforward.
3. Run inference only from an HTTP request. This avoids background work but
   couples the first model integration to new HTTP behavior and error handling.

The dedicated task is selected.

## Components

### Edge Impulse model component

Convert `modev1` from its generic/Zephyr-oriented CMake configuration into an
ESP-IDF component. Register only the model and SDK sources required for the
Espressif port. Expose the model root as an include directory and select the
Espressif Edge Impulse port at compile time.

### Inference component

Create `components/INFERENCE` with:

- `inference_start()` to create exactly one background task.
- A task loop that captures a frame, decodes JPEG to RGB888, releases the
  camera frame promptly, runs the classifier, logs the result, and delays for
  two seconds.
- A signal callback that packs decoded RGB888 pixels into the format expected
  by the Edge Impulse image DSP. The generated DSP performs the grayscale
  conversion used during training.
- Explicit allocation and cleanup for the 128x128x3 decode buffer.

The component returns `ESP_ERR_INVALID_STATE` if started more than once and
reports allocation, capture, decode, or classifier failures without terminating
the task.

### Application startup

Keep `main.c` as C. Add the public inference header and call
`inference_start()` after the existing HTTP server starts. A task-creation
failure is logged and stops further startup completion reporting.

## Data Flow

1. Wait two seconds between inference attempts.
2. Acquire a JPEG frame through `camera_capture_frame()`.
3. Validate the frame dimensions, pixel format, buffer, and length.
4. Decode JPEG into a persistent RGB888 buffer.
5. Return the camera frame so HTTP capture can continue.
6. Expose RGB pixels through an Edge Impulse `signal_t` callback.
7. Call `run_classifier()` and log DSP, classification, and anomaly timing.
8. Log every label probability and the highest-confidence label. Treat the
   result as uncertain when the maximum probability is below 0.6.

## Concurrency and Memory

The existing camera mutex remains the sole camera-ownership mechanism. The
inference task holds it only during capture and JPEG decode; inference runs
after the camera frame is released. The task is single-instance and uses a
fixed period, preventing overlapping inference calls.

Allocate the RGB888 buffer from capability-aware heap with PSRAM allowed. The
Edge Impulse Espressif port allocates the large tensor arena through the ESP-IDF
heap allocator. Use a task stack sized for C++ inference orchestration while
keeping large buffers off the stack.

The current default single-app partition is close to capacity before adding the
model. Add a custom partition table with a 4 MB factory application partition
so the model firmware fits within the board's configured 16 MB flash.

## Error Handling

- A transient busy camera or capture failure is logged and retried next period.
- Invalid frame metadata or JPEG decode failure releases the frame and retries.
- Model errors are translated to numeric Edge Impulse error codes in the log.
- No failure path leaks the JPEG frame or RGB buffer.
- The HTTP server continues operating when an inference iteration fails.

## Testing and Acceptance

Host tests cover the lifecycle and decision logic that can be isolated from
ESP-IDF and Edge Impulse runtime dependencies: single-start behavior, class
selection at and below the 0.6 threshold, and retry-safe iteration outcomes.
Existing host and Python regression tests must continue to pass.

Build acceptance requires a successful clean ESP-IDF build for `esp32s3` with
the model linked and the application binary fitting the configured partition.
Hardware acceptance requires serial evidence of repeated inference timing and
three probabilities while the existing HTTP preview/capture endpoint remains
usable. Hardware acceptance cannot be claimed from a host build alone.

# Modev2 Inference Migration Design

## Goal

Run the new Edge Impulse deployment in `modev2` on the ESP32-S3 while keeping
camera capture, dataset collection, inference snapshots, and the browser UI at
128x128. Convert each decoded 128x128 frame to the model's 96x96 input before
classification, without changing the externally visible image.

## Existing Constraints

- The camera and HTTP capture paths produce and publish 128x128 JPEG frames.
- The training data for the new model was captured at 128x128.
- `modev2` deployment version 2 consumes a 96x96 RGB-packed image signal,
  returns the existing three labels, and uses the existing 0.6 threshold.
- The model metadata selects `EI_CLASSIFIER_RESIZE_FIT_SHORTEST`.
- The generated `modev2` tensor arena is approximately 126 KB, down from
  approximately 385 KB in `modev1`.
- The current ESP32-S3 integration relies on bundled ESP-NN sources and a
  16-byte-aligned `calloc` implementation that are not present in the raw
  `modev2` export configuration.
- `modev1` must remain in the repository as a rollback asset, but it does not
  need to participate in the active build.

## Chosen Approach

Keep the camera permanently configured for 128x128. Decode the JPEG into a
128x128 RGB888 buffer, then use the image-processing function bundled with the
new Edge Impulse SDK to produce a separate 96x96 RGB888 buffer. Only the 96x96
buffer is exposed through `ei::signal_t`; the original 128x128 JPEG remains the
image published by the inference dashboard.

Alternatives considered:

1. Use two RGB buffers. This consumes about 27 KB more PSRAM than an in-place
   conversion, but keeps capture and model data separate and makes bounds and
   failure behavior explicit.
2. Resize in place within a single 128x128 RGB buffer. This saves memory but
   couples inference correctness to in-place behavior and makes tests and
   future model changes less clear.
3. Reconfigure the camera to 96x96 for inference. This changes the input image
   distribution and field of view, conflicts with the 128x128 capture API, and
   introduces camera reconfiguration races.

The two-buffer approach is selected. The new model's smaller tensor arena more
than offsets the additional 96x96 image buffer compared with the current model.

## Model Component Migration

Treat `modev2` as a separate ESP-IDF component rather than modifying `modev1`.
Update the project-level extra component directory and the `INFERENCE`
component dependency to select `modev2`. Leave `modev1` unchanged and excluded
from the active dependency graph.

Adapt the generated `modev2` library for ESP-IDF by reapplying the minimal,
device-specific integration already proven for `modev1`:

- Register the generated Edge Impulse C++ sources, compiled model source, and
  TensorFlow Lite Micro C source through `idf_component_register`.
- Discover and compile the bundled ESP-NN C and ESP32-S3 assembly sources.
- Enable the ESP-NN and ESP32-S3 compile definitions and retain
  `EI_MAX_OVERFLOW_BUFFER_COUNT=256`.
- Disable ESP-DSP through the existing compile definition.
- Reapply the ESP32-S3 16-byte-aligned allocation behavior to the new SDK's
  Espressif port, including zero-initialized aligned allocation for `calloc`.
- Retain the narrowly scoped compiler-warning exception required by the
  generated third-party TensorFlow Lite Micro code.

Apply these changes to the new export rather than copying all modified SDK
files from `modev1`, because the generated SDK files differ between deployment
versions.

## Inference Data Flow

For each inference iteration:

1. Capture and validate one 128x128 JPEG frame through the camera component.
2. Reject a JPEG that exceeds the existing snapshot capacity.
3. Copy the original JPEG into the staging snapshot buffer.
4. Decode the JPEG into a persistent 128x128 RGB888 capture buffer.
5. Release the camera frame and its mutex before image preprocessing and model
   execution.
6. Call `ei::image::processing::resize_image_using_mode` with the camera
   dimensions, model dimensions, RGB pixel size, and
   `EI_CLASSIFIER_RESIZE_MODE` from `modev2` metadata.
7. Expose only the resulting 96x96 RGB buffer through the signal callback and
   set the signal length to `EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE`.
8. Run the classifier and construct the classification metadata.
9. Atomically publish the original 128x128 JPEG together with the new model's
   result after the complete iteration succeeds.

Because both source and destination are square, `FIT_SHORTEST` does not remove
one axis; it performs the same aspect-preserving downscale expected by the
model metadata.

## Memory and Ownership

Allocate both RGB buffers from 8-bit-capable PSRAM:

- Capture RGB buffer: `CAMERA_FRAME_WIDTH * CAMERA_FRAME_HEIGHT * 3`, currently
  49,152 bytes.
- Model RGB buffer: `EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT *
  3`, currently 27,648 bytes.

Keep the JPEG staging and published buffers unchanged. Extend the existing
central cleanup path so either partial startup allocation failure or task
creation failure releases both RGB buffers, both JPEG buffers, and the snapshot
mutex. Large image data remains off the task stack.

The signal callback reads only the model buffer. Its bounds check remains based
on `EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE`, and each accepted element maps to one
packed `0xRRGGBB` pixel from the 96x96 image.

## Compatibility Boundaries

- The camera remains `FRAMESIZE_128X128`.
- Dataset capture headers, status JSON, browser layout, and stored images remain
  128x128.
- The inference snapshot JPEG remains the original camera JPEG.
- Classification labels and the public inference snapshot structure remain
  unchanged because `modev2` still has three labels with the same names.
- Dimension checks in inference use camera constants for captured frames and
  Edge Impulse macros for model input. Logs must not hard-code either size.

## Failure Handling

- If either RGB allocation fails, startup returns `ESP_ERR_NO_MEM` after
  releasing every resource allocated earlier in the sequence.
- An invalid camera frame or oversized JPEG is released without touching the
  last published snapshot.
- JPEG decode failure skips resizing and classification.
- A nonzero resize result is logged with source and destination dimensions;
  classification is not called.
- A classifier or metadata-construction failure leaves the previous complete
  JPEG and metadata visible to HTTP clients.
- Only a fully decoded, resized, classified, and locked iteration may replace
  the published snapshot.
- The background task treats per-frame failures as transient and retries on the
  next configured period.

## Testing

Extend the host inference harness so the generated image-processing header is
represented by a controllable stub. Tests verify:

- Startup allocates 49,152-byte and 27,648-byte RGB buffers in addition to the
  existing JPEG buffers.
- Every allocation and task-creation failure rolls back all prior resources.
- A valid 128x128 JPEG is decoded into the capture buffer.
- The resize call receives 128x128 source dimensions, 96x96 destination
  dimensions, RGB pixel size, and the model resize mode.
- The classifier signal reads packed pixels from the resized model buffer and
  has exactly 9,216 elements.
- Resize failure prevents the classifier call and preserves the previous
  published snapshot.
- Successful inference publishes the original 128x128 JPEG rather than a
  converted or resized image.
- Existing decode, classifier, threshold, mutex, snapshot, and camera ownership
  scenarios continue to pass.

Update component-structure tests so active paths and dependencies name
`modev2`. Run the ESP-NN source-discovery and aligned-allocation tests against
`modev2`, and add metadata assertions for deployment version 2, 96x96 input,
three labels, INT8 quantization, and the expected arena configuration.

Regression acceptance requires the complete host/Python test suite and a clean
ESP-IDF build for ESP32-S3. Hardware acceptance requires a newly flashed device
to run repeated inference while the 128x128 dataset and inference images remain
available through HTTP. Serial output must show stable classification without
allocation errors, heap corruption, watchdog warnings, or camera contention.

## Rollout and Rollback

Implement the migration on a new branch created from current `main`, named
`feature/modev2-inference`. Do not reuse the already merged and stale
`feature/edge-impulse-inference` branch.

Keep `modev1` committed and unchanged during validation. Rollback consists of
restoring the project extra component directory and `INFERENCE` dependency to
`modev1`; the camera and public HTTP behavior require no rollback because they
do not change in this design.

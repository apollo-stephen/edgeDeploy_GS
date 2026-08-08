# Modev2 Inference Migration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Run `modev2` on the ESP32-S3 by resizing each decoded 128x128 camera frame to the model's 96x96 input while continuing to publish the original 128x128 JPEG.

**Architecture:** Keep the camera and HTTP contracts at 128x128. The inference component owns separate PSRAM-backed capture and model RGB888 buffers, uses the Edge Impulse SDK's configured resize mode to produce 96x96 input, and publishes only after decode, resize, and classification all succeed. `modev2` becomes the active ESP-IDF component while `modev1` remains unchanged for rollback.

**Tech Stack:** ESP-IDF 5.5.4, ESP32-S3, C/C++17, Edge Impulse C++ SDK, TensorFlow Lite Micro EON model, bundled ESP-NN, Python `unittest`, host C++ test harness, CMake.

## Global Constraints

- Work on `feature/modev2-inference`, created from the current `main` containing the approved design and plan commits.
- Do not modify or delete `modev1`.
- Do not stage the user's existing `.gitignore` modification.
- Camera capture, dataset images, HTTP headers, inference JPEGs, and browser display remain 128x128.
- The active model is deployment version 2 with 96x96 input, three labels, INT8 input/output, threshold 0.6, and `EI_CLASSIFIER_RESIZE_FIT_SHORTEST`.
- Use separate 49,152-byte capture RGB and 27,648-byte model RGB buffers in PSRAM.
- Reuse the `modev2` SDK resize implementation; do not add a custom interpolation algorithm.
- Retain `EI_CLASSIFIER_TFLITE_ENABLE_ESP_NN=1`, `EI_CLASSIFIER_TFLITE_ENABLE_ESP_NN_S3=1`, and `EI_MAX_OVERFLOW_BUFFER_COUNT=256`.
- Preserve the ESP32-S3 16-byte-aligned, zero-initialized `calloc` behavior.
- A failed decode, resize, classifier call, or metadata build must preserve the previous published JPEG and metadata.
- Hardware behavior is not accepted from host tests or compilation alone.

---

### Task 1: Make `modev2` the Active ESP32-S3 Model Component

**Files:**
- Modify: `CMakeLists.txt:5`
- Modify: `components/INFERENCE/CMakeLists.txt:1-6`
- Modify: `modev2/CMakeLists.txt:1-14`
- Create: `modev2/esp_nn_sources.cmake`
- Modify: `modev2/edge-impulse-sdk/porting/espressif/ei_classifier_porting.cpp:112-118`
- Modify: `tests/cmake/test_esp_nn_sources.cmake:3-10`
- Modify: `tests/test_inference_component.py:10-106`
- Add generated deployment: all currently untracked files under `modev2/`

**Interfaces:**
- Consumes: generated model root `modev2`, compiled model source `tflite-model/tflite_learn_1053141_4_compiled.cpp`, and bundled SDK/ESP-NN trees.
- Produces: ESP-IDF component `modev2`; public model headers rooted at `modev2`; aligned `ei_calloc(size_t nitems, size_t size)` for ESP32-S3.

- [ ] **Step 1: Point component tests at `modev2` and add failing active-model assertions**

In `tests/test_inference_component.py`, change the two host compile include/source paths from `modev1` to `modev2`. Add this configuration test class:

```python
class Modev2ComponentConfigurationTest(unittest.TestCase):
    def test_modev2_is_the_active_model_component(self):
        project_cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        inference_cmake = (
            ROOT / "components/INFERENCE/CMakeLists.txt"
        ).read_text(encoding="utf-8")
        self.assertIn('EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/modev2"', project_cmake)
        self.assertNotIn('EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/modev1"', project_cmake)
        self.assertIn("REQUIRES CAMERA esp32-camera modev2", inference_cmake)

    def test_modev2_metadata_matches_deployment(self):
        metadata = (
            ROOT / "modev2/model-parameters/model_metadata.h"
        ).read_text(encoding="utf-8")
        for definition in (
            "#define EI_CLASSIFIER_PROJECT_DEPLOY_VERSION     2",
            "#define EI_CLASSIFIER_INPUT_WIDTH                96",
            "#define EI_CLASSIFIER_INPUT_HEIGHT               96",
            "#define EI_CLASSIFIER_LABEL_COUNT                3",
            "#define EI_CLASSIFIER_TFLITE_INPUT_DATATYPE      EI_CLASSIFIER_DATATYPE_INT8",
            "#define EI_CLASSIFIER_TFLITE_OUTPUT_DATATYPE     EI_CLASSIFIER_DATATYPE_INT8",
            "#define EI_CLASSIFIER_RESIZE_MODE                EI_CLASSIFIER_RESIZE_FIT_SHORTEST",
        ):
            self.assertIn(definition, metadata)

    def test_modev2_idf_component_keeps_esp_nn_configuration(self):
        component = (ROOT / "modev2/CMakeLists.txt").read_text(encoding="utf-8")
        esp_nn = (ROOT / "modev2/esp_nn_sources.cmake").read_text(encoding="utf-8")
        self.assertIn("idf_component_register(", component)
        self.assertIn("${EI_ESP_NN_C_SOURCES}", component)
        self.assertIn("${EI_ESP_NN_ASM_SOURCES}", component)
        self.assertIn("EI_CLASSIFIER_TFLITE_ENABLE_ESP_NN=1", esp_nn)
        self.assertIn("EI_CLASSIFIER_TFLITE_ENABLE_ESP_NN_S3=1", esp_nn)
        self.assertIn("EI_MAX_OVERFLOW_BUFFER_COUNT=256", esp_nn)
```

In `tests/cmake/test_esp_nn_sources.cmake`, rename `MODEV1_ROOT` to `MODEV2_ROOT` and resolve both the SDK and `esp_nn_sources.cmake` from `modev2`.

- [ ] **Step 2: Run the targeted tests and confirm the expected failures**

Run:

```bash
python3 -m unittest tests.test_inference_component.Modev2ComponentConfigurationTest -v
python3 -m unittest tests.test_inference_component.InferenceComponentBehaviorTest.test_esp32s3_calloc_uses_aligned_zeroed_allocator -v
cmake -P tests/cmake/test_esp_nn_sources.cmake
```

Expected: the active-component and ESP-NN configuration assertions fail because the build still selects `modev1` and `modev2/CMakeLists.txt` is the generic export; the aligned-calloc test fails because `modev2` calls `heap_caps_calloc`.

- [ ] **Step 3: Adapt `modev2` as an ESP-IDF component and switch dependencies**

Change the project root to:

```cmake
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/modev2")
```

Change the inference dependency to:

```cmake
REQUIRES CAMERA esp32-camera modev2
```

Replace `modev2/CMakeLists.txt` with the proven ESP-IDF structure, retaining the new generated model source:

```cmake
include("${CMAKE_CURRENT_LIST_DIR}/edge-impulse-sdk/cmake/utils.cmake")

set(EI_SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/edge-impulse-sdk")
RECURSIVE_FIND_FILE(EI_CPP_SOURCES "${EI_SDK_DIR}" "*.cpp")
RECURSIVE_FIND_FILE(EI_CC_SOURCES "${EI_SDK_DIR}" "*.cc")
include("${CMAKE_CURRENT_LIST_DIR}/esp_nn_sources.cmake")

idf_component_register(
    SRCS
        ${EI_CPP_SOURCES}
        ${EI_CC_SOURCES}
        ${EI_ESP_NN_C_SOURCES}
        ${EI_ESP_NN_ASM_SOURCES}
        "${EI_SDK_DIR}/tensorflow/lite/c/common.c"
        "tflite-model/tflite_learn_1053141_4_compiled.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_timer
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC
    EIDSP_USE_ESP_DSP=0
    ${EI_ESP_NN_COMPILE_DEFINITIONS}
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=maybe-uninitialized
)
```

Create `modev2/esp_nn_sources.cmake`:

```cmake
set(EI_ESP_NN_ROOT "${EI_SDK_DIR}/porting/espressif/ESP-NN")
set(EI_ESP_NN_COMPILE_DEFINITIONS
    EI_CLASSIFIER_TFLITE_ENABLE_ESP_NN=1
    EI_CLASSIFIER_TFLITE_ENABLE_ESP_NN_S3=1
    EI_MAX_OVERFLOW_BUFFER_COUNT=256
)

RECURSIVE_FIND_FILE(EI_ESP_NN_C_SOURCES "${EI_ESP_NN_ROOT}" "*.c")
RECURSIVE_FIND_FILE(EI_ESP_NN_ASM_SOURCES "${EI_ESP_NN_ROOT}" "*.S")
```

In the ESP32-S3 branch of `ei_calloc`, replace:

```cpp
return heap_caps_calloc(nitems, size, MALLOC_CAP_DEFAULT);
```

with:

```cpp
return heap_caps_aligned_calloc(16, nitems, size, MALLOC_CAP_DEFAULT);
```

- [ ] **Step 4: Run the model-component tests and confirm they pass**

Run:

```bash
python3 -m unittest tests.test_inference_component.Modev2ComponentConfigurationTest -v
python3 -m unittest tests.test_inference_component.InferenceComponentBehaviorTest.test_esp32s3_calloc_uses_aligned_zeroed_allocator -v
python3 -m unittest tests.test_inference_component.InferenceComponentBehaviorTest.test_esp_nn_overflow_capacity_override_reaches_generated_model -v
cmake -P tests/cmake/test_esp_nn_sources.cmake
```

Expected: all tests pass and CMake reports nonzero ESP-NN C and assembly source counts under `modev2`.

- [ ] **Step 5: Commit the active model component**

Stage only the model migration and its tests:

```bash
git add CMakeLists.txt components/INFERENCE/CMakeLists.txt modev2 tests/cmake/test_esp_nn_sources.cmake tests/test_inference_component.py
git commit -m "feat: activate modev2 edge impulse model"
```

### Task 2: Resize 128x128 Capture Data to the 96x96 Model Input

**Files:**
- Modify: `components/INFERENCE/inference.cpp:7-102,179-315`
- Modify: `tests/host/include/edge-impulse-sdk/classifier/ei_run_classifier.h:7-11`
- Create: `tests/host/include/edge-impulse-sdk/dsp/image/processing.hpp`
- Modify: `tests/host/inference_component_test.cpp:24-475`
- Modify: `tests/test_inference_component.py:125-185`

**Interfaces:**
- Consumes: `CAMERA_FRAME_WIDTH`, `CAMERA_FRAME_HEIGHT`, `EI_CLASSIFIER_INPUT_WIDTH`, `EI_CLASSIFIER_INPUT_HEIGHT`, `EI_CLASSIFIER_RESIZE_MODE`, and `ei::image::processing::resize_image_using_mode(const uint8_t *, int, int, uint8_t *, int, int, int, int)`.
- Produces: capture buffer containing decoded 128x128 RGB888; model buffer containing resized 96x96 RGB888; classifier signal of exactly `EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE` packed pixels.

- [ ] **Step 1: Change host model metadata to 96x96 and declare the resize seam**

In the host classifier header, replace the input macros with:

```cpp
#define EI_CLASSIFIER_INPUT_WIDTH 96
#define EI_CLASSIFIER_INPUT_HEIGHT 96
#define EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE 9216
#define EI_CLASSIFIER_RESIZE_FIT_SHORTEST 1
#define EI_CLASSIFIER_RESIZE_MODE EI_CLASSIFIER_RESIZE_FIT_SHORTEST
```

Create `tests/host/include/edge-impulse-sdk/dsp/image/processing.hpp`:

```cpp
#pragma once

#include <stdint.h>

namespace ei { namespace image { namespace processing {
int resize_image_using_mode(const uint8_t *src_image,
                            int src_width,
                            int src_height,
                            uint8_t *dst_image,
                            int dst_width,
                            int dst_height,
                            int pixel_size_bytes,
                            int mode);
}}}
```

- [ ] **Step 2: Add failing dual-buffer and resize behavior to the host harness**

Add resize controls and an SDK stub to `tests/host/inference_component_test.cpp`:

```cpp
static int s_resize_calls;
static int s_resize_result;
static const uint8_t *s_resize_source;
static uint8_t *s_resize_destination;
static int s_resize_source_width;
static int s_resize_source_height;
static int s_resize_destination_width;
static int s_resize_destination_height;
static int s_resize_pixel_size;
static int s_resize_mode;

namespace ei { namespace image { namespace processing {
int resize_image_using_mode(const uint8_t *src_image,
                            int src_width,
                            int src_height,
                            uint8_t *dst_image,
                            int dst_width,
                            int dst_height,
                            int pixel_size_bytes,
                            int mode)
{
    ++s_resize_calls;
    s_resize_source = src_image;
    s_resize_destination = dst_image;
    s_resize_source_width = src_width;
    s_resize_source_height = src_height;
    s_resize_destination_width = dst_width;
    s_resize_destination_height = dst_height;
    s_resize_pixel_size = pixel_size_bytes;
    s_resize_mode = mode;
    if (s_resize_result != 0) {
        return s_resize_result;
    }
    memset(dst_image, 0,
           EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT * 3U);
    dst_image[0] = 0x40;
    dst_image[1] = 0x50;
    dst_image[2] = 0x60;
    return 0;
}
}}}
```

Make `fmt2rgb888` clear `CAMERA_FRAME_WIDTH * CAMERA_FRAME_HEIGHT * 3U` bytes and write `0x10, 0x20, 0x30` only to the capture buffer. In the success scenario, assert:

```cpp
assert(s_allocation_sizes.size() == 4);
assert(s_allocation_sizes[0] == CAMERA_FRAME_WIDTH * CAMERA_FRAME_HEIGHT * 3U);
assert(s_allocation_sizes[1] == EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT * 3U);
assert(s_resize_calls == 1);
assert(s_resize_source_width == CAMERA_FRAME_WIDTH);
assert(s_resize_source_height == CAMERA_FRAME_HEIGHT);
assert(s_resize_destination_width == EI_CLASSIFIER_INPUT_WIDTH);
assert(s_resize_destination_height == EI_CLASSIFIER_INPUT_HEIGHT);
assert(s_resize_pixel_size == 3);
assert(s_resize_mode == EI_CLASSIFIER_RESIZE_MODE);
assert(s_observed_pixels[0] == 0x405060);
```

Add `verify_resize_failure()` that first publishes sequence 1, sets `s_resize_result = -7`, expects `inference_run_once() == ESP_FAIL`, asserts the classifier call count did not increase, and calls `verify_snapshot_unchanged(1)`.

Update allocation scenarios for four allocations:

```python
"no-memory-capture-rgb": "inference allocation failure passed",
"no-memory-model-rgb": "inference allocation failure passed",
"no-memory-staging": "inference allocation failure passed",
"no-memory-published": "inference allocation failure passed",
"resize-failure": "inference resize failure passed",
```

Map the four failure names to allocation calls 1 through 4. Update task-failure expectations to four frees on the first attempt and eight total allocations after a successful retry.

- [ ] **Step 3: Run the host inference test and verify it fails before production changes**

Run:

```bash
python3 -m unittest tests.test_inference_component.InferenceComponentBehaviorTest.test_task_lifecycle_frame_ownership_and_classifier_bridge -v
```

Expected: the host executable fails to compile because production inference does not include or call `resize_image_using_mode`, or the new dual-buffer assertions fail.

- [ ] **Step 4: Implement separate capture/model buffers and resize before classification**

Include the SDK processing header:

```cpp
#include "edge-impulse-sdk/dsp/image/processing.hpp"
```

Replace the single RGB size/pointer with:

```cpp
constexpr size_t kCaptureRgbBufferBytes =
    CAMERA_FRAME_WIDTH * CAMERA_FRAME_HEIGHT * 3U;
constexpr size_t kModelRgbBufferBytes =
    EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT * 3U;

static_assert(EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE ==
                  EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT,
              "Image signal length must match model dimensions");

uint8_t *s_capture_rgb_buffer;
uint8_t *s_model_rgb_buffer;
```

Make `release_resources()` free and null the model buffer and then the capture buffer. In `inference_start()`, allocate the capture buffer first and the model buffer second with:

```cpp
heap_caps_malloc(buffer_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
```

Use the camera contract in frame validation:

```cpp
frame->width == CAMERA_FRAME_WIDTH &&
frame->height == CAMERA_FRAME_HEIGHT
```

Decode into `s_capture_rgb_buffer`, release the camera frame, and resize:

```cpp
const int resize_result = ei::image::processing::resize_image_using_mode(
    s_capture_rgb_buffer,
    CAMERA_FRAME_WIDTH,
    CAMERA_FRAME_HEIGHT,
    s_model_rgb_buffer,
    EI_CLASSIFIER_INPUT_WIDTH,
    EI_CLASSIFIER_INPUT_HEIGHT,
    3,
    EI_CLASSIFIER_RESIZE_MODE);
if (resize_result != 0) {
    ESP_LOGE(TAG,
             "RGB resize %ux%u to %ux%u failed: %d",
             static_cast<unsigned int>(CAMERA_FRAME_WIDTH),
             static_cast<unsigned int>(CAMERA_FRAME_HEIGHT),
             static_cast<unsigned int>(EI_CLASSIFIER_INPUT_WIDTH),
             static_cast<unsigned int>(EI_CLASSIFIER_INPUT_HEIGHT),
             resize_result);
    return ESP_FAIL;
}
```

Make `get_signal_data()` read only `s_model_rgb_buffer`. Replace the hard-coded invalid-frame log with a formatted camera-size message. Require both RGB pointers in the startup-state guard. Do not change the JPEG staging/published swap.

- [ ] **Step 5: Run the focused host test and confirm it passes**

Run:

```bash
python3 -m unittest tests.test_inference_component.InferenceComponentBehaviorTest.test_task_lifecycle_frame_ownership_and_classifier_bridge -v
```

Expected: every scenario passes, including capture-buffer allocation failure, model-buffer allocation failure, resize failure, original-JPEG publication, and existing inference behavior.

- [ ] **Step 6: Commit the preprocessing path**

```bash
git add components/INFERENCE/inference.cpp tests/host/include/edge-impulse-sdk/classifier/ei_run_classifier.h tests/host/include/edge-impulse-sdk/dsp/image/processing.hpp tests/host/inference_component_test.cpp tests/test_inference_component.py
git commit -m "feat: resize camera frames for modev2 inference"
```

### Task 3: Document and Verify the Complete Migration

**Files:**
- Modify: `README.md:34-58`
- Verify: all project sources and tests

**Interfaces:**
- Consumes: active `modev2` component and tested 128x128-to-96x96 preprocessing path.
- Produces: accurate operator documentation, full host regression evidence, and an ESP32-S3 firmware build artifact.

- [ ] **Step 1: Update inference documentation with the active model data flow**

Revise the README iteration list to state:

```markdown
1. Acquires a 128x128 JPEG frame with a 250 ms timeout.
2. Decodes the frame into a 49,152-byte RGB888 capture buffer in PSRAM.
3. Releases the camera frame, then downsizes the RGB image to a separate
   27,648-byte 96x96 model buffer using the Edge Impulse export's configured
   resize mode.
4. Converts the resized pixels to the Edge Impulse signal format and runs the
   deployment-version-2 INT8 EON model.
5. Logs DSP/classification timing and probabilities for `harmful`,
   `recycleable`, and `wet`.
6. Publishes the original 128x128 classified JPEG and versioned result metadata
   for the local dashboard.
```

Replace the old tensor-arena statement with the generated `modev2` value of approximately 126 KB. State explicitly that `modev1` remains in the repository only for rollback and is not in the active build.

- [ ] **Step 2: Run the complete host regression suite**

Run:

```bash
python3 -m unittest discover -s tests -v
```

Expected: all tests pass. Any failure must be investigated; do not weaken unrelated assertions.

- [ ] **Step 3: Run a clean ESP32-S3 build**

With ESP-IDF 5.5.4 activated, run:

```bash
idf.py fullclean
idf.py set-target esp32s3
idf.py build
```

Expected: configuration and compilation succeed, the linked component is `modev2`, and the application fits the configured 4 MB factory partition.

- [ ] **Step 4: Inspect final scope and commit documentation**

Run:

```bash
git status --short
git diff --check
git diff --stat main...HEAD
```

Expected: only the approved migration files differ; `.gitignore` remains an unstaged user modification; `modev1` has no diff.

Commit the README update:

```bash
git add README.md
git commit -m "docs: explain modev2 inference preprocessing"
```

- [ ] **Step 5: Record the hardware acceptance boundary**

Do not claim hardware acceptance without flashing. The handoff must request or report a device run that confirms repeated inference, the original 128x128 HTTP image, stable three-label output, no allocation/heap errors, no watchdog warnings, and no camera contention.

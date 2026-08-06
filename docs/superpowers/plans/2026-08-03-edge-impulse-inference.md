# Edge Impulse Camera Inference Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Link the exported Edge Impulse model into the ESP32-S3 firmware and run one camera classification every two seconds from a dedicated FreeRTOS task, logging all probabilities and timing without changing the HTTP API.

**Architecture:** Convert `modev1` into an ESP-IDF component, then add a C-compatible `INFERENCE` component whose C++ implementation owns a PSRAM RGB888 buffer and a single FreeRTOS task. Each iteration captures and decodes one mutex-protected JPEG frame, releases the camera, exposes pixels through `ei::signal_t`, runs `run_classifier()`, and logs a thresholded classification result.

**Tech Stack:** ESP-IDF 5.5.4, ESP32-S3, FreeRTOS, `espressif/esp32-camera` 2.1.7, Edge Impulse C++ SDK/EON model, Python `unittest`, host C++ tests.

## Global Constraints

- Target ESP32-S3 with ESP-IDF 5.5.4.
- Preserve existing OV5640 128x128 JPEG capture, SoftAP, MJPEG preview, and `/capture` behavior.
- Model input is 128x128 grayscale; feed packed RGB888 pixels and let the generated image DSP perform grayscale conversion.
- Model output labels remain exactly `harmful`, `recycleable`, and `wet`.
- Classification confidence threshold is exactly `0.6`.
- Inference period is exactly `2000 ms`; camera acquisition timeout is `250 ms`.
- Keep large image and tensor buffers off the FreeRTOS task stack and allow PSRAM allocation.
- Do not stage or commit the user's unrelated `.gitignore` modification.

---

### Task 1: Define and test the inference component contract

**Files:**
- Create: `tests/test_inference_component.py`
- Create: `tests/host/inference_component_test.cpp`
- Create: `tests/host/include/edge-impulse-sdk/classifier/ei_run_classifier.h`
- Modify: `tests/host/include/esp_err.h`
- Modify: `tests/host/include/esp_heap_caps.h`
- Modify: `tests/host/include/freertos/FreeRTOS.h`
- Modify: `tests/host/include/freertos/task.h`
- Create: `tests/host/include/img_converters.h`
- Create: `components/INFERENCE/include/inference.h`
- Create: `components/INFERENCE/inference.cpp`
- Create: `components/INFERENCE/CMakeLists.txt`

**Interfaces:**
- Consumes: `camera_capture_frame(uint32_t)`, `camera_release_frame(camera_fb_t *)`, `fmt2rgb888(...)`, `run_classifier(ei::signal_t *, ei_impulse_result_t *, bool)`, FreeRTOS task and ESP heap APIs.
- Produces: `esp_err_t inference_start(void)` and `esp_err_t inference_run_once(void)` with C linkage.

- [ ] **Step 1: Write the failing host behavior test**

Create a Python test that compiles `inference_component_test.cpp` together with
`components/INFERENCE/inference.cpp` using `c++ -std=c++17 -Wall -Wextra -Werror`.
The C++ test supplies real fakes for camera ownership, JPEG decoding, heap
allocation, task creation, delay, and `run_classifier()` and asserts:

```cpp
assert(inference_run_once() == ESP_ERR_INVALID_STATE);
assert(inference_start() == ESP_OK);
assert(inference_start() == ESP_ERR_INVALID_STATE);
assert(task_create_calls == 1);
assert(inference_run_once() == ESP_OK);
assert(camera_capture_calls == 1);
assert(camera_release_calls == 1);
assert(decode_calls == 1);
assert(classifier_calls == 1);
assert(observed_pixels[0] == 0x102030);
```

It must also cover invalid frame metadata, decode failure, classifier failure,
and a below-threshold result while verifying that every acquired frame is
released exactly once.

- [ ] **Step 2: Run the new test and verify RED**

Run:

```bash
python3 -m unittest tests.test_inference_component -v
```

Expected: failure because `components/INFERENCE/inference.cpp` and the public
header do not exist.

- [ ] **Step 3: Add minimal host stubs and public header**

Extend the host headers with only the declarations needed by the test:

```c
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_ARG 0x102
#define MALLOC_CAP_8BIT 0x00000004
#define pdPASS 1
typedef void (*TaskFunction_t)(void *);
int xTaskCreate(TaskFunction_t task, const char *name, uint32_t stack_depth,
                void *argument, unsigned int priority, void *task_handle);
void *heap_caps_malloc(size_t size, unsigned int capabilities);
void heap_caps_free(void *pointer);
```

The Edge Impulse stub defines the exact minimal `signal_t`, classification
result, timing, input macros, label count, threshold, and error types used by
production code. The JPEG converter stub declares the actual `fmt2rgb888`
signature.

- [ ] **Step 4: Implement the minimal inference component**

Create `inference.h`:

```c
#pragma once
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t inference_start(void);
esp_err_t inference_run_once(void);
#ifdef __cplusplus
}
#endif
```

Implement a single-instance component with these constants:

```cpp
constexpr uint32_t kCaptureTimeoutMs = 250;
constexpr uint32_t kInferencePeriodMs = 2000;
constexpr uint32_t kTaskStackBytes = 8192;
constexpr UBaseType_t kTaskPriority = 5;
constexpr size_t kRgbBufferBytes =
    EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT * 3U;
```

`inference_start()` allocates the RGB buffer with
`MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT`, creates exactly one task, and rolls back
all state on task-creation failure. `inference_run_once()` validates a 128x128
JPEG, decodes it, releases the frame before inference, packs each RGB triplet as
`0xRRGGBB` in the signal callback, calls `run_classifier()`, logs timing and all
labels, and reports `uncertain` when the best value is below 0.6. The task loop
delays with `vTaskDelay(pdMS_TO_TICKS(2000))` before each call to
`inference_run_once()`, matching the approved startup and scheduling design.

Create `components/INFERENCE/CMakeLists.txt`:

```cmake
idf_component_register(
    SRCS "inference.cpp"
    INCLUDE_DIRS "include"
    REQUIRES CAMERA esp32-camera modev1
)
```

- [ ] **Step 5: Run the test and verify GREEN**

Run:

```bash
python3 -m unittest tests.test_inference_component -v
```

Expected: all inference host behavior tests pass with no compiler warnings.

- [ ] **Step 6: Run the complete host regression suite**

Run:

```bash
python3 -m unittest discover -s tests -v
```

Expected: all existing and new tests pass.

- [ ] **Step 7: Commit the inference component behavior**

Stage only the Task 1 paths and commit:

```bash
git commit -m "feat: add periodic camera inference component"
```

---

### Task 2: Register the Edge Impulse export as an ESP-IDF component

**Files:**
- Modify: `CMakeLists.txt`
- Replace: `modev1/CMakeLists.txt`
- Create: `tests/test_edge_impulse_model_component.py`

**Interfaces:**
- Consumes: generated sources under `modev1/edge-impulse-sdk`, `modev1/model-parameters`, and `modev1/tflite-model`.
- Produces: ESP-IDF component target `modev1`, public include root `modev1`, and the linked generated EON model functions.

- [ ] **Step 1: Write the failing CMake contract test**

Create a Python test that asserts the root CMake file adds `modev1` to
`EXTRA_COMPONENT_DIRS` before including `project.cmake`, and that the model
CMake file uses `idf_component_register`, references the generated
`tflite_learn_1053141_4_compiled.cpp`, registers the Espressif port, and disables
ESP-NN for the first correctness-oriented integration:

```python
self.assertIn('EXTRA_COMPONENT_DIRS', root_cmake)
self.assertIn('idf_component_register', model_cmake)
self.assertIn('tflite_learn_1053141_4_compiled.cpp', model_cmake)
self.assertIn('EI_CLASSIFIER_TFLITE_ENABLE_ESP_NN=0', model_cmake)
```

- [ ] **Step 2: Run the model component test and verify RED**

Run:

```bash
python3 -m unittest tests.test_edge_impulse_model_component -v
```

Expected: failure because the current model CMake file expects a generic target
named `app` and includes Zephyr CMake.

- [ ] **Step 3: Convert the root and model CMake files**

In the root CMake file, add:

```cmake
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/modev1")
```

before ESP-IDF's `project.cmake` include. In `modev1/CMakeLists.txt`, use the
exported recursive-source helper to collect the same generic C++ SDK sources,
the required CMSIS DSP C sources, `tensorflow/lite/c/common.c`, the generated
model source, and the Espressif port. Register them against `${COMPONENT_LIB}`,
export `.` as the include root, require `esp_timer`, and add:

```cmake
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    EI_CLASSIFIER_TFLITE_ENABLE_ESP_NN=0
)
```

ESP-NN is deliberately disabled in this first integration so no architecture
assembly source set is needed; performance optimization is a separate change.

- [ ] **Step 4: Run the model component test and verify GREEN**

Run:

```bash
python3 -m unittest tests.test_edge_impulse_model_component -v
```

Expected: pass.

- [ ] **Step 5: Commit the model component integration**

Stage the root CMake file, all of `modev1`, and the new test, then commit:

```bash
git commit -m "build: register edge impulse model component"
```

---

### Task 3: Add the flash partition and application startup integration

**Files:**
- Create: `partitions.csv`
- Modify: `sdkconfig.defaults`
- Modify: `main/main.c`
- Modify: `main/CMakeLists.txt`
- Create: `tests/test_inference_app_integration.py`

**Interfaces:**
- Consumes: `inference_start()` from Task 1 and component target `INFERENCE`.
- Produces: 4 MB factory application partition and startup sequence `NVS -> camera -> Wi-Fi -> HTTP -> inference`.

- [ ] **Step 1: Write the failing integration contract test**

Create a Python test that asserts:

```python
self.assertIn('CONFIG_PARTITION_TABLE_CUSTOM=y', defaults)
self.assertIn('CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"', defaults)
self.assertIn('factory,  app,  factory,  0x10000,  4M', partitions)
self.assertIn('#include "inference.h"', main_source)
self.assertLess(main_source.index('http_capture_start()'),
                main_source.index('inference_start()'))
self.assertIn('INFERENCE', main_cmake)
```

- [ ] **Step 2: Run the integration test and verify RED**

Run:

```bash
python3 -m unittest tests.test_inference_app_integration -v
```

Expected: failure because no custom partition or inference startup exists.

- [ ] **Step 3: Add partition and startup configuration**

Create `partitions.csv`:

```csv
# Name,   Type, SubType, Offset,   Size, Flags
nvs,      data, nvs,     0x9000,   0x6000,
phy_init, data, phy,     0xf000,   0x1000,
factory,  app,  factory, 0x10000,  4M,
```

Add the custom-partition settings to `sdkconfig.defaults`. Add `INFERENCE` to
`main` private requirements, include `inference.h`, and start inference after
HTTP startup. Log and return if task startup fails; preserve the existing image
preview URL log on success.

- [ ] **Step 4: Run the integration test and verify GREEN**

Run:

```bash
python3 -m unittest tests.test_inference_app_integration -v
```

Expected: pass.

- [ ] **Step 5: Run all host regressions**

Run:

```bash
python3 -m unittest discover -s tests -v
```

Expected: all tests pass.

- [ ] **Step 6: Commit application integration**

Stage only the Task 3 paths and commit:

```bash
git commit -m "feat: start edge impulse inference task"
```

---

### Task 4: Build, document, and audit the completed integration

**Files:**
- Modify: `README.md`
- Modify only if generated locally: ignored `sdkconfig`

**Interfaces:**
- Consumes: complete firmware source from Tasks 1-3.
- Produces: a linked ESP32-S3 application binary, documented serial output and hardware acceptance steps.

- [ ] **Step 1: Configure the local ignored sdkconfig for the custom partition**

Use `apply_patch` on the ignored local `sdkconfig` so the current build selects:

```text
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
```

Do not stage `sdkconfig`.

- [ ] **Step 2: Activate ESP-IDF and build**

Run:

```bash
source /Users/stephenapollo/.espressif/v5.5.4/esp-idf/export.sh
idf.py reconfigure
idf.py build
```

Expected: target `esp32s3`, model sources compiled, application linked, and the
binary fits the 4 MB factory partition. If compilation exposes an SDK source
selection error, change only `modev1/CMakeLists.txt`, add a contract assertion
when applicable, and repeat the failing/passing build cycle.

- [ ] **Step 3: Run the entire host suite again**

Run:

```bash
python3 -m unittest discover -s tests -v
```

Expected: all tests pass with no regressions.

- [ ] **Step 4: Document use and honest acceptance boundaries**

Update `README.md` with the two-second inference task, the three serial labels,
confidence threshold, expected timing log shape, build command, and the fact
that browser preview plus hardware inference coexistence still requires board
verification. Do not claim hardware success from the build.

- [ ] **Step 5: Verify documentation and source consistency**

Run:

```bash
rg -n "2000|0\.6|harmful|recycleable|wet|inference_start" README.md components main
git diff --check
git status --short
```

Expected: constants and documentation agree; no whitespace errors; the only
unstaged unrelated change remains the user's pre-existing `.gitignore` edit.

- [ ] **Step 6: Commit documentation and any build-derived CMake correction**

Stage only intended tracked files and commit:

```bash
git commit -m "docs: describe periodic edge inference"
```

- [ ] **Step 7: Final verification**

Run fresh:

```bash
source /Users/stephenapollo/.espressif/v5.5.4/esp-idf/export.sh
idf.py build
python3 -m unittest discover -s tests -v
git diff --check
git status --short
```

Expected: firmware build passes, all host tests pass, no whitespace errors, and
no intended feature changes remain uncommitted. Report that physical flash,
serial inference, probability quality, and concurrent browser preview remain
hardware acceptance steps unless a connected board is available.

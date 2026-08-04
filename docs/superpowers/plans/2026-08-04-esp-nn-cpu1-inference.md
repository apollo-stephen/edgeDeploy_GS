# ESP-NN CPU1 Inference Fix Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the 13.6-second reference convolution with the Edge Impulse export's bundled ESP-NN implementation and isolate inference on CPU1.

**Architecture:** Keep the existing `INFERENCE` and `modev1` component boundaries. The inference component changes only task affinity; the model component adds the bundled ESP-NN C/ESP32-S3 assembly sources and lets the Edge Impulse target detection enable its optimized kernels.

**Tech Stack:** ESP-IDF 5.5.4, ESP32-S3 dual-core FreeRTOS, Edge Impulse SDK 1.94.2, bundled ESP-NN, Python `unittest`, host C++17 test executable.

## Global Constraints

- Use the ESP-NN sources already under `modev1/edge-impulse-sdk/porting/espressif/ESP-NN`; add no Registry dependency.
- Pin `ei_inference` to CPU1, core ID `1`.
- Keep the five-second task-watchdog configuration unchanged.
- Preserve camera ownership, task priority `5`, stack size `8192`, capture timeout `250 ms`, and post-iteration delay `2000 ms`.
- Preserve the user's unrelated `.gitignore` modification without staging it.

---

### Task 1: Pin inference to CPU1

**Files:**
- Modify: `tests/host/include/freertos/task.h`
- Modify: `tests/host/inference_component_test.cpp`
- Modify: `components/INFERENCE/inference.cpp:121-126`

**Interfaces:**
- Consumes: ESP-IDF `xTaskCreatePinnedToCore(TaskFunction_t, const char *, configSTACK_DEPTH_TYPE, void *, UBaseType_t, TaskHandle_t *, BaseType_t)`.
- Produces: one `ei_inference` task created with core ID `1` and the existing lifecycle behavior.

- [ ] **Step 1: Write the failing host behavior test**

Replace the host task double with `xTaskCreatePinnedToCore`, capture its
`core_id`, and assert in `verify_start_success()` that the value is exactly
`1`. Keep the existing name, stack, priority, and failure assertions.

- [ ] **Step 2: Run the targeted test and verify RED**

Run:

```bash
python3 -m unittest tests.test_inference_component.InferenceComponentBehaviorTest.test_task_lifecycle_frame_ownership_and_classifier_bridge -v
```

Expected: compilation fails because `inference.cpp` still calls `xTaskCreate`.

- [ ] **Step 3: Implement CPU1 task affinity**

Change `inference_start()` to call:

```cpp
xTaskCreatePinnedToCore(inference_task,
                        "ei_inference",
                        kTaskStackBytes,
                        nullptr,
                        kTaskPriority,
                        nullptr,
                        1);
```

- [ ] **Step 4: Run the targeted test and verify GREEN**

Run the command from Step 2. Expected: pass for every existing lifecycle and inference scenario.

---

### Task 2: Compile and enable bundled ESP-NN

**Files:**
- Create: `modev1/esp_nn_sources.cmake`
- Create: `tests/cmake/test_esp_nn_sources.cmake`
- Modify: `tests/test_inference_component.py`
- Modify: `modev1/CMakeLists.txt`

**Interfaces:**
- Consumes: `RECURSIVE_FIND_FILE` from `edge-impulse-sdk/cmake/utils.cmake` and the bundled `ESP-NN/src` tree.
- Produces: `EI_ESP_NN_C_SOURCES` and `EI_ESP_NN_ASM_SOURCES`, both passed to `idf_component_register`; Edge Impulse target detection defines ESP-NN for ESP32-S3.

- [ ] **Step 1: Write the failing source-discovery test**

Add a Python test that executes:

```bash
cmake -P tests/cmake/test_esp_nn_sources.cmake
```

The CMake test includes the production source collector and fails unless it
finds at least one `.c` file, at least one `.S` file, the S3 convolution
implementation, and the common S3 assembly implementation. It also rejects any
source outside the bundled `porting/espressif/ESP-NN` directory.

- [ ] **Step 2: Run the source-discovery test and verify RED**

Run:

```bash
python3 -m unittest tests.test_inference_component.InferenceComponentBehaviorTest.test_bundled_esp_nn_sources_are_discovered -v
```

Expected: fail because `modev1/esp_nn_sources.cmake` does not exist.

- [ ] **Step 3: Implement the ESP-NN source collector and component integration**

Create `modev1/esp_nn_sources.cmake` to collect `*.c` and `*.S` beneath the
bundled ESP-NN directory. Include it from `modev1/CMakeLists.txt`, append both
lists to `SRCS`, and remove the explicit
`EI_CLASSIFIER_TFLITE_ENABLE_ESP_NN=0` definition. Keep
`EIDSP_USE_ESP_DSP=0` and the third-party GCC warning exception unchanged.

- [ ] **Step 4: Run the targeted source-discovery test and verify GREEN**

Run the command from Step 2. Expected: pass.

- [ ] **Step 5: Run the entire host suite**

Run:

```bash
python3 -m unittest discover -s tests -v
```

Expected: all tests pass.

- [ ] **Step 6: Run a clean ESP-IDF build**

Activate ESP-IDF 5.5.4 and build from a new temporary build directory and a
blank temporary `sdkconfig`. Expected: ESP-NN `.c` and `.S` compilation is
visible, all ESP-NN symbols link, and the application fits the four-megabyte
factory partition.

- [ ] **Step 7: Audit and commit**

Run `git diff --check` and `git status --short`. Stage only the CPU1, ESP-NN,
test, and documentation paths; leave `.gitignore` unstaged. Commit with:

```bash
git commit -m "fix: accelerate inference with esp-nn"
```

- [ ] **Step 8: Push and verify PR #3**

Push `feature/edge-impulse-inference`, then verify PR #3 still targets `main`
and includes the new commit. Physical classification time and watchdog-free
operation remain post-flash acceptance checks.

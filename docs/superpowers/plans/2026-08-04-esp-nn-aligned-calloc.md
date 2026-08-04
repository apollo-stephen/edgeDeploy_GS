# ESP-NN Aligned Calloc Fix Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prevent ESP-NN scratch-buffer heap corruption by making the ESP32-S3 ESP-IDF 5.x `ei_calloc()` path return 16-byte-aligned, zero-initialized memory.

**Architecture:** Compile the real Edge Impulse Espressif porting implementation in a host regression test against controlled ESP-IDF heap doubles. Change only the ESP32-S3/ESP-IDF 5.x calloc branch to use ESP-IDF's aligned calloc API; keep the existing CPU1 task placement, bundled ESP-NN integration, overflow-buffer capacity, PSRAM policy, and free path unchanged.

**Tech Stack:** ESP-IDF 5.5.4, ESP32-S3, Edge Impulse SDK 1.94.2, C++17, Python `unittest`.

## Global Constraints

- Use `heap_caps_aligned_calloc(16, nitems, size, MALLOC_CAP_DEFAULT)` for ESP32-S3 on ESP-IDF 5.x.
- Preserve calloc zero-initialization and the existing `ei_free()` implementation.
- Keep `EI_MAX_OVERFLOW_BUFFER_COUNT=256` and the bundled ESP-NN sources enabled.
- Keep inference pinned to CPU1 and leave the five-second task-watchdog configuration unchanged.
- Add no ESP Component Registry dependency.
- Preserve the user's unrelated `.gitignore` modification without staging it.

---

### Task 1: Align Edge Impulse calloc buffers on ESP32-S3

**Files:**
- Create: `tests/host/include/esp_idf_version.h`
- Create: `tests/host/ei_classifier_porting_test.cpp`
- Modify: `tests/host/include/esp_heap_caps.h`
- Modify: `tests/host/include/esp_timer.h`
- Modify: `tests/host/include/freertos/FreeRTOS.h`
- Modify: `tests/test_inference_component.py`
- Modify: `modev1/edge-impulse-sdk/porting/espressif/ei_classifier_porting.cpp:111-115`

**Interfaces:**
- Consumes: `void *heap_caps_aligned_calloc(size_t alignment, size_t n, size_t size, uint32_t caps)` from ESP-IDF 5.5.4.
- Produces: `void *ei_calloc(size_t nitems, size_t size)` returning zeroed memory aligned to 16 bytes on ESP32-S3.

- [ ] **Step 1: Add ESP-IDF host declarations and version definitions**

Create `tests/host/include/esp_idf_version.h`:

```c
#pragma once

#define ESP_IDF_VERSION_VAL(major, minor, patch) \
    (((major) << 16) | ((minor) << 8) | (patch))
#define ESP_IDF_VERSION ESP_IDF_VERSION_VAL(5, 5, 4)
```

Extend `tests/host/include/esp_heap_caps.h` with `#include <stdint.h>`, `MALLOC_CAP_DEFAULT`, and these declarations inside its existing `extern "C"` block:

```c
#define MALLOC_CAP_DEFAULT (1U << 12)

void *heap_caps_aligned_alloc(size_t alignment,
                              size_t size,
                              uint32_t capabilities);
void *heap_caps_aligned_calloc(size_t alignment,
                               size_t n,
                               size_t size,
                               uint32_t capabilities);
void *heap_caps_calloc(size_t n,
                       size_t size,
                       uint32_t capabilities);
```

Add the IDF 5 timing compatibility constant to `tests/host/include/freertos/FreeRTOS.h`:

```c
#define portTICK_PERIOD_MS 1
```

Wrap the declaration in `tests/host/include/esp_timer.h` in an `extern "C"`
block when compiling as C++ so the host double matches ESP-IDF linkage.

- [ ] **Step 2: Write the real-porting-layer regression harness**

Create `tests/host/ei_classifier_porting_test.cpp`. Provide C-linkage doubles for `vTaskDelay`, `esp_timer_get_time`, `heap_caps_aligned_alloc`, `heap_caps_aligned_calloc`, `heap_caps_calloc`, `heap_caps_malloc`, `heap_caps_free`, and `heap_caps_get_free_size`. The aligned doubles must allocate with `posix_memalign`; the aligned-calloc double must zero the allocation and record its alignment, item count, item size, and capabilities. The ordinary calloc double must increment a separate call counter.

In `main()`, call:

```cpp
constexpr size_t kItems = 9;
constexpr size_t kItemSize = 64;
void *allocation = ei_calloc(kItems, kItemSize);
assert(allocation != nullptr);
assert(aligned_calloc_calls == 1);
assert(ordinary_calloc_calls == 0);
assert(last_alignment == 16);
assert(last_nitems == kItems);
assert(last_item_size == kItemSize);
assert(last_capabilities == MALLOC_CAP_DEFAULT);
assert(reinterpret_cast<uintptr_t>(allocation) % 16 == 0);

const auto *bytes = static_cast<const unsigned char *>(allocation);
for (size_t index = 0; index < kItems * kItemSize; ++index) {
    assert(bytes[index] == 0);
}

ei_free(allocation);
```

Print `ei calloc alignment behavior passed` after the free completes.

- [ ] **Step 3: Add the Python compile-and-run test**

Add `test_esp32s3_calloc_uses_aligned_zeroed_allocator()` to `tests/test_inference_component.py`. Compile the harness together with the real production file using:

```python
[
    "c++",
    "-std=c++17",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-Wno-format-security",
    "-DCONFIG_IDF_TARGET_ESP32S3=1",
    "-I",
    str(ROOT / "tests/host/include"),
    "-I",
    str(ROOT / "modev1"),
    str(ROOT / "tests/host/ei_classifier_porting_test.cpp"),
    str(ROOT / "modev1/edge-impulse-sdk/porting/espressif/ei_classifier_porting.cpp"),
    "-o",
    str(executable),
]
```

Assert that compilation succeeds, execution returns zero, and stdout contains `ei calloc alignment behavior passed`.

- [ ] **Step 4: Run the targeted test and verify RED**

Run:

```bash
python3 -m unittest tests.test_inference_component.InferenceComponentBehaviorTest.test_esp32s3_calloc_uses_aligned_zeroed_allocator -v
```

Expected: the executable aborts because production calls `heap_caps_calloc`; `aligned_calloc_calls` is `0` and `ordinary_calloc_calls` is `1`.

- [ ] **Step 5: Apply the minimal production fix**

In the ESP32-S3 and ESP-IDF 5.x branch of `ei_calloc()`, replace the ordinary capability calloc with:

```cpp
return heap_caps_aligned_calloc(16, nitems, size, MALLOC_CAP_DEFAULT);
```

Do not modify the pre-IDF-5 fallback or non-ESP32-S3 path.

- [ ] **Step 6: Run the targeted test and verify GREEN**

Run the command from Step 4. Expected: pass and print `ei calloc alignment behavior passed`.

- [ ] **Step 7: Run the full host regression suite**

Run:

```bash
python3 -m unittest discover -s tests -v
```

Expected: every host test passes, including task affinity, ESP-NN source discovery, overflow capacity, camera, HTTP, Wi-Fi, and dataset-capture tests.

- [ ] **Step 8: Run the ESP-IDF 5.5.4 firmware build**

Build with the existing isolated build directory and sdkconfig:

```bash
export IDF_PYTHON_ENV_PATH=/Users/stephenapollo/.espressif/tools/python/v5.5.4/venv
export IDF_PYTHON_CHECK_CONSTRAINTS=no
source /Users/stephenapollo/.espressif/v5.5.4/esp-idf/export.sh >/dev/null
idf.py -B /private/tmp/edgeDeploy-espnn-build.1mG2cC/build -D SDKCONFIG=/private/tmp/edgeDeploy-espnn-build.1mG2cC/sdkconfig build
```

Expected: compile and link succeed with no undefined `heap_caps_aligned_calloc` symbol, ESP-NN remains compiled, and the application fits the configured partition.

- [ ] **Step 9: Audit and commit the fix**

Run:

```bash
git diff --check
git status --short
```

Stage only the porting implementation, host fakes, host regression harness, Python test, and this plan. Leave `.gitignore` unstaged. Commit with:

```bash
git commit -m "fix: align esp-nn calloc buffers"
```

- [ ] **Step 10: Push and verify draft PR #3**

Push `feature/edge-impulse-inference`, then verify draft PR #3 still targets `main` and includes both the design checkpoint and aligned-allocation fix. Do not claim hardware acceptance until a newly flashed device runs repeated inference without allocation errors, heap corruption, `StoreProhibited`, or watchdog warnings.

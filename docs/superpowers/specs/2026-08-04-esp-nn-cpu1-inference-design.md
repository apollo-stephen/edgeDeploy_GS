# ESP-NN CPU1 Inference Fix Design

## Problem

Hardware logs show that a single inference completes successfully but spends
about 13.6 seconds in TensorFlow Lite's reference INT8 convolution. During that
continuous computation, the `ei_inference` task runs on CPU0 and prevents
`IDLE0` from running for the configured five-second task-watchdog interval.

The Edge Impulse export contains an ESP32-S3 ESP-NN implementation, but the
current model component explicitly disables it and compiles only C++ sources.
The bundled ESP-NN C and assembly sources therefore never enter the firmware.

## Chosen Approach

Use the ESP-NN copy included in the exported Edge Impulse SDK. Compile its C
and ESP32-S3 assembly sources, allow the SDK's ESP32-S3 configuration to enable
ESP-NN, and pin the inference task to CPU1.

Alternatives rejected:

1. Increasing or disabling the watchdog would hide CPU starvation without
   reducing the 13.6-second classification time.
2. Adding `espressif/esp-nn` from the Component Registry would introduce a
   second ESP-NN version whose API may not match the generated Edge Impulse SDK.
3. Pinning the unaccelerated task to CPU1 alone would move the idle-task
   watchdog warning from CPU0 to CPU1.

## Build Integration

`modev1/CMakeLists.txt` will continue compiling the existing Edge Impulse C++
runtime and generated model. It will additionally collect only `.c` and `.S`
files beneath `edge-impulse-sdk/porting/espressif/ESP-NN`. It will not restore
the unrelated CMSIS source set.

The explicit `EI_CLASSIFIER_TFLITE_ENABLE_ESP_NN=0` override will be replaced
with `EI_CLASSIFIER_TFLITE_ENABLE_ESP_NN=1` and
`EI_CLASSIFIER_TFLITE_ENABLE_ESP_NN_S3=1`. These definitions are explicit
because the bundled source guards are evaluated before ESP-IDF's target
configuration is included in some translation units. `EIDSP_USE_ESP_DSP=0`
remains unchanged because this image impulse does not require the separate
ESP-DSP integration.

## Task Placement

`inference_start()` will create `ei_inference` with
`xTaskCreatePinnedToCore(..., 1)`. CPU0 remains available for Wi-Fi and HTTP
work. Task priority, stack size, capture timeout, inference delay, camera-frame
ownership, and watchdog configuration remain unchanged.

CPU affinity is isolation rather than the primary performance fix. ESP-NN is
responsible for replacing the reference convolution path.

## Testing and Acceptance

Host behavior tests will verify that task creation requests CPU1 and preserves
failure rollback. A CMake discovery test will execute the Edge Impulse source
collector and verify that the bundled ESP-NN set contains both C and S3
assembly implementations.

A clean ESP-IDF 5.5.4 build must compile ESP-NN C and `.S` files, link without
undefined ESP-NN symbols, and fit the configured four-megabyte application
partition. Existing host tests must remain green.

Final performance acceptance is hardware-only: after flashing, serial output
must show classification below the five-second watchdog interval and no
`IDLE0` or `IDLE1` task-watchdog warnings during repeated inference. If the
accelerated model still exceeds five seconds, watchdog policy or model size
will be considered only after measuring the accelerated runtime.

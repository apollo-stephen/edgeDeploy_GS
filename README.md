# EdgeDeploy GS

[English](README.md) | [中文](README.zh-CN.md)

EdgeDeploy GS is an ESP32-S3 edge-vision firmware that captures native
128x128 JPEG frames from an OV5640 camera, serves a local Wi-Fi dashboard,
and runs a MobileNetV1 waste classifier on-device.

![EdgeDeploy GS demonstration](演示.gif)

## Features

- Native 128x128 OV5640 JPEG capture and MJPEG live preview.
- Periodic MobileNetV1 inference in a FreeRTOS task pinned to CPU1.
- Three waste classes: `harmful`, `recycleable`, and `wet`.
- A standalone ESP32-S3 SoftAP and dashboard at `http://192.168.4.1/`.
- Live preview, the exact frame used for inference, class scores, prediction,
  and timing information on one page.
- A local Python tool for continuous dataset capture without rebuilding the
  firmware.
- Serialized camera-frame ownership across streaming, capture, and inference.

## Requirements

### Hardware

- ESP32-S3 with 16 MB flash and Octal PSRAM.
- OV5640 camera wired to the pinout configured by this project.

### Software

- ESP-IDF 5.5.4.
- Python 3 for the local dataset capture tool and host tests.
- `espressif/esp32-camera` 2.1.7, resolved by ESP-IDF Component Manager.

`esp_http_server` is included with ESP-IDF. The project-owned `HTTP_CAPTURE`
component and the bundled ESP-NN kernels do not require separate registry
packages.

## How it works

### Camera capture

The firmware configures the OV5640 for `PIXFORMAT_JPEG`,
`FRAMESIZE_128X128`, JPEG quality 12, two DRAM frame buffers, and latest-frame
acquisition. The camera contract limits a JPEG to 8,192 bytes. Captured frames
are validated for format, dimensions, data pointer, and length before use.

The `CAMERA` component owns the frame-acquisition mutex. HTTP streaming,
single-frame capture, and inference therefore cannot retain the same camera
frame concurrently.

### MobileNetV1 inference

After the camera, SoftAP, and HTTP services start, the `ei_inference` task runs
once every two seconds on CPU1. CPU0 remains available for Wi-Fi and HTTP work.

Each inference iteration:

1. Acquires a native 128x128 JPEG with a 250 ms timeout.
2. Saves the original JPEG for the dashboard and decodes it into RGB888.
3. Resizes the image to the model's 96x96 input with the exported
   `FIT_SHORTEST` policy.
4. Runs the INT8 MobileNetV1 classifier using ESP-NN kernels.
5. Publishes versioned metadata and the matching original JPEG.

The camera frame is decoded into a 49,152-byte RGB888 capture buffer. The
`FIT_SHORTEST` resize operation uses a separate 49,152-byte workspace because
it first stores the 128x128 crop there, then resizes in place. The classifier
reads only the final 96x96 RGB region.

The exported label is spelled `recycleable`; the firmware preserves that exact
model label. A highest probability below the exported 0.60 threshold is
reported as `uncertain`.

Example serial output:

```text
I (...) inference: Timing: DSP 6 ms, classification 125 ms, anomaly 0 ms
I (...) inference: harmful: 0.00391
I (...) inference: recycleable: 0.01172
I (...) inference: wet: 0.98438
I (...) inference: Prediction: wet (0.98438)
```

The MobileNetV1 classifier uses an approximately 126 KB tensor arena. Large
allocations use PSRAM, and a custom 4 MB factory partition accommodates the
inference runtime and generated model.

## Configure the SoftAP

Project defaults are:

| Setting | Default |
| --- | --- |
| SSID | `ESP32S3-CAPTURE` |
| Password | `12345678` |
| Channel | `1` |
| Maximum stations | `1` |

To change them, run:

```bash
idf.py menuconfig
```

Open `Example Configuration` and edit the SoftAP settings. Defaults in
`sdkconfig.defaults` survive `idf.py set-target esp32s3`; use `menuconfig` only
for a deliberate local override.

## Build, flash, and monitor

Activate ESP-IDF 5.5.4, then run:

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.YOUR_PORT flash monitor
```

Exit the serial monitor with `Ctrl-]`.

Successful startup reports the detected camera PID, SoftAP address, inference
task status, and:

```text
Image preview ready at http://192.168.4.1/
```

## Use the dashboard

1. Connect a computer or phone to the configured ESP32-S3 SoftAP.
2. Open `http://192.168.4.1/`.
3. The left panel starts the native MJPEG live preview automatically.
4. The right panel shows the exact 128x128 JPEG used by the latest inference.
5. The result area updates the prediction, all class scores, and timing data.

Losing normal internet access while connected to this standalone access point
is expected. The dashboard polls metadata once per second and requests the
matching JPEG by sequence. Metadata and image updates are independent: a
temporary image failure leaves the last valid snapshot visible while results
continue updating and the image is retried. The live preview also reconnects
automatically after a stream error.

## Collect a dataset locally

The local Python console uses the firmware's existing HTTP API, so continuous
capture does not require a firmware rebuild. Connect the Mac to the ESP32
SoftAP and run from the repository root:

```bash
python3 dataset_capture_server.py
```

Then:

1. Open `http://127.0.0.1:8000`.
2. Enter a dataset name and capture interval.
3. Select **Start continuous capture**.
4. Select **Stop and save** when finished.
5. Find JPEG files and `metadata.csv` under `data/<dataset-name>/`.

The page keeps the MJPEG preview connected and displays the latest saved JPEG
plus up to 30 recent thumbnails. The 30-image limit applies only to the web
gallery and browser memory; every successful frame remains on disk. Capturing
400–600 images for one class is therefore supported.

## HTTP API

| Endpoint | Description |
| --- | --- |
| `GET /capture` | Returns one fresh `image/jpeg`. |
| `GET /api/status` | Returns camera readiness, counters, last JPEG length, free heap, and free PSRAM. |
| `GET /api/inference` | Returns the latest prediction, dynamic scores, timing, JPEG length, sequence, and age, or `{"ready":false}` before the first result. |
| `GET /api/inference/image?sequence=N` | Returns the JPEG for the current inference sequence; stale sequences receive HTTP 409. |

For a direct capture check:

```bash
curl -D - http://192.168.4.1/capture -o capture.jpg
file capture.jpg
```

The response should contain `Content-Type: image/jpeg`,
`X-Frame-Width: 128`, and `X-Frame-Height: 128`.

## Verification

Run all host-side behavior and regression tests with:

```bash
python3 -m unittest discover -s tests -v
```

A successful host test run and firmware build prove the software contracts and
compilation, but hardware acceptance still requires confirming that:

- The OV5640 is detected and the SoftAP remains discoverable.
- All three probabilities appear every two seconds without watchdog warnings.
- A known sample produces the expected class.
- The dashboard snapshot, scores, and serial output match for the same sequence.
- Preview and inference remain responsive during repeated operation.
- Heap, PSRAM, and camera frame buffers remain stable.

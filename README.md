# EdgeDeploy GS: OV5640 Capture and Edge Inference

This ESP32-S3 firmware initializes an OV5640 camera, serves native 128x128
MJPEG and single-frame JPEG capture over the board's SoftAP, and runs the
exported Edge Impulse image classifier in a periodic FreeRTOS task.

## Requirements

- ESP-IDF 5.5.4
- ESP32-S3 target
- 16 MB flash and Octal PSRAM
- OV5640 wired to the board pinout used by this project
- `espressif/esp32-camera` 2.1.7, resolved through ESP-IDF Component Manager

`esp_http_server` is built into ESP-IDF. The project-owned `HTTP_CAPTURE`
component does not require a separate registry download.

## Camera configuration

The firmware requests:

- `PIXFORMAT_JPEG`
- `FRAMESIZE_128X128`
- JPEG quality 12
- two DRAM frame buffers
- an explicit 8192-byte JPEG frame-buffer capacity, shared as the CAMERA
  component contract and compile-time checked against inference storage
- latest-frame acquisition

The serial log reports the detected sensor PID. Every HTTP response is also
validated against the actual `camera_fb_t` format, width, height, buffer, and
length.

## Edge Impulse inference

After the camera, SoftAP, and HTTP server start, one `ei_inference` FreeRTOS
task pinned to CPU1 runs an inference every two seconds. CPU0 remains available
for Wi-Fi and HTTP work. The model uses the Edge Impulse export's bundled
ESP-NN kernels for ESP32-S3 acceleration; no separate Registry component is
required. The task uses the CAMERA component's shared frame-ownership API, so
HTTP capture and inference cannot hold the same camera frame concurrently.

Each iteration:

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

The exported model label is spelled `recycleable`; logs preserve that exact
model label. If the highest probability is below the exported threshold of
0.60, the reported prediction is `uncertain`.

Example serial output:

```text
I (...) inference: Timing: DSP 4 ms, classification 120 ms, anomaly 0 ms
I (...) inference: harmful: 0.01234
I (...) inference: recycleable: 0.90123
I (...) inference: wet: 0.08643
I (...) inference: Prediction: recycleable (0.90123)
```

The active `modev2` deployment allocates an approximately 126 KB tensor arena.
`sdkconfig.defaults` enables Octal PSRAM and routes large heap allocations
there. The application uses a custom 4 MB factory partition because the Edge
Impulse SDK and generated model do not fit the default 1 MB application
partition. `modev1` remains in the repository for rollback, but it is not part
of the active build.

## Configure the SoftAP

Run:

```bash
idf.py menuconfig
```

Under `Example Configuration`, set the SoftAP SSID, password, channel, and
maximum station count. Project defaults in `sdkconfig.defaults` are SSID
`ESP32S3-CAPTURE`, password `12345678`, channel 1, and one station. These
values survive `idf.py set-target esp32s3`; use `menuconfig` only when a
deliberate local override is required.

## Build, flash, and monitor

Activate ESP-IDF 5.5.4, then run:

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.YOUR_PORT flash monitor
```

Exit the serial monitor with `Ctrl-]`.

Successful startup prints the detected camera PID, SoftAP address, inference
task status, and:

```text
Image preview ready at http://192.168.4.1/
```

## View and capture images

1. Join the configured ESP32-S3 SoftAP from the computer or phone.
2. Losing normal internet access while connected to this standalone AP is
   expected.
3. Open `http://192.168.4.1/`.
4. The left panel starts the native MJPEG live preview automatically. The right
   panel shows the latest successfully decoded JPEG used by inference.
5. Prediction, dynamic label scores, timing, sequence, and update age refresh
   from inference metadata even if the right-hand JPEG is temporarily unavailable.

The dashboard polls inference metadata once per second and requests the JPEG by
sequence. Metadata and snapshot publication are independent: a matching,
successfully decoded JPEG replaces the right-hand image, while image failures
leave the previous snapshot visible and retry on the next poll. The live preview
reconnects automatically after a stream error. The CAMERA component serializes
frame ownership.

Direct endpoints:

- `GET /capture` returns one fresh `image/jpeg`.
- `GET /api/status` returns camera readiness, capture counters, last JPEG
  length, free heap, and free PSRAM.
- `GET /api/inference` returns the latest versioned prediction, dynamic scores,
  timing, JPEG length, and update age, or `{"ready":false}` before first result.
- `GET /api/inference/image?sequence=N` returns the JPEG for the current
  inference sequence; stale sequences receive HTTP 409.

For a direct header check:

```bash
curl -D - http://192.168.4.1/capture -o capture.jpg
file capture.jpg
```

The response should include:

```text
Content-Type: image/jpeg
X-Frame-Width: 128
X-Frame-Height: 128
```

## 本机连续采集数据集

Python 本机控制台使用固件现有的 HTTP 接口，因此无需重新构建或烧录 ESP32
固件。先让 Mac 连接到 ESP32 SoftAP，再在项目根目录运行：

```bash
python3 dataset_capture_server.py
```

然后按以下步骤操作：

1. 在浏览器打开 `http://127.0.0.1:8000`。
2. 输入数据集名称和拍照间隔。
3. 按“开始连续拍照”。
4. 需要结束时按“停止并保存”。
5. JPEG 图片和 `metadata.csv` 位于 `data/<数据集名称>/`。

采集期间左侧 MJPEG 预览会继续播放，后台单帧采集通过摄像头互斥锁与直播
交错取帧，因此取帧瞬间可能出现轻微预览抖动，但页面不会主动断开视频流。
右侧显示最新一张已成功保存到磁盘的 JPEG，并保留本轮最近 30 张缩略图，
便于及时检查清晰度、构图和样本变化；失败或尚未落盘的帧不会进入图库。

30 张仅是网页显示和浏览器内存上限，磁盘仍会保存完整数据集及
`metadata.csv`，因此单个类别采集 400–600 张不会被截断。直播临时断开时页面
会自动重连，已经显示的有效采集图片会继续保留。

## Verification boundary

A successful build proves only that the current source compiles. Complete
hardware acceptance still requires serial confirmation that the OV5640 was
detected, the inference task started, all three probabilities appear every two
seconds, classification completes within the five-second task-watchdog window,
no `IDLE0` or `IDLE1` watchdog warnings appear, a known sample produces the
expected class, the HTTP preview remains usable during inference, and repeated
operation does not exhaust heap, PSRAM, or frame buffers. For dashboard
acceptance, the right-hand image and page scores must match serial output for
the same inference sequence.

Run host-side behavior and regression tests with:

```bash
python3 -m unittest discover -s tests -v
```

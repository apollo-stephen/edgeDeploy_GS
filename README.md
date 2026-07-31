# EdgeDeploy GS: OV5640 HTTP Image Transfer

This ESP32-S3 firmware initializes an OV5640 camera and serves native 128x128
MJPEG and single-frame JPEG capture over the board's SoftAP.

This branch intentionally does not include model inference, resizing, or LCD
output.

## Requirements

- ESP-IDF 5.5.4
- ESP32-S3 target
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
- an explicit 8192-byte JPEG frame-buffer capacity
- latest-frame acquisition

The serial log reports the detected sensor PID. Every HTTP response is also
validated against the actual `camera_fb_t` format, width, height, buffer, and
length.

## Configure the SoftAP

Run:

```bash
idf.py menuconfig
```

Under `Example Configuration`, set the SoftAP SSID, password, channel, and
maximum station count. The defaults are `myssid` and `mypassword`.

## Build, flash, and monitor

Activate ESP-IDF 5.5.4, then run:

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.YOUR_PORT flash monitor
```

Exit the serial monitor with `Ctrl-]`.

Successful startup prints the detected camera PID, SoftAP address, and:

```text
Image preview ready at http://192.168.4.1/
```

## View and capture images

1. Join the configured ESP32-S3 SoftAP from the computer or phone.
2. Losing normal internet access while connected to this standalone AP is
   expected.
3. Open `http://192.168.4.1/`.
4. Start the native MJPEG preview to view the live camera feed, or pause the
   preview when it is not needed. Press **Capture now** to request a separate
   single JPEG frame.

The page reports the actual width, height, JPEG byte length, and any HTTP
failure. It prevents overlapping browser requests; the CAMERA component also
serializes frame ownership.

Direct endpoints:

- `GET /capture` returns one fresh `image/jpeg`.
- `GET /api/status` returns camera readiness, capture counters, last JPEG
  length, free heap, and free PSRAM.

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

采集过程中，页面会显示来自 ESP32 的 MJPEG 预览。停止后预览会重新连接，便于
继续确认画面或开始下一轮采集。

## Verification boundary

A successful build proves only that the current source compiles. Complete
hardware acceptance still requires serial confirmation that the OV5640 was
detected, a browser-visible image, an actual 128x128 JPEG response, and stable
repeated capture without crashes or exhausted frame buffers.

Run host-side behavior and regression tests with:

```bash
python3 -m unittest discover -s tests -v
```

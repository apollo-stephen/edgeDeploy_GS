"""Loopback-only browser console for local ESP32 dataset capture."""

import argparse
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import signal
import threading
from urllib.parse import urlsplit

from dataset_capture import (
    CaptureAlreadyRunningError,
    CaptureManager,
    DEFAULT_DEVICE_URL,
    DEFAULT_INTERVAL_MS,
    Esp32Client,
)


MAX_REQUEST_BODY_BYTES = 4096

INDEX_HTML_TEMPLATE = """<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>ESP32 数据集采集</title>
  <style>
    body { font-family: system-ui, sans-serif; max-width: 36rem; margin: 2rem auto; padding: 0 1rem; }
    main { display: grid; gap: 1rem; }
    #preview { width:128px;height:128px; image-rendering: pixelated; background: #ddd; }
    label { display: grid; gap: .25rem; }
    input, button { font: inherit; padding: .5rem; }
    dl { display: grid; grid-template-columns: max-content 1fr; gap: .4rem 1rem; }
    dt { font-weight: 600; }
    dd { margin: 0; overflow-wrap: anywhere; }
    #lastError { color: #b42318; }
  </style>
</head>
<body>
  <main>
    <h1>ESP32 数据集采集</h1>
    <img id="preview" width="128" height="128" alt="摄像头预览">
    <label>数据集名称
      <input id="datasetName" maxlength="64" autocomplete="off">
    </label>
    <label>拍照间隔（毫秒）
      <input id="intervalMs" type="number" min="200" max="60000" value="__DEFAULT_INTERVAL_MS__">
    </label>
    <div>
      <button id="startButton" type="button" disabled>开始连续拍照</button>
      <button id="stopButton" type="button" disabled>停止并保存</button>
    </div>
    <dl>
      <dt>运行状态</dt><dd id="runningState">读取中</dd>
      <dt>已保存</dt><dd id="savedCount">0</dd>
      <dt>失败数</dt><dd id="failedCount">0</dd>
      <dt>最新文件</dt><dd id="latestFile">—</dd>
      <dt>最后错误</dt><dd id="lastError">—</dd>
    </dl>
  </main>
  <script>
    const streamUrl = __STREAM_URL__;
    const preview = document.getElementById("preview");
    const datasetName = document.getElementById("datasetName");
    const intervalMs = document.getElementById("intervalMs");
    const startButton = document.getElementById("startButton");
    const stopButton = document.getElementById("stopButton");
    const runningState = document.getElementById("runningState");
    const savedCount = document.getElementById("savedCount");
    const failedCount = document.getElementById("failedCount");
    const latestFile = document.getElementById("latestFile");
    const lastError = document.getElementById("lastError");
    let transition = "initializing";
    let refreshGeneration = 0;

    startButton.disabled = true;
    stopButton.disabled = true;

    function stopPreview() {
      preview.removeAttribute("src");
    }

    function startPreview() {
      preview.src = `${streamUrl}?t=${Date.now()}`;
    }

    async function requestJson(path, options = {}) {
      const response = await fetch(path, {
        cache: "no-store",
        headers: {"Content-Type": "application/json"},
        ...options,
      });
      const result = await response.json();
      if (!response.ok) {
        throw new Error(result.error || `请求失败 (${response.status})`);
      }
      return result;
    }

    function renderStatus(state) {
      runningState.textContent = state.running ? "采集中" : "已停止";
      savedCount.textContent = state.saved_count;
      failedCount.textContent = state.failed_count;
      latestFile.textContent = state.latest_file || "—";
      lastError.textContent = state.last_error || "—";
      if (state.running) {
        stopPreview();
      }
      if (transition === "idle") {
        startButton.disabled = state.running;
        stopButton.disabled = !state.running;
      } else {
        startButton.disabled = true;
        stopButton.disabled = true;
      }
    }

    async function refreshStatus() {
      const generation = ++refreshGeneration;
      const state = await requestJson("/api/status");
      if (generation !== refreshGeneration ||
          transition === "starting" ||
          transition === "stopping") {
        return null;
      }
      renderStatus(state);
      return state;
    }

    async function synchronizeStatus() {
      try {
        const state = await refreshStatus();
        if (state === null) {
          return null;
        }
        if (transition === "initializing") {
          transition = "idle";
          renderStatus(state);
          if (!state.running) {
            startPreview();
          }
        }
        return state;
      } catch (error) {
        stopPreview();
        startButton.disabled = true;
        stopButton.disabled = true;
        lastError.textContent = error.message;
        return null;
      }
    }

    startButton.addEventListener("click", async () => {
      transition = "starting";
      refreshGeneration += 1;
      startButton.disabled = true;
      stopButton.disabled = true;
      stopPreview();
      await new Promise(resolve => setTimeout(resolve, 200));
      try {
        const state = await requestJson("/api/capture/start", {
          method: "POST",
          body: JSON.stringify({
            dataset_name: datasetName.value,
            interval_ms: Number(intervalMs.value),
          }),
        });
        transition = "idle";
        refreshGeneration += 1;
        renderStatus(state);
      } catch (error) {
        transition = "initializing";
        refreshGeneration += 1;
        await synchronizeStatus();
        lastError.textContent = error.message;
      }
    });

    stopButton.addEventListener("click", async () => {
      transition = "stopping";
      refreshGeneration += 1;
      startButton.disabled = true;
      stopButton.disabled = true;
      try {
        const state = await requestJson("/api/capture/stop", {
          method: "POST",
          body: JSON.stringify({}),
        });
        transition = "idle";
        refreshGeneration += 1;
        renderStatus(state);
        startPreview();
      } catch (error) {
        transition = "initializing";
        refreshGeneration += 1;
        await synchronizeStatus();
        lastError.textContent = error.message;
      }
    });

    synchronizeStatus()
      .finally(() => {
        setInterval(synchronizeStatus, 500);
      });
  </script>
</body>
</html>
"""


def _stream_url(device_url: str) -> str:
    hostname = urlsplit(device_url).hostname
    if not hostname:
        raise ValueError("Device URL must include a hostname")
    if ":" in hostname:
        hostname = f"[{hostname}]"
    return f"http://{hostname}:81/stream"


def create_server(
    host: str,
    port: int,
    data_root: Path,
    device_url: str,
    manager: CaptureManager | None = None,
) -> ThreadingHTTPServer:
    """Create a local control server, optionally using a supplied manager."""
    active_manager = manager or CaptureManager(
        data_root,
        Esp32Client(device_url),
    )
    index_html = INDEX_HTML_TEMPLATE.replace(
        "__STREAM_URL__",
        json.dumps(_stream_url(device_url)),
    ).replace(
        "__DEFAULT_INTERVAL_MS__",
        str(DEFAULT_INTERVAL_MS),
    ).encode("utf-8")

    class DatasetCaptureRequestHandler(BaseHTTPRequestHandler):
        def _send_json(self, status: int, value: dict[str, object]) -> None:
            body = json.dumps(value, ensure_ascii=False).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def _send_html(self) -> None:
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(index_html)))
            self.end_headers()
            self.wfile.write(index_html)

        def _read_json_object(self) -> dict[str, object]:
            try:
                content_length = int(self.headers.get("Content-Length", ""))
            except ValueError as error:
                raise ValueError("Request body length is invalid") from error
            if content_length < 1 or content_length > MAX_REQUEST_BODY_BYTES:
                raise ValueError(
                    f"Request body must contain 1 to {MAX_REQUEST_BODY_BYTES} bytes"
                )
            try:
                value = json.loads(self.rfile.read(content_length))
            except (UnicodeDecodeError, json.JSONDecodeError) as error:
                raise ValueError("Request body must be valid JSON") from error
            if not isinstance(value, dict):
                raise ValueError("Request body must be a JSON object")
            return value

        def do_GET(self) -> None:
            path = urlsplit(self.path).path
            if path == "/":
                self._send_html()
            elif path == "/api/status":
                try:
                    state = active_manager.snapshot()
                except ValueError as error:
                    self._send_json(400, {"error": str(error)})
                except Exception:
                    self._send_json(500, {"error": "Internal server error"})
                else:
                    self._send_json(200, state)
            else:
                self._send_json(404, {"error": "Route not found"})

        def do_POST(self) -> None:
            path = urlsplit(self.path).path
            if path not in {"/api/capture/start", "/api/capture/stop"}:
                self._send_json(404, {"error": "Route not found"})
                return

            try:
                payload = self._read_json_object()
                if path == "/api/capture/start":
                    if set(payload) != {"dataset_name", "interval_ms"}:
                        raise ValueError(
                            "Start request requires dataset_name and interval_ms"
                        )
                    state = active_manager.start(
                        payload["dataset_name"],
                        int(payload["interval_ms"]),
                    )
                else:
                    if payload:
                        raise ValueError("Stop request requires an empty JSON object")
                    state = active_manager.stop()
            except CaptureAlreadyRunningError as error:
                self._send_json(409, {"error": str(error)})
            except (TypeError, ValueError) as error:
                self._send_json(400, {"error": str(error)})
            except Exception:
                self._send_json(500, {"error": "Internal server error"})
            else:
                self._send_json(200, state)

        def log_message(self, format_string: str, *args: object) -> None:
            print(f"{self.client_address[0]} {format_string % args}")

    server = ThreadingHTTPServer((host, port), DatasetCaptureRequestHandler)
    server.capture_manager = active_manager
    server.daemon_threads = True
    return server


def main() -> None:
    parser = argparse.ArgumentParser(description="启动本地 ESP32 数据集采集控制台")
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--device-url", default=DEFAULT_DEVICE_URL)
    arguments = parser.parse_args()

    data_root = Path(__file__).resolve().parent / "data"
    server = create_server(
        "127.0.0.1",
        arguments.port,
        data_root,
        arguments.device_url,
    )

    def request_shutdown(signum: int, frame: object) -> None:
        del signum, frame
        threading.Thread(
            target=server.shutdown,
            name="dataset-server-shutdown",
            daemon=True,
        ).start()

    signal.signal(signal.SIGINT, request_shutdown)
    signal.signal(signal.SIGTERM, request_shutdown)
    print(f"ESP32 device URL: {arguments.device_url}")
    print(f"Dataset root: {data_root.resolve()}")
    print(f"Local control URL: http://127.0.0.1:{server.server_port}")
    try:
        server.serve_forever(poll_interval=0.2)
    finally:
        server.capture_manager.shutdown()
        server.server_close()


if __name__ == "__main__":
    main()

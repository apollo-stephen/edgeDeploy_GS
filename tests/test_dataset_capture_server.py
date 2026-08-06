import json
from pathlib import Path
import re
import subprocess
import threading
import textwrap
import unittest
from urllib import error, request

from dataset_capture import CaptureAlreadyRunningError
from dataset_capture_server import create_server


ROOT = Path(__file__).resolve().parents[1]


class FakeManager:
    def __init__(self):
        self.state = {
            "running": False,
            "dataset_name": None,
            "interval_ms": 500,
            "saved_count": 0,
            "failed_count": 0,
            "latest_file": None,
            "last_error": None,
            "session_token": None,
            "recent_captures": [],
        }
        self.start_arguments = None
        self.stop_calls = 0
        self.shutdown_calls = 0
        self.start_error = None
        self.snapshot_error = None
        self.read_capture_error = None
        self.capture_payloads = {}
        self.capture_read_calls = []

    def snapshot(self):
        if self.snapshot_error is not None:
            raise self.snapshot_error
        return dict(self.state)

    def start(self, dataset_name, interval_ms):
        if self.start_error is not None:
            raise self.start_error
        self.start_arguments = (dataset_name, interval_ms)
        self.state["running"] = True
        return self.snapshot()

    def stop(self):
        self.stop_calls += 1
        self.state["running"] = False
        return self.snapshot()

    def shutdown(self):
        self.shutdown_calls += 1

    def read_recent_capture(self, session_token, sequence):
        self.capture_read_calls.append((session_token, sequence))
        if self.read_capture_error is not None:
            raise self.read_capture_error
        try:
            return self.capture_payloads[(session_token, sequence)]
        except KeyError as caught_error:
            raise FileNotFoundError(
                "Recent capture is not available"
            ) from caught_error


class DatasetCaptureServerTest(unittest.TestCase):
    def setUp(self):
        self.manager = FakeManager()
        self.server = create_server(
            "127.0.0.1",
            0,
            Path("/unused"),
            "http://192.168.4.1",
            manager=self.manager,
        )
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.base_url = f"http://127.0.0.1:{self.server.server_port}"

    def tearDown(self):
        self.server.shutdown()
        self.thread.join()
        self.server.server_close()

    def request(self, method, path, payload=None, raw_body=None):
        headers = {}
        body = raw_body
        if payload is not None:
            body = json.dumps(payload).encode("utf-8")
            headers["Content-Type"] = "application/json"
        http_request = request.Request(
            self.base_url + path,
            data=body,
            headers=headers,
            method=method,
        )
        try:
            response = request.urlopen(http_request, timeout=2)
        except error.HTTPError as caught_error:
            response = caught_error
        with response:
            return (
                response.status,
                response.headers.get("Content-Type"),
                response.headers.get("Cache-Control"),
                response.read(),
            )

    def test_index_contains_chinese_controls_and_stream_lifecycle(self):
        status, content_type, cache_control, body = self.request("GET", "/")

        self.assertEqual(200, status)
        self.assertIn("text/html", content_type)
        self.assertEqual("no-store", cache_control)
        page = body.decode("utf-8")
        self.assertIn("开始连续拍照", page)
        self.assertIn("停止并保存", page)
        self.assertIn("数据集名称", page)
        self.assertIn('class="visual-grid"', page)
        self.assertIn('id="preview"', page)
        self.assertIn('id="previewStatus"', page)
        self.assertIn('id="latestCapture"', page)
        self.assertIn('id="recentGallery"', page)
        self.assertIn('aria-label="最近保存的图片"', page)
        self.assertIn("@media (max-width: 700px)", page)
        self.assertIn('id="datasetName" maxlength="64"', page)
        self.assertIn('id="intervalMs" type="number"', page)
        self.assertIn('min="200" max="60000" value="500"', page)
        self.assertIn('id="startButton" type="button" disabled', page)
        self.assertIn('id="stopButton" type="button" disabled', page)
        self.assertIn(
            'const streamUrl = "http://192.168.4.1:81/stream";',
            page,
        )
        self.assertIn('preview.src = `${streamUrl}?t=${Date.now()}`;', page)
        self.assertIn("setInterval(synchronizeStatus, 500)", page)
        self.assertNotIn('preview.removeAttribute("src")', page)
        self.assertNotIn("setTimeout(resolve, 200)", page)

    def test_start_stop_and_status_transitions_keep_preview_connected(self):
        _, _, _, body = self.request("GET", "/")
        script = re.search(
            r"<script>(.*)</script>",
            body.decode("utf-8"),
            re.DOTALL,
        ).group(1)
        harness = textwrap.dedent(
            """
            const assert = require("node:assert/strict");
            const elements = new Map();
            function element(id) {
              const value = {
                id,
                disabled: false,
                textContent: "",
                value: id === "datasetName" ? "wet" : "500",
                listeners: {},
                sourceAssignments: 0,
                sourceRemovals: 0,
                sourceValue: undefined,
                children: [],
                removeAttribute(name) {
                  if (name === "src") {
                    this.sourceRemovals += 1;
                    this.sourceValue = undefined;
                  }
                },
                replaceChildren(...children) { this.children = children; },
                append(...children) { this.children.push(...children); },
                addEventListener(name, listener) { this.listeners[name] = listener; },
              };
              Object.defineProperty(value, "src", {
                get() { return this.sourceValue; },
                set(source) {
                  this.sourceAssignments += 1;
                  this.sourceValue = source;
                },
              });
              elements.set(id, value);
              return value;
            }
            global.document = {
              getElementById(id) { return elements.get(id) || element(id); },
              createElement(tag) { return element(`${tag}-${elements.size}`); },
            };
            global.setTimeout = callback => queueMicrotask(callback);
            let intervalCallback;
            global.setInterval = callback => { intervalCallback = callback; };

            function response(value) {
              return {ok: true, status: 200, async json() { return value; }};
            }
            let resolveInitial;
            let initialRequested = false;
            let running = false;
            let sessionToken = null;
            global.fetch = path => {
              if (path === "/api/status") {
                if (!initialRequested) {
                  initialRequested = true;
                  return new Promise(resolve => {
                    resolveInitial = () => resolve(response({
                      running, saved_count: 0, failed_count: 0,
                      latest_file: null, last_error: null,
                      session_token: null, recent_captures: [],
                    }));
                  });
                }
                return Promise.resolve(response({
                  running, saved_count: running ? 0 : 1, failed_count: 0,
                  latest_file: running ? null : "saved.jpg", last_error: null,
                  session_token: sessionToken,
                  recent_captures: [],
                }));
              }
              if (path === "/api/capture/start") {
                running = true;
                sessionToken = "a".repeat(32);
                return Promise.resolve(response({
                  running, saved_count: 0, failed_count: 0,
                  latest_file: null, last_error: null,
                  session_token: "a".repeat(32), recent_captures: [],
                }));
              }
              if (path === "/api/capture/stop") {
                running = false;
                return Promise.resolve(response({
                  running, saved_count: 1, failed_count: 0,
                  latest_file: "saved.jpg", last_error: null,
                  session_token: sessionToken, recent_captures: [],
                }));
              }
              throw new Error(`unexpected request: ${path}`);
            };
            """
        )
        assertions = textwrap.dedent(
            """
            async function drain() {
              await new Promise(resolve => setImmediate(resolve));
              await new Promise(resolve => setImmediate(resolve));
            }
            (async () => {
              const previewElement = elements.get("preview");
              const startElement = elements.get("startButton");
              const stopElement = elements.get("stopButton");
              assert.match(previewElement.src, /^http:\\/\\/192\\.168\\.4\\.1:81\\/stream\\?t=/);
              assert.equal(previewElement.sourceAssignments, 1);
              assert.equal(previewElement.sourceRemovals, 0);

              resolveInitial();
              await drain();
              assert.equal(startElement.disabled, false);

              await startElement.listeners.click();
              await intervalCallback();
              await drain();
              assert.equal(startElement.disabled, true);
              assert.equal(stopElement.disabled, false);
              assert.equal(previewElement.sourceAssignments, 1);
              assert.equal(previewElement.sourceRemovals, 0);

              await stopElement.listeners.click();
              await intervalCallback();
              await drain();
              assert.equal(startElement.disabled, false);
              assert.equal(stopElement.disabled, true);
              assert.equal(previewElement.sourceAssignments, 1);
              assert.equal(previewElement.sourceRemovals, 0);
            })().catch(error => {
              console.error(error);
              process.exitCode = 1;
            });
            """
        )

        completed = subprocess.run(
            ["node"],
            input=harness + script + assertions,
            text=True,
            capture_output=True,
            timeout=5,
            check=False,
        )

        self.assertEqual(0, completed.returncode, completed.stderr)

    def test_preview_errors_schedule_only_one_reconnect(self):
        _, _, _, body = self.request("GET", "/")
        script = re.search(
            r"<script>(.*)</script>",
            body.decode("utf-8"),
            re.DOTALL,
        ).group(1)
        harness = textwrap.dedent(
            """
            const assert = require("node:assert/strict");
            const elements = new Map();
            function element(id) {
              const value = {
                id,
                disabled: false,
                textContent: "",
                value: id === "datasetName" ? "wet" : "500",
                listeners: {},
                sourceAssignments: 0,
                sourceValue: undefined,
                removeAttribute(name) {
                  if (name === "src") {
                    this.sourceValue = undefined;
                  }
                },
                addEventListener(name, listener) { this.listeners[name] = listener; },
              };
              Object.defineProperty(value, "src", {
                get() { return this.sourceValue; },
                set(source) {
                  this.sourceAssignments += 1;
                  this.sourceValue = source;
                },
              });
              elements.set(id, value);
              return value;
            }
            global.document = {
              getElementById(id) { return elements.get(id) || element(id); },
            };
            const timers = [];
            global.setTimeout = (callback, milliseconds) => {
              timers.push({callback, milliseconds});
              return timers.length;
            };
            global.setInterval = () => {};

            function response(value) {
              return {ok: true, status: 200, async json() { return value; }};
            }
            global.fetch = path => {
              assert.equal(path, "/api/status");
              return Promise.resolve(response({
                running: false, saved_count: 0, failed_count: 0,
                latest_file: null, last_error: null,
                session_token: null, recent_captures: [],
              }));
            };
            """
        )
        assertions = textwrap.dedent(
            """
            async function drain() {
              await new Promise(resolve => setImmediate(resolve));
              await new Promise(resolve => setImmediate(resolve));
            }
            (async () => {
              const previewElement = elements.get("preview");
              const statusElement = elements.get("previewStatus");
              await drain();
              assert.equal(previewElement.sourceAssignments, 1);

              previewElement.listeners.error();
              previewElement.listeners.error();
              assert.equal(timers.length, 1);
              assert.equal(timers[0].milliseconds, 1000);
              assert.equal(statusElement.textContent, "直播断开，正在重连");

              timers[0].callback();
              assert.equal(previewElement.sourceAssignments, 2);
              previewElement.listeners.load();
              assert.equal(statusElement.textContent, "直播中");
            })().catch(error => {
              console.error(error);
              process.exitCode = 1;
            });
            """
        )

        completed = subprocess.run(
            ["node"],
            input=harness + script + assertions,
            text=True,
            capture_output=True,
            timeout=5,
            check=False,
        )

        self.assertEqual(0, completed.returncode, completed.stderr)

    def test_recent_gallery_is_bounded_deduplicated_and_session_scoped(self):
        _, _, _, body = self.request("GET", "/")
        script = re.search(
            r"<script>(.*)</script>",
            body.decode("utf-8"),
            re.DOTALL,
        ).group(1)
        harness = textwrap.dedent(
            """
            const assert = require("node:assert/strict");
            const elements = new Map();
            let elementSequence = 0;
            function makeElement(id) {
              const value = {
                id,
                disabled: false,
                textContent: "",
                value: id === "datasetName" ? "wet" : "500",
                listeners: {},
                children: [],
                sourceValue: undefined,
                removeAttribute(name) {
                  if (name === "src") this.sourceValue = undefined;
                },
                replaceChildren(...children) { this.children = children; },
                append(...children) { this.children.push(...children); },
                addEventListener(name, listener) { this.listeners[name] = listener; },
              };
              Object.defineProperty(value, "src", {
                get() { return this.sourceValue; },
                set(source) { this.sourceValue = source; },
              });
              return value;
            }
            function element(id) {
              if (!elements.has(id)) elements.set(id, makeElement(id));
              return elements.get(id);
            }
            global.document = {
              getElementById(id) { return element(id); },
              createElement(tag) {
                elementSequence += 1;
                return makeElement(`${tag}-${elementSequence}`);
              },
            };

            const revokedUrls = [];
            const failedImageKeys = new Set();
            global.URL = {
              createObjectURL(blob) { return `blob:${blob.key}`; },
              revokeObjectURL(value) { revokedUrls.push(value); },
            };
            global.Image = class {
              set src(value) {
                queueMicrotask(() => {
                  const key = value.slice("blob:".length);
                  if (failedImageKeys.has(key)) this.onerror();
                  else this.onload();
                });
              }
            };

            let intervalCallback;
            global.setInterval = callback => { intervalCallback = callback; };
            global.setTimeout = () => 1;

            const tokenA = "a".repeat(32);
            const tokenB = "b".repeat(32);
            const imageFetches = [];
            const delayedImageResolvers = new Map();
            const delayedImageKeys = new Set();
            let statusFetchCount = 0;
            let holdStatus = false;
            let heldStatusResolve;

            function jsonResponse(value) {
              return {ok: true, status: 200, async json() { return value; }};
            }
            function imageResponse(key) {
              return {ok: true, status: 200, async blob() { return {key}; }};
            }
            function recent(high, low) {
              const records = [];
              for (let sequence = high; sequence >= low; sequence -= 1) {
                records.push({
                  sequence,
                  filename: `${sequence}.jpg`,
                  captured_at: `2026-08-06T00:00:${String(sequence).padStart(2, "0")}+00:00`,
                });
              }
              return records;
            }
            function captureState(token, high, low, savedCount) {
              return {
                running: true,
                saved_count: savedCount,
                failed_count: 0,
                latest_file: `${high}.jpg`,
                last_error: null,
                session_token: token,
                recent_captures: recent(high, low),
              };
            }

            global.fetch = path => {
              if (path === "/api/status") {
                statusFetchCount += 1;
                const value = {
                  running: false, saved_count: 0, failed_count: 0,
                  latest_file: null, last_error: null,
                  session_token: null, recent_captures: [],
                };
                if (holdStatus) {
                  return new Promise(resolve => { heldStatusResolve = () => resolve(jsonResponse(value)); });
                }
                return Promise.resolve(jsonResponse(value));
              }
              const match = path.match(/^[/]api[/]captures[/]([0-9a-f]{32})[/]([0-9]+)$/);
              if (!match) throw new Error(`unexpected request: ${path}`);
              const key = `${match[1]}-${match[2]}`;
              imageFetches.push(path);
              if (delayedImageKeys.has(key)) {
                return new Promise(resolve => delayedImageResolvers.set(
                  key, () => resolve(imageResponse(key)),
                ));
              }
              return Promise.resolve(imageResponse(key));
            };
            """
        )
        assertions = textwrap.dedent(
            """
            async function drain() {
              await new Promise(resolve => setImmediate(resolve));
              await new Promise(resolve => setImmediate(resolve));
            }
            function fetchCount(suffix) {
              return imageFetches.filter(path => path.endsWith(suffix)).length;
            }
            (async () => {
              await drain();
              const gallery = elements.get("recentGallery");
              const latest = elements.get("latestCapture");

              const firstState = captureState(tokenA, 30, 1, 30);
              await reconcileGallery(firstState);
              assert.equal(gallery.children.length, 30);
              assert.equal(latest.src, `blob:${tokenA}-30`);
              assert.equal(imageFetches.length, 30);

              await reconcileGallery(firstState);
              assert.equal(imageFetches.length, 30);

              await reconcileGallery(captureState(tokenA, 31, 2, 31));
              assert.equal(gallery.children.length, 30);
              assert.equal(latest.src, `blob:${tokenA}-31`);
              assert.equal(fetchCount("/31"), 1);
              assert.ok(revokedUrls.includes(`blob:${tokenA}-1`));

              const key32 = `${tokenA}-32`;
              delayedImageKeys.add(key32);
              const state32 = captureState(tokenA, 32, 3, 32);
              const firstReconcile = reconcileGallery(state32);
              await drain();
              const secondReconcile = reconcileGallery(state32);
              await drain();
              assert.equal(fetchCount("/32"), 1);
              delayedImageResolvers.get(key32)();
              await Promise.all([firstReconcile, secondReconcile]);
              delayedImageKeys.delete(key32);
              assert.equal(latest.src, `blob:${tokenA}-32`);

              const key33 = `${tokenA}-33`;
              failedImageKeys.add(key33);
              await reconcileGallery(captureState(tokenA, 33, 4, 33));
              assert.equal(latest.src, `blob:${tokenA}-32`);
              failedImageKeys.delete(key33);

              await reconcileGallery(captureState(tokenA, 34, 5, 34));
              assert.equal(latest.src, `blob:${tokenA}-34`);

              const previousSessionUrls = [...loadedCaptures.values()]
                .map(item => item.url);
              await reconcileGallery(captureState(tokenB, 1, 1, 1));
              assert.equal(gallery.children.length, 1);
              assert.equal(latest.src, `blob:${tokenB}-1`);
              previousSessionUrls.forEach(url => assert.ok(revokedUrls.includes(url)));

              const statusCountBefore = statusFetchCount;
              holdStatus = true;
              const firstPoll = intervalCallback();
              const secondPoll = intervalCallback();
              await drain();
              assert.equal(statusFetchCount, statusCountBefore + 1);
              heldStatusResolve();
              await Promise.all([firstPoll, secondPoll]);
            })().catch(error => {
              console.error(error);
              process.exitCode = 1;
            });
            """
        )

        completed = subprocess.run(
            ["node"],
            input=harness + script + assertions,
            text=True,
            capture_output=True,
            timeout=5,
            check=False,
        )

        self.assertEqual(0, completed.returncode, completed.stderr)

    def test_status_start_and_stop_return_json_state(self):
        status, content_type, cache_control, body = self.request("GET", "/api/status")
        self.assertEqual(200, status)
        self.assertEqual("application/json; charset=utf-8", content_type)
        self.assertEqual("no-store", cache_control)
        self.assertFalse(json.loads(body)["running"])

        status, _, _, body = self.request(
            "POST",
            "/api/capture/start",
            {"dataset_name": "wet", "interval_ms": 500},
        )
        self.assertEqual(200, status)
        self.assertTrue(json.loads(body)["running"])
        self.assertEqual(("wet", 500), self.manager.start_arguments)

        status, _, _, body = self.request("POST", "/api/capture/stop", {})
        self.assertEqual(200, status)
        self.assertFalse(json.loads(body)["running"])
        self.assertEqual(1, self.manager.stop_calls)

    def test_recent_capture_route_returns_exact_saved_jpeg(self):
        token = "a" * 32
        payload = b"\xff\xd8saved\xff\xd9"
        self.manager.capture_payloads[(token, 7)] = payload

        try:
            response = request.urlopen(
                f"{self.base_url}/api/captures/{token}/7",
                timeout=2,
            )
        except error.HTTPError as caught_error:
            response = caught_error
        with response:
            self.assertEqual(200, response.status)
            self.assertEqual("image/jpeg", response.headers["Content-Type"])
            self.assertEqual("no-store", response.headers["Cache-Control"])
            self.assertEqual(str(len(payload)), response.headers["Content-Length"])
            self.assertEqual(payload, response.read())

        self.assertEqual([(token, 7)], self.manager.capture_read_calls)

    def test_recent_capture_route_rejects_malformed_and_stale_identifiers(self):
        token = "a" * 32
        malformed_paths = (
            f"/api/captures/{'A' * 32}/1",
            f"/api/captures/{'a' * 31}/1",
            "/api/captures/not-a-token/1",
            f"/api/captures/{token}/0",
            f"/api/captures/{token}/-1",
            f"/api/captures/{token}/+1",
            f"/api/captures/{token}",
            f"/api/captures/{token}/1/extra",
        )
        for path in malformed_paths:
            with self.subTest(path=path):
                status, content_type, _, body = self.request("GET", path)
                self.assertEqual(404, status)
                self.assertEqual("application/json; charset=utf-8", content_type)
                self.assertIn("error", json.loads(body))
        self.assertEqual([], self.manager.capture_read_calls)

        status, content_type, _, body = self.request(
            "GET", f"/api/captures/{'b' * 32}/9"
        )

        self.assertEqual(404, status)
        self.assertEqual("application/json; charset=utf-8", content_type)
        self.assertEqual(
            "Recent capture is not available",
            json.loads(body)["error"],
        )
        self.assertEqual([("b" * 32, 9)], self.manager.capture_read_calls)

    def test_recent_capture_route_hides_unexpected_manager_errors(self):
        self.manager.read_capture_error = RuntimeError("private image detail")

        status, _, _, body = self.request(
            "GET", f"/api/captures/{'a' * 32}/1"
        )

        decoded = body.decode("utf-8")
        self.assertEqual(500, status)
        self.assertEqual("Internal server error", json.loads(body)["error"])
        self.assertNotIn("private image detail", decoded)

    def test_unknown_routes_return_json_404(self):
        for method, path, payload in (
            ("GET", "/missing", None),
            ("POST", "/missing", {}),
        ):
            with self.subTest(method=method):
                status, content_type, cache_control, body = self.request(
                    method, path, payload
                )
                self.assertEqual(404, status)
                self.assertEqual("application/json; charset=utf-8", content_type)
                self.assertEqual("no-store", cache_control)
                self.assertIn("error", json.loads(body))

    def test_invalid_json_and_large_body_return_400(self):
        invalid_requests = (
            b"{not json}",
            b'"not an object"',
            b"x" * 4097,
        )
        for raw_body in invalid_requests:
            with self.subTest(body_size=len(raw_body)):
                status, _, _, body = self.request(
                    "POST",
                    "/api/capture/start",
                    raw_body=raw_body,
                )
                self.assertEqual(400, status)
                self.assertIn("error", json.loads(body))

    def test_start_requires_exact_fields_and_converts_interval_to_int(self):
        for payload in (
            {},
            {"dataset_name": "wet"},
            {"interval_ms": 500},
            {"dataset_name": "wet", "interval_ms": 500, "extra": True},
        ):
            with self.subTest(payload=payload):
                status, _, _, _ = self.request(
                    "POST", "/api/capture/start", payload
                )
                self.assertEqual(400, status)

        status, _, _, _ = self.request(
            "POST",
            "/api/capture/start",
            {"dataset_name": "wet", "interval_ms": "500"},
        )
        self.assertEqual(200, status)
        self.assertEqual(("wet", 500), self.manager.start_arguments)

    def test_stop_requires_an_empty_object(self):
        status, _, _, _ = self.request(
            "POST",
            "/api/capture/stop",
            {"unexpected": True},
        )
        self.assertEqual(400, status)
        self.assertEqual(0, self.manager.stop_calls)

    def test_known_and_unexpected_manager_errors_map_to_safe_json(self):
        cases = (
            (ValueError("bad dataset"), 400, "bad dataset"),
            (CaptureAlreadyRunningError("already running"), 409, "already running"),
            (RuntimeError("private implementation detail"), 500, "Internal server error"),
        )
        for manager_error, expected_status, expected_message in cases:
            with self.subTest(error=type(manager_error).__name__):
                self.manager.start_error = manager_error
                status, _, _, body = self.request(
                    "POST",
                    "/api/capture/start",
                    {"dataset_name": "wet", "interval_ms": 500},
                )
                decoded = body.decode("utf-8")
                self.assertEqual(expected_status, status)
                self.assertEqual(expected_message, json.loads(body)["error"])
                self.assertNotIn("Traceback", decoded)
                if expected_status == 500:
                    self.assertNotIn("private implementation detail", decoded)

    def test_status_manager_errors_map_to_safe_json(self):
        self.manager.snapshot_error = RuntimeError("private status detail")

        status, _, _, body = self.request("GET", "/api/status")

        decoded = body.decode("utf-8")
        self.assertEqual(500, status)
        self.assertEqual("Internal server error", json.loads(body)["error"])
        self.assertNotIn("Traceback", decoded)
        self.assertNotIn("private status detail", decoded)

    def test_readme_documents_local_capture_console(self):
        readme = (ROOT / "README.md").read_text(encoding="utf-8")
        self.assertIn("python3 dataset_capture_server.py", readme)
        self.assertIn("http://127.0.0.1:8000", readme)
        self.assertIn("data/<数据集名称>/", readme)
        self.assertIn("停止并保存", readme)
        self.assertIn("开始采集前，页面显示来自 ESP32 的 MJPEG 预览", readme)
        self.assertIn("连续采集期间会断开预览", readme)
        self.assertIn("手动停止或任务自动终止后", readme)
        self.assertIn("页面会重新连接预览", readme)
        self.assertNotIn("automatic refresh", readme)


if __name__ == "__main__":
    unittest.main()

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
        }
        self.start_arguments = None
        self.stop_calls = 0
        self.shutdown_calls = 0
        self.start_error = None
        self.snapshot_error = None

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
        self.assertIn('id="preview"', page)
        self.assertIn("width:128px;height:128px", page)
        self.assertIn('id="datasetName" maxlength="64"', page)
        self.assertIn('id="intervalMs" type="number"', page)
        self.assertIn('min="200" max="60000" value="500"', page)
        self.assertIn('id="startButton" type="button" disabled', page)
        self.assertIn('id="stopButton" type="button" disabled', page)
        self.assertIn(
            'const streamUrl = "http://192.168.4.1:81/stream";',
            page,
        )
        self.assertIn('preview.removeAttribute("src");', page)
        self.assertIn('preview.src = `${streamUrl}?t=${Date.now()}`;', page)
        self.assertIn("setInterval(synchronizeStatus, 500)", page)

    def test_deferred_status_and_start_conflict_keep_preview_disconnected(self):
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
                removeAttribute(name) { delete this[name]; },
                addEventListener(name, listener) { this.listeners[name] = listener; },
              };
              elements.set(id, value);
              return value;
            }
            global.document = {
              getElementById(id) { return elements.get(id) || element(id); },
            };
            global.setTimeout = callback => queueMicrotask(callback);
            let intervalCallback;
            global.setInterval = callback => { intervalCallback = callback; };

            function response(ok, status, value) {
              return {ok, status, async json() { return value; }};
            }
            let resolveInitial;
            let resolveStart;
            let statusCalls = 0;
            global.fetch = (path, options = {}) => {
              if (path === "/api/status") {
                statusCalls += 1;
                if (statusCalls === 1) {
                  return new Promise(resolve => {
                    resolveInitial = () => resolve(response(true, 200, {
                      running: false, saved_count: 0, failed_count: 0,
                      latest_file: null, last_error: null,
                    }));
                  });
                }
                if (statusCalls === 2) {
                  return Promise.resolve(response(true, 200, {
                    running: false, saved_count: 0, failed_count: 0,
                    latest_file: null, last_error: null,
                  }));
                }
                return Promise.resolve(response(true, 200, {
                  running: true, saved_count: 0, failed_count: 0,
                  latest_file: null, last_error: null,
                }));
              }
              if (path === "/api/capture/start") {
                return new Promise(resolve => {
                  resolveStart = () => resolve(response(false, 409, {
                    error: "already running",
                  }));
                });
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
              assert.equal(startElement.disabled, true);
              assert.equal(stopElement.disabled, true);
              assert.equal(previewElement.src, undefined);

              resolveInitial();
              await drain();
              assert.equal(startElement.disabled, false);
              assert.match(previewElement.src, /^http:\\/\\/192\\.168\\.4\\.1:81\\/stream\\?t=/);

              const click = startElement.listeners.click();
              await drain();
              assert.equal(previewElement.src, undefined);
              await intervalCallback();
              await drain();
              assert.equal(startElement.disabled, true);
              assert.equal(previewElement.src, undefined);

              resolveStart();
              await click;
              await drain();
              assert.equal(startElement.disabled, true);
              assert.equal(stopElement.disabled, false);
              assert.equal(previewElement.src, undefined);
              assert.equal(elements.get("lastError").textContent, "already running");
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


if __name__ == "__main__":
    unittest.main()

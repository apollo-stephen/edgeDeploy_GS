import json
from pathlib import Path
import threading
import unittest
from urllib import error, request

from dataset_capture import CaptureAlreadyRunningError
from dataset_capture_server import create_server


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
        self.assertIn('id="startButton"', page)
        self.assertIn('id="stopButton"', page)
        self.assertIn(
            'const streamUrl = "http://192.168.4.1:81/stream";',
            page,
        )
        self.assertIn('preview.removeAttribute("src");', page)
        self.assertIn('preview.src = `${streamUrl}?t=${Date.now()}`;', page)
        self.assertIn("setInterval(refreshStatus, 500)", page)

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


if __name__ == "__main__":
    unittest.main()

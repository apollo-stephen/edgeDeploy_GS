from datetime import datetime, timezone
from pathlib import Path
import csv
import json
import os
import tempfile
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from dataset_capture import (
    CaptureAlreadyRunningError,
    CaptureManager,
    DatasetWriter,
    Esp32Client,
    Frame,
    normalize_dataset_name,
    resolve_dataset_directory,
    validate_frame,
)


JPEG = b"\xff\xd8frame\xff\xd9"


def wait_until(test_case, predicate, timeout_seconds=2.0):
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.01)
    test_case.fail("condition was not reached before timeout")


class FakeClient:
    def __init__(self, outcomes):
        self.outcomes = iter(outcomes)
        self.wait_calls = 0
        self.capture_calls = 0

    def wait_for_stream_release(self, timeout_seconds=3.0):
        self.wait_calls += 1

    def capture(self):
        self.capture_calls += 1
        outcome = next(self.outcomes)
        if isinstance(outcome, Exception):
            raise outcome
        return outcome


class Esp32Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path.startswith("/api/status"):
            time.sleep(getattr(self.server, "status_delay_seconds", 0))
            payload = json.dumps(
                {
                    "camera_ready": True,
                    "frame_size": "128x128",
                    "stream_client_connected": self.server.stream_connected,
                }
            ).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
        elif self.path.startswith("/capture"):
            self.send_response(200)
            self.send_header("Content-Type", "image/jpeg")
            self.send_header("X-Frame-Width", "128")
            self.send_header("X-Frame-Height", "128")
            self.send_header("Content-Length", str(len(JPEG)))
            self.end_headers()
            self.wfile.write(JPEG)
        else:
            self.send_error(404)

    def log_message(self, format, *args):
        pass


class DatasetStorageTest(unittest.TestCase):
    def test_dataset_name_accepts_confirmed_allowlist_and_rejects_other_text(self):
        for value, expected in (
            (" 可回收物 01 ", "可回收物 01"),
            ("Data_set-02", "Data_set-02"),
            ("\u3400\u4dbf\u4e00\u9fff", "\u3400\u4dbf\u4e00\u9fff"),
        ):
            with self.subTest(value=value):
                self.assertEqual(expected, normalize_dataset_name(value))

        for value in (
            "",
            ".",
            "..",
            "../wet",
            "wet/hot",
            "wet\\hot",
            "bad\nname",
            "wet!",
            "湿垃圾。",
            "湿垃圾♻️",
            "wet🔥",
        ):
            with self.subTest(value=value):
                with self.assertRaises(ValueError):
                    normalize_dataset_name(value)

    def test_resolved_directory_stays_under_data_root(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory) / "data"
            directory = resolve_dataset_directory(root, "有害垃圾")
            self.assertEqual(root.resolve() / "有害垃圾", directory)

    def test_frame_validation_requires_jpeg_and_128_square_headers(self):
        validate_frame(JPEG, "image/jpeg; charset=binary", "128", "128")
        invalid = (
            (b"plain", "image/jpeg", "128", "128"),
            (JPEG, "text/plain", "128", "128"),
            (JPEG, "image/jpeg", "160", "120"),
        )
        for arguments in invalid:
            with self.subTest(arguments=arguments):
                with self.assertRaises(ValueError):
                    validate_frame(*arguments)

    def test_writer_continues_numbering_and_appends_metadata(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory) / "data"
            dataset = root / "wet"
            dataset.mkdir(parents=True)
            (dataset / "00003_old.jpg").write_bytes(JPEG)
            writer = DatasetWriter(root, "wet")
            frame = Frame(
                payload=JPEG,
                source_url="http://192.168.4.1/capture",
                captured_at=datetime(2026, 7, 31, 12, 0, 0, 123000, timezone.utc),
            )

            destination = writer.save(frame)

            self.assertEqual("00004_20260731T120000_123.jpg", destination.name)
            self.assertEqual(JPEG, destination.read_bytes())
            self.assertFalse(destination.with_suffix(".jpg.part").exists())
            with (dataset / "metadata.csv").open(encoding="utf-8", newline="") as metadata_file:
                rows = list(csv.DictReader(metadata_file))
            self.assertEqual(1, len(rows))
            self.assertEqual(destination.name, rows[0]["relative_path"])
            self.assertEqual("wet", rows[0]["dataset_name"])
            self.assertEqual("4", rows[0]["index"])

    def test_writer_rejects_metadata_symlink_without_changing_outside_file(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            temporary_root = Path(temporary_directory)
            root = temporary_root / "data"
            outside = temporary_root / "outside.csv"
            outside.write_text("outside content\n", encoding="utf-8")
            writer = DatasetWriter(root, "wet")
            (writer.directory / "metadata.csv").symlink_to(outside)
            frame = Frame(JPEG, "http://device/capture", datetime.now(timezone.utc))

            with self.assertRaises(OSError):
                writer.save(frame)

            self.assertEqual("outside content\n", outside.read_text(encoding="utf-8"))
            self.assertEqual([], list(writer.directory.glob("*.jpg")))

    def test_writer_rejects_dangling_metadata_symlink_without_creating_target(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            temporary_root = Path(temporary_directory)
            root = temporary_root / "data"
            outside = temporary_root / "outside.csv"
            writer = DatasetWriter(root, "wet")
            (writer.directory / "metadata.csv").symlink_to(outside)
            frame = Frame(JPEG, "http://device/capture", datetime.now(timezone.utc))

            with self.assertRaises(OSError):
                writer.save(frame)

            self.assertFalse(outside.exists())
            self.assertEqual([], list(writer.directory.glob("*.jpg")))

    def test_writer_rolls_back_partial_metadata_when_metadata_append_fails(self):
        class FailingWriter(DatasetWriter):
            @staticmethod
            def _write_metadata(metadata_fd, payload):
                os.write(metadata_fd, payload[:10])
                raise OSError("disk full")

        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory) / "data"
            writer = FailingWriter(root, "wet")
            frame = Frame(JPEG, "http://device/capture", datetime.now(timezone.utc))

            with self.assertRaises(OSError):
                writer.save(frame)

            self.assertEqual([], list(writer.directory.glob("*.jpg")))
            self.assertFalse((writer.directory / "metadata.csv").exists())

    def test_writer_restores_existing_metadata_when_append_fails(self):
        class FailingWriter(DatasetWriter):
            @staticmethod
            def _write_metadata(metadata_fd, payload):
                os.write(metadata_fd, payload[:10])
                raise OSError("disk full")

        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory) / "data"
            writer = FailingWriter(root, "wet")
            metadata_path = writer.directory / "metadata.csv"
            metadata_path.write_text("existing metadata\n", encoding="utf-8")
            frame = Frame(JPEG, "http://device/capture", datetime.now(timezone.utc))

            with self.assertRaises(OSError):
                writer.save(frame)

            self.assertEqual([], list(writer.directory.glob("*.jpg")))
            self.assertEqual(
                "existing metadata\n",
                metadata_path.read_text(encoding="utf-8"),
            )

    def test_two_writers_allocate_distinct_indices(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory) / "data"
            first_writer = DatasetWriter(root, "wet")
            second_writer = DatasetWriter(root, "wet")
            frame = Frame(JPEG, "http://device/capture", datetime.now(timezone.utc))

            first_path = first_writer.save(frame)
            second_path = second_writer.save(frame)

            self.assertEqual("00001", first_path.name[:5])
            self.assertEqual("00002", second_path.name[:5])


class Esp32ClientTest(unittest.TestCase):
    def setUp(self):
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Esp32Handler)
        self.server.stream_connected = False
        self.thread = threading.Thread(target=self.server.serve_forever)
        self.thread.start()
        self.base_url = f"http://127.0.0.1:{self.server.server_port}"

    def tearDown(self):
        self.server.shutdown()
        self.thread.join()
        self.server.server_close()

    def test_client_fetches_status_waits_and_captures_frame(self):
        client = Esp32Client(self.base_url)
        self.assertFalse(client.fetch_status()["stream_client_connected"])
        client.wait_for_stream_release(timeout_seconds=0.5)
        frame = client.capture()
        self.assertEqual(JPEG, frame.payload)
        self.assertTrue(frame.source_url.startswith(f"{self.base_url}/capture?t="))

    def test_wait_for_stream_release_times_out_while_stream_is_connected(self):
        self.server.stream_connected = True
        client = Esp32Client(self.base_url)
        with self.assertRaises(TimeoutError):
            client.wait_for_stream_release(timeout_seconds=0.05)

    def test_wait_for_stream_release_limits_each_status_request_to_deadline(self):
        self.server.stream_connected = True
        self.server.status_delay_seconds = 0.2
        client = Esp32Client(self.base_url)
        started_at = time.monotonic()

        with self.assertRaises(TimeoutError):
            client.wait_for_stream_release(timeout_seconds=0.05)

        self.assertLess(time.monotonic() - started_at, 0.15)


class CaptureManagerTest(unittest.TestCase):
    def setUp(self):
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.data_root = Path(self.temporary_directory.name) / "data"
        self.frame = Frame(JPEG, "http://device/capture", datetime.now(timezone.utc))

    def tearDown(self):
        self.temporary_directory.cleanup()

    def test_start_saves_frames_and_stop_returns_final_state(self):
        client = FakeClient([self.frame, self.frame])
        manager = CaptureManager(self.data_root, client)

        manager.start("wet", 200)
        wait_until(self, lambda: manager.snapshot()["saved_count"] == 2)
        state = manager.stop()

        self.assertFalse(state["running"])
        self.assertEqual(2, state["saved_count"])
        self.assertEqual(1, client.wait_calls)

    def test_start_rejects_a_second_worker(self):
        client = FakeClient([self.frame])
        manager = CaptureManager(self.data_root, client)

        manager.start("wet", 200)
        with self.assertRaises(CaptureAlreadyRunningError):
            manager.start("wet", 200)
        manager.stop()

    def test_worker_retries_three_times_before_recording_one_failure(self):
        client = FakeClient([OSError("offline")] * 3 + [self.frame])
        manager = CaptureManager(self.data_root, client)

        manager.start("wet", 200)
        wait_until(self, lambda: manager.snapshot()["saved_count"] == 1)
        state = manager.stop()

        self.assertEqual(1, state["failed_count"])
        self.assertEqual(1, state["saved_count"])
        self.assertEqual(4, client.capture_calls)

    def test_worker_stops_after_five_consecutive_failed_frames(self):
        client = FakeClient([OSError("offline")] * 15)
        manager = CaptureManager(self.data_root, client)

        manager.start("wet", 200)
        wait_until(self, lambda: not manager.snapshot()["running"], timeout_seconds=6.0)
        state = manager.snapshot()

        self.assertEqual(5, state["failed_count"])
        self.assertEqual(0, state["saved_count"])
        self.assertIn("连续 5 张采集失败", state["last_error"])

    def test_start_rejects_out_of_range_intervals_before_creating_dataset(self):
        manager = CaptureManager(self.data_root, FakeClient([]))

        for interval_ms in (199, 60_001):
            with self.subTest(interval_ms=interval_ms):
                with self.assertRaises(ValueError):
                    manager.start("wet", interval_ms)
                self.assertFalse((self.data_root / "wet").exists())

    def test_shutdown_interrupts_wait_without_another_capture(self):
        client = FakeClient([self.frame, self.frame])
        manager = CaptureManager(self.data_root, client)

        manager.start("wet", 60_000)
        wait_until(self, lambda: manager.snapshot()["saved_count"] == 1)
        manager.shutdown()

        self.assertFalse(manager.snapshot()["running"])
        self.assertEqual(1, client.capture_calls)

    def test_stop_does_not_join_a_worker_started_during_stop(self):
        class BlockingClient(FakeClient):
            def __init__(self, outcomes):
                super().__init__(outcomes)
                self.worker_started = threading.Event()

            def wait_for_stream_release(self, timeout_seconds=3.0):
                super().wait_for_stream_release(timeout_seconds)
                self.worker_started.set()

        client = BlockingClient([self.frame])
        manager = CaptureManager(self.data_root, client)
        start_thread = None

        class StartDuringStopEvent(threading.Event):
            def __init__(self):
                super().__init__()
                self.triggered = False

            def set(self):
                nonlocal start_thread
                super().set()
                if not self.triggered:
                    self.triggered = True
                    start_thread = threading.Thread(
                        target=manager.start,
                        args=("wet", 60_000),
                    )
                    start_thread.start()
                    client.worker_started.wait(0.2)

        manager._stop_event = StartDuringStopEvent()
        stop_finished = threading.Event()
        stop_thread = threading.Thread(
            target=lambda: (manager.stop(), stop_finished.set()),
        )

        stop_thread.start()
        try:
            self.assertTrue(stop_finished.wait(1.0))
        finally:
            manager._stop_event.set()
            stop_thread.join(1.0)
            if start_thread is not None:
                start_thread.join(1.0)

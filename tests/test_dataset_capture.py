from datetime import datetime, timezone
from pathlib import Path
import csv
import json
import tempfile
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from dataset_capture import (
    DatasetWriter,
    Esp32Client,
    Frame,
    normalize_dataset_name,
    resolve_dataset_directory,
    validate_frame,
)


JPEG = b"\xff\xd8frame\xff\xd9"


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
    def test_dataset_name_accepts_unicode_and_rejects_path_escape(self):
        self.assertEqual("可回收物 01", normalize_dataset_name(" 可回收物 01 "))
        for value in ("", ".", "..", "../wet", "wet/hot", "wet\\hot", "bad\nname"):
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

    def test_writer_rolls_back_partial_metadata_when_metadata_append_fails(self):
        class FailingWriter(DatasetWriter):
            def _append_metadata(self, destination, frame, index):
                (self.directory / "metadata.csv").write_text("partial row", encoding="utf-8")
                raise OSError("disk full")

        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory) / "data"
            writer = FailingWriter(root, "wet")
            frame = Frame(JPEG, "http://device/capture", datetime.now(timezone.utc))

            with self.assertRaises(OSError):
                writer.save(frame)

            self.assertEqual([], list(writer.directory.glob("*.jpg")))
            self.assertFalse((writer.directory / "metadata.csv").exists())

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

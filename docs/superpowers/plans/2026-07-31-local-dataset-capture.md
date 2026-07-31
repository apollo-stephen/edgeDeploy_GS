# Local Dataset Capture Console Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a localhost browser console that pauses the ESP32 MJPEG preview, continuously fetches native 128×128 JPEG frames, and saves each frame immediately under `data/<dataset-name>/`.

**Architecture:** Keep the ESP32 firmware unchanged. Put device access, path validation, atomic storage, metadata, and the single-worker capture state machine in `dataset_capture.py`; put the loopback HTTP API, embedded control page, and process lifecycle in `dataset_capture_server.py`. Use only the Python standard library and test every boundary with `unittest` and local fake HTTP servers.

**Tech Stack:** Python 3 standard library (`dataclasses`, `csv`, `hashlib`, `http.server`, `json`, `pathlib`, `threading`, `urllib`), existing ESP32 HTTP endpoints, `unittest`

## Global Constraints

- Run from the repository root with `python3 dataset_capture_server.py`.
- Bind only to `127.0.0.1:8000` by default.
- Save only under `/Users/stephenapollo/Desktop/proj/edgeDeploy_GS/data`.
- Default capture interval is exactly 500 ms; accepted range is 200–60,000 ms.
- Allow only one capture worker and only one in-flight `/capture` request.
- Retry each frame at most 2 additional times, waiting 500 ms between attempts.
- Stop automatically after 5 consecutively failed frames.
- Accept only complete JPEG responses marked `image/jpeg` and `128×128`.
- Write each JPEG through a `.part` file and atomic rename.
- Continue numbering an existing dataset without overwriting old images.
- Preserve the existing ESP32 `/capture`, `/stream`, and `/api/status` implementation unchanged.
- Use no third-party Python packages.

---

## File Structure

- Create `dataset_capture.py`: dataset-name validation, frame validation, ESP32 client, atomic dataset writer, capture status, and single-worker capture manager.
- Create `dataset_capture_server.py`: loopback HTTP server, JSON API, embedded Chinese control page, signal handling, and command-line entry point.
- Create `tests/test_dataset_capture.py`: storage, validation, ESP32 client, retry, interval, and manager lifecycle tests.
- Create `tests/test_dataset_capture_server.py`: local HTTP API and browser-page behavior tests.
- Modify `.gitignore`: ignore generated `/data/` images while keeping source and tests tracked.
- Modify `README.md`: replace stale snapshot-preview text and document the local dataset console.

### Task 1: Safe Dataset Storage and ESP32 Client

**Files:**
- Create: `dataset_capture.py`
- Create: `tests/test_dataset_capture.py`
- Modify: `.gitignore`

**Interfaces:**
- Produces: `Frame(payload: bytes, source_url: str, captured_at: datetime)`.
- Produces: `normalize_dataset_name(value: str) -> str`.
- Produces: `resolve_dataset_directory(data_root: Path, dataset_name: str) -> Path`.
- Produces: `validate_frame(payload: bytes, content_type: str | None, width: str | None, height: str | None) -> None`.
- Produces: `DatasetWriter(data_root: Path, dataset_name: str)` with `save(frame: Frame) -> Path`.
- Produces: `Esp32Client(base_url: str, timeout_seconds: float = 10.0)` with `fetch_status() -> dict[str, object]`, `wait_for_stream_release(timeout_seconds: float = 3.0) -> None`, and `capture() -> Frame`.
- Consumes: ESP32 response headers `Content-Type`, `X-Frame-Width`, and `X-Frame-Height`.

- [ ] **Step 1: Write failing name, path, JPEG, numbering, and metadata tests**

Create `tests/test_dataset_capture.py` with these `unittest` cases:

```python
from datetime import datetime, timezone
from pathlib import Path
import csv
import tempfile
import unittest

from dataset_capture import (
    DatasetWriter,
    Frame,
    normalize_dataset_name,
    resolve_dataset_directory,
    validate_frame,
)


JPEG = b"\xff\xd8frame\xff\xd9"


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

            self.assertEqual(
                "00004_20260731T120000_123.jpg",
                destination.name,
            )
            self.assertEqual(JPEG, destination.read_bytes())
            self.assertFalse(destination.with_suffix(".jpg.part").exists())
            with (dataset / "metadata.csv").open(
                encoding="utf-8",
                newline="",
            ) as metadata_file:
                rows = list(csv.DictReader(metadata_file))
            self.assertEqual(1, len(rows))
            self.assertEqual(destination.name, rows[0]["relative_path"])
            self.assertEqual("wet", rows[0]["dataset_name"])
            self.assertEqual("4", rows[0]["index"])
```

- [ ] **Step 2: Run the focused tests and verify failure**

Run:

```bash
python3 -m unittest tests.test_dataset_capture.DatasetStorageTest -v
```

Expected: import failure because `dataset_capture.py` does not exist.

- [ ] **Step 3: Implement validation, atomic storage, and metadata**

Create `dataset_capture.py` with these exact public constants and types:

```python
DEFAULT_DEVICE_URL = "http://192.168.4.1"
DEFAULT_INTERVAL_MS = 500
MIN_INTERVAL_MS = 200
MAX_INTERVAL_MS = 60_000
FRAME_RETRIES = 2
RETRY_DELAY_SECONDS = 0.5
MAX_CONSECUTIVE_FAILURES = 5
DATASET_NAME_MAX_LENGTH = 64


@dataclass(frozen=True)
class Frame:
    payload: bytes
    source_url: str
    captured_at: datetime
```

Implement `normalize_dataset_name()` so it trims whitespace, accepts 1–64
Unicode characters, and rejects `.`, `..`, `/`, `\`, and all characters for
which `unicodedata.category(character)` begins with `"C"`.

Implement `resolve_dataset_directory()` by resolving both the root and candidate
and requiring:

```python
candidate.is_relative_to(resolved_root)
```

Implement `validate_frame()` by normalizing the media type before `;`, checking
the JPEG SOI and EOI bytes, and requiring the width and height strings to equal
`"128"`.

Implement `DatasetWriter` with:

```python
METADATA_FIELDS = (
    "relative_path",
    "dataset_name",
    "index",
    "captured_at",
    "bytes",
    "sha256",
    "source_url",
)
INDEX_PATTERN = re.compile(r"^(\d+)_.*\.jpe?g$", re.IGNORECASE)
```

`DatasetWriter.save()` must:

1. Build the next five-digit filename.
2. Refuse to proceed if the final JPEG path already exists.
3. Write bytes to `<filename>.part` using `open("xb")`.
4. Flush and call `os.fsync()`.
5. Replace the final JPEG path with `Path.replace()`.
6. Append one `csv.DictWriter` row, creating the header once.
7. If metadata append fails, delete the just-created final JPEG so the operation
   has no partially recorded success.
8. Delete a remaining `.part` file from a `finally` block.
9. Increment its in-memory next index only after the JPEG and metadata succeed.

Add `/data/` to `.gitignore`.

- [ ] **Step 4: Add failing ESP32 HTTP client tests**

Extend `tests/test_dataset_capture.py` with a local `ThreadingHTTPServer` whose
handler returns:

```python
if self.path.startswith("/api/status"):
    payload = json.dumps(
        {
            "camera_ready": True,
            "frame_size": "128x128",
            "stream_client_connected": False,
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
```

Assert:

```python
client = Esp32Client(base_url)
self.assertFalse(client.fetch_status()["stream_client_connected"])
client.wait_for_stream_release(timeout_seconds=0.5)
frame = client.capture()
self.assertEqual(JPEG, frame.payload)
self.assertTrue(frame.source_url.startswith(f"{base_url}/capture?t="))
```

Add a status handler mode that always returns
`"stream_client_connected": true` and assert that
`wait_for_stream_release(timeout_seconds=0.05)` raises `TimeoutError`.

- [ ] **Step 5: Run the client tests and verify failure**

Run:

```bash
python3 -m unittest tests.test_dataset_capture -v
```

Expected: failure because `Esp32Client` is not defined.

- [ ] **Step 6: Implement the ESP32 client**

Implement `Esp32Client` using `urllib.request`:

```python
class Esp32Client:
    def __init__(
        self,
        base_url: str = DEFAULT_DEVICE_URL,
        timeout_seconds: float = 10.0,
    ) -> None:
        self.base_url = base_url.rstrip("/")
        self.timeout_seconds = timeout_seconds
```

`fetch_status()` must require HTTP 200, JSON object output,
`camera_ready is True`, and `frame_size == "128x128"`.

`wait_for_stream_release()` must poll `fetch_status()` every 100 ms until
`stream_client_connected is False`, using `time.monotonic()` for the deadline.

`capture()` must add `?t=<time.time_ns()>`, read the response, call
`validate_frame()` with the three response headers, and return a timezone-aware
`Frame` from `datetime.now().astimezone()`.

- [ ] **Step 7: Run Task 1 tests and commit**

Run:

```bash
python3 -m unittest tests.test_dataset_capture -v
python3 -m py_compile dataset_capture.py
git diff --check
```

Expected: all tests pass, compilation succeeds, and `git diff --check` prints
nothing.

Commit:

```bash
git add .gitignore dataset_capture.py tests/test_dataset_capture.py
git commit -m "feat: add safe dataset frame storage"
```

### Task 2: Single-Worker Capture Manager

**Files:**
- Modify: `dataset_capture.py`
- Modify: `tests/test_dataset_capture.py`

**Interfaces:**
- Consumes: `Esp32Client`, `DatasetWriter`, and the constants from Task 1.
- Produces: `CaptureManager(data_root: Path, client: Esp32Client)`.
- Produces: `CaptureManager.start(dataset_name: str, interval_ms: int) -> dict[str, object]`.
- Produces: `CaptureManager.stop() -> dict[str, object]`.
- Produces: `CaptureManager.snapshot() -> dict[str, object]`.
- Produces: `CaptureManager.shutdown() -> None`.

- [ ] **Step 1: Write failing manager lifecycle and retry tests**

Add a deterministic fake client:

```python
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
```

Add tests that:

- Start with interval `200`, wait until two images are saved, stop, and assert
  `running is False`, `saved_count == 2`, and `wait_calls == 1`.
- Call `start()` twice and assert the second call raises
  `CaptureAlreadyRunningError`.
- Feed three exceptions followed by a frame and assert one failed frame, one
  saved frame, and four total capture calls.
- Feed fifteen exceptions and assert automatic stop with
  `failed_count == 5`, `saved_count == 0`, and an error containing
  `"连续 5 张采集失败"`.
- Reject intervals `199` and `60_001` before a dataset directory is created.
- Call `shutdown()` during `Event.wait()` and assert the worker exits without
  another capture.

Use a polling helper with a two-second deadline rather than fixed test sleeps:

```python
def wait_until(predicate, timeout_seconds=2.0):
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.01)
    self.fail("condition was not reached before timeout")
```

- [ ] **Step 2: Run manager tests and verify failure**

Run:

```bash
python3 -m unittest tests.test_dataset_capture.CaptureManagerTest -v
```

Expected: import failure for `CaptureManager`.

- [ ] **Step 3: Implement thread-safe manager state**

Add:

```python
class CaptureAlreadyRunningError(RuntimeError):
    pass


class CaptureManager:
    def __init__(self, data_root: Path, client: Esp32Client) -> None:
        self._data_root = data_root
        self._client = client
        self._lock = threading.Lock()
        self._stop_event = threading.Event()
        self._worker: threading.Thread | None = None
        self._state = {
            "running": False,
            "dataset_name": None,
            "interval_ms": DEFAULT_INTERVAL_MS,
            "saved_count": 0,
            "failed_count": 0,
            "latest_file": None,
            "last_error": None,
        }
```

`start()` must validate the interval and dataset name, construct
`DatasetWriter` before starting a thread, reset all counters, set
`running=True`, and name the daemon thread `"dataset-capture"`.

The worker must:

1. Call `wait_for_stream_release(3.0)` once.
2. Try the complete `capture()` plus `writer.save()` operation up to three
   times for each logical frame.
3. Use `self._stop_event.wait(RETRY_DELAY_SECONDS)` between retries.
4. Increment `failed_count` only after all three attempts fail.
5. Reset the consecutive-failure counter after each successful save.
6. Call `self._stop_event.wait(interval_ms / 1000)` after a successful save.
7. Set `running=False` in `finally`.
8. Store Chinese error text in `last_error`.

`snapshot()` must return `dict(self._state)` while holding the lock.

`stop()` must set the event, join the current worker unless called from that
same worker, then return `snapshot()`. `shutdown()` delegates to `stop()`.

- [ ] **Step 4: Run Task 2 tests and commit**

Run:

```bash
python3 -m unittest tests.test_dataset_capture -v
python3 -m py_compile dataset_capture.py
git diff --check
```

Expected: all tests pass.

Commit:

```bash
git add dataset_capture.py tests/test_dataset_capture.py
git commit -m "feat: manage continuous dataset capture"
```

### Task 3: Local JSON API and Chinese Browser Console

**Files:**
- Create: `dataset_capture_server.py`
- Create: `tests/test_dataset_capture_server.py`

**Interfaces:**
- Consumes: `CaptureManager`, `Esp32Client`, `DEFAULT_DEVICE_URL`, and `DEFAULT_INTERVAL_MS`.
- Produces: `create_server(host: str, port: int, data_root: Path, device_url: str) -> ThreadingHTTPServer`.
- Produces HTTP: `GET /`, `GET /api/status`, `POST /api/capture/start`, and `POST /api/capture/stop`.

- [ ] **Step 1: Write failing HTTP API tests**

Create `tests/test_dataset_capture_server.py`. Define
`DatasetCaptureServerTest` and use this fake manager:

```python
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

    def snapshot(self):
        return dict(self.state)

    def start(self, dataset_name, interval_ms):
        self.start_arguments = (dataset_name, interval_ms)
        self.state["running"] = True
        return self.snapshot()

    def stop(self):
        self.stop_calls += 1
        self.state["running"] = False
        return self.snapshot()

    def shutdown(self):
        self.shutdown_calls += 1
```

Create the server on port `0` in `setUp()`, run `serve_forever()` in a daemon
thread, and in `tearDown()` call `shutdown()`, join the thread, and call
`server_close()`.

Test:

```python
status, content_type, body = request("GET", "/")
self.assertEqual(200, status)
self.assertIn("text/html", content_type)
page = body.decode("utf-8")
self.assertIn("开始连续拍照", page)
self.assertIn("停止并保存", page)
self.assertIn("数据集名称", page)
self.assertIn("intervalMs", page)
self.assertIn("http://192.168.4.1:81/stream", page)

status, _, body = request("GET", "/api/status")
self.assertEqual(200, status)
self.assertFalse(json.loads(body)["running"])

status, _, body = request(
    "POST",
    "/api/capture/start",
    {"dataset_name": "wet", "interval_ms": 500},
)
self.assertEqual(200, status)
self.assertEqual(("wet", 500), manager.start_arguments)

status, _, body = request("POST", "/api/capture/stop", {})
self.assertEqual(200, status)
self.assertEqual(1, manager.stop_calls)
```

Also assert:

- Unknown routes return JSON HTTP 404.
- Invalid JSON and request bodies over 4096 bytes return HTTP 400.
- `ValueError` returns HTTP 400.
- `CaptureAlreadyRunningError` returns HTTP 409.
- Unexpected exceptions return HTTP 500 without a Python traceback in the
  response.

- [ ] **Step 2: Run server tests and verify failure**

Run:

```bash
python3 -m unittest tests.test_dataset_capture_server -v
```

Expected: import failure because `dataset_capture_server.py` does not exist.

- [ ] **Step 3: Implement the loopback server and JSON routes**

Implement:

```python
def create_server(
    host: str,
    port: int,
    data_root: Path,
    device_url: str,
    manager: CaptureManager | None = None,
) -> ThreadingHTTPServer:
    active_manager = manager or CaptureManager(
        data_root,
        Esp32Client(device_url),
    )
```

Build a request-handler class inside `create_server()` so it closes over
`active_manager` and `device_url`. Set:

```python
server.capture_manager = active_manager
server.daemon_threads = True
```

Every JSON response must include:

```text
Content-Type: application/json; charset=utf-8
Cache-Control: no-store
```

The start route accepts exactly a JSON object containing `dataset_name` and
`interval_ms`, converts `interval_ms` with `int()`, and calls
`active_manager.start()`. The stop route accepts an empty JSON object and calls
`active_manager.stop()`.

Override `log_message()` to print one concise request line without emitting
per-frame logs.

- [ ] **Step 4: Implement the browser console**

Embed a complete `INDEX_HTML_TEMPLATE` string in
`dataset_capture_server.py` with:

- A native `width:128px;height:128px` `<img id="preview">`.
- `<input id="datasetName" maxlength="64">`.
- `<input id="intervalMs" type="number" min="200" max="60000" value="500">`.
- Buttons with IDs `startButton` and `stopButton`.
- Status fields for running state, saved count, failed count, latest file, and
  last error.

Render the page by replacing `__STREAM_URL__` with a stream URL built from the
hostname in `device_url` and fixed stream port 81. The default rendering must
therefore produce:

```javascript
const streamUrl = "http://192.168.4.1:81/stream";

function stopPreview() {
  preview.removeAttribute("src");
}

function startPreview() {
  preview.src = `${streamUrl}?t=${Date.now()}`;
}
```

The start button must:

1. Disable itself.
2. Call `stopPreview()`.
3. Wait 200 ms.
4. POST `dataset_name` and numeric `interval_ms`.
5. Leave preview stopped on success.
6. Restart preview and show the response error on failure.

The stop button must POST `/api/capture/stop`, refresh status, and call
`startPreview()`.

Poll `/api/status` every 500 ms. On initial page load, start preview only when
`running` is false; a refreshed page during active capture must keep preview
disconnected.

- [ ] **Step 5: Add command-line startup and graceful shutdown**

Use `argparse` options with these defaults:

```python
--port 8000
--device-url http://192.168.4.1
```

In `main()`:

1. Resolve `data_root` as
   `Path(__file__).resolve().parent / "data"`; do not accept a command-line
   override.
2. Create the server with host fixed to `127.0.0.1`.
3. Register `SIGINT` and `SIGTERM` handlers that call
   `server.shutdown()` from a short helper thread.
4. Print the device URL, resolved data root, and local control URL once.
5. Call `serve_forever(poll_interval=0.2)`.
6. In `finally`, call `server.capture_manager.shutdown()` and
   `server.server_close()`.

- [ ] **Step 6: Run Task 3 tests and commit**

Run:

```bash
python3 -m unittest tests.test_dataset_capture_server -v
python3 -m unittest tests.test_dataset_capture -v
python3 -m py_compile dataset_capture.py dataset_capture_server.py
git diff --check
```

Expected: all tests pass.

Commit:

```bash
git add dataset_capture_server.py tests/test_dataset_capture_server.py
git commit -m "feat: add local dataset capture console"
```

### Task 4: Documentation, Full Regression, and Manual Smoke Test

**Files:**
- Modify: `README.md`
- Test: `tests/test_dataset_capture.py`
- Test: `tests/test_dataset_capture_server.py`

**Interfaces:**
- Consumes: the command-line and browser interfaces from Tasks 1–3.
- Produces: a Chinese quick-start section and final verification evidence.

- [ ] **Step 1: Write a failing README structure test**

Add to `tests/test_dataset_capture_server.py`:

```python
def test_readme_documents_local_capture_console(self):
    readme = (ROOT / "README.md").read_text(encoding="utf-8")
    self.assertIn("python3 dataset_capture_server.py", readme)
    self.assertIn("http://127.0.0.1:8000", readme)
    self.assertIn("data/<数据集名称>/", readme)
    self.assertIn("停止并保存", readme)
```

- [ ] **Step 2: Run the README test and verify failure**

Run:

```bash
python3 -m unittest \
  tests.test_dataset_capture_server.DatasetCaptureServerTest.test_readme_documents_local_capture_console \
  -v
```

Expected: failure because the current README says dataset automation is absent.

- [ ] **Step 3: Update README with the current verified workflow**

Correct the stale top-level description so it says the firmware provides native
128×128 MJPEG and single-frame capture. Add a Chinese section containing:

```bash
python3 dataset_capture_server.py
```

Then document:

1. Connect the Mac to the ESP32 SoftAP.
2. Open `http://127.0.0.1:8000`.
3. Enter a dataset name and interval.
4. Press “开始连续拍照”.
5. Press “停止并保存”.
6. Find JPEGs and `metadata.csv` under `data/<数据集名称>/`.

State explicitly that the Python console does not require an ESP32 firmware
rebuild or reflashing because it uses the existing HTTP endpoints.

- [ ] **Step 4: Run all automated verification**

Run:

```bash
python3 -m unittest discover -s tests -v
python3 -m py_compile dataset_capture.py dataset_capture_server.py
git diff --check
git status --short
```

Expected:

- All pre-existing firmware host tests and new Python tests pass.
- Python compilation succeeds.
- `git diff --check` prints nothing.
- Only intentional Task 4 changes remain.

- [ ] **Step 5: Run a localhost smoke test without hardware writes**

Start the server:

```bash
python3 dataset_capture_server.py --port 8001
```

In a second shell:

```bash
curl -s http://127.0.0.1:8001/api/status
curl -s http://127.0.0.1:8001/ | grep "开始连续拍照"
```

Expected:

- Status returns JSON with `"running": false`.
- The page contains “开始连续拍照”.
- `Ctrl-C` shuts the process down cleanly.

Do not start a capture in this smoke test unless the Mac is connected to the
ESP32 SoftAP.

- [ ] **Step 6: Commit documentation**

```bash
git add README.md tests/test_dataset_capture_server.py
git commit -m "docs: explain continuous dataset capture"
```

- [ ] **Step 7: Perform hardware acceptance with the user**

With the Mac connected to `ESP32S3-CAPTURE`:

1. Run `python3 dataset_capture_server.py`.
2. Open `http://127.0.0.1:8000`.
3. Capture 10 frames into a new dataset at 500 ms.
4. Stop and confirm the MJPEG preview reconnects.
5. Count 10 JPEG files and 10 metadata rows.
6. Run `file data/<dataset-name>/*.jpg` and confirm JPEG output.
7. Run `curl http://192.168.4.1/api/status` and confirm capture and stream
   failure counters did not increase.

Record hardware acceptance separately from automated test and localhost smoke
test results. The implementation may be complete before the board is available,
but low-level device behavior is not claimed until this step passes.

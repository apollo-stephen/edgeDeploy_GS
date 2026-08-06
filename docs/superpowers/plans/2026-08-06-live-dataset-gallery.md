# Live Dataset Capture Gallery Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Keep the ESP32 MJPEG preview running during continuous local dataset capture while showing the exact latest saved JPEG and a bounded newest-first gallery of 30 saved JPEGs.

**Architecture:** `CaptureManager` will capture through the existing `/capture` endpoint without waiting for MJPEG disconnection, retain a token-scoped 30-record window, and expose exact saved bytes by token and sequence. The loopback HTTP server will add a non-path-based image route, while a focused page module will own the responsive two-panel HTML and bounded object-URL lifecycle.

**Tech Stack:** Python 3 standard library (`http.server`, `threading`, `deque`, `secrets`, `unittest`), embedded HTML/CSS/JavaScript, Node.js for DOM-lifecycle harness tests, ESP-IDF 5.5.4 for the unchanged firmware build.

## Global Constraints

- The browser gallery is capped at exactly 30 images; on-disk datasets remain unlimited by this feature.
- The right-hand image must come from a JPEG after `DatasetWriter.save()` succeeds, never from the MJPEG preview.
- The MJPEG preview must not be intentionally disconnected when capture starts, runs, stops, or fails.
- The existing CAMERA mutex remains the only camera ownership mechanism; do not add a firmware endpoint in this implementation.
- The supported capture interval remains 200–60,000 ms, with 500 ms as the default.
- Recent-image routing must accept no dataset name, filename, or filesystem path from the caller.
- Each capture start creates a fresh 32-character lowercase hexadecimal session token.
- Status polling remains at 500 ms, with no more than one poll/reconciliation pass in flight.
- Preserve existing dataset naming, JPEG validation, atomic file writes, metadata schema, retry policy, stop synchronization, and safe HTTP error bodies.
- Preserve the user's unrelated `.gitignore` modification and do not stage it.

## File Structure

- Modify `dataset_capture.py`: remove stream-release gating; own session tokens, recent capture records, bounded lookup, and JSON-safe status snapshots.
- Create `dataset_capture_page.py`: render the capture console HTML/CSS/JavaScript from an escaped stream URL and default interval.
- Modify `dataset_capture_server.py`: delegate page rendering and serve recent JPEG bytes through the token/sequence route.
- Modify `tests/test_dataset_capture.py`: cover ungated capture, session boundaries, the 30-record window, and exact recent-byte lookup.
- Modify `tests/test_dataset_capture_server.py`: cover image-route safety, uninterrupted preview behavior, gallery reconciliation, and browser resource bounds.
- Modify `README.md`: document simultaneous preview/capture and the bounded recent gallery.

---

### Task 1: Ungated Capture Worker and Bounded Recent-Capture Model

**Files:**
- Modify: `dataset_capture.py:1-425`
- Test: `tests/test_dataset_capture.py:1-397`

**Interfaces:**
- Consumes: existing `Frame`, `DatasetWriter.save(frame: Frame) -> Path`, and `Esp32Client.capture() -> Frame`.
- Produces: `RECENT_CAPTURE_LIMIT = 30`; `SESSION_TOKEN_BYTES = 16`; `CaptureManager.snapshot() -> dict[str, object]` containing `session_token` and `recent_captures`; `CaptureManager.read_recent_capture(session_token: str, sequence: int) -> bytes`.
- `recent_captures` entries are `{"sequence": int, "filename": str, "captured_at": str}` ordered newest first.

- [ ] **Step 1: Replace the stream-wait expectations with failing manager tests**

Remove `wait_calls` and `wait_for_stream_release()` from `FakeClient`. Replace the client test that waits for stream release with a capture-only test, and add these manager assertions:

```python
from unittest.mock import patch

from dataset_capture import RECENT_CAPTURE_LIMIT


def test_start_captures_without_a_stream_release_api(self):
    client = FakeClient([self.frame])
    manager = CaptureManager(self.data_root, client)

    state = manager.start("wet", 60_000)
    wait_until(self, lambda: manager.snapshot()["saved_count"] == 1)
    manager.stop()

    self.assertRegex(state["session_token"], r"^[0-9a-f]{32}$")
    self.assertEqual(1, client.capture_calls)


def test_recent_captures_are_newest_first_and_bounded(self):
    client = FakeClient([self.frame] * (RECENT_CAPTURE_LIMIT + 1))
    manager = CaptureManager(self.data_root, client)

    with patch("dataset_capture.MIN_INTERVAL_MS", 1):
        manager.start("wet", 1)
        wait_until(
            self,
            lambda: manager.snapshot()["saved_count"] == RECENT_CAPTURE_LIMIT + 1,
        )
        state = manager.stop()

    recent = state["recent_captures"]
    self.assertEqual(RECENT_CAPTURE_LIMIT, len(recent))
    self.assertEqual(
        list(range(RECENT_CAPTURE_LIMIT + 1, 1, -1)),
        [item["sequence"] for item in recent],
    )
    self.assertEqual(JPEG, manager.read_recent_capture(
        state["session_token"], recent[0]["sequence"]
    ))
```

Add the session-boundary test and extend the existing retry/failure assertion:

```python
def test_new_session_clears_recent_captures_and_rejects_old_token(self):
    client = FakeClient([self.frame, self.frame])
    manager = CaptureManager(self.data_root, client)

    manager.start("wet", 60_000)
    wait_until(self, lambda: manager.snapshot()["saved_count"] == 1)
    old_state = manager.stop()

    new_state = manager.start("wet", 60_000)
    try:
        self.assertNotEqual(old_state["session_token"], new_state["session_token"])
        self.assertEqual([], new_state["recent_captures"])
        with self.assertRaises(FileNotFoundError):
            manager.read_recent_capture(old_state["session_token"], 1)
    finally:
        manager.stop()

# In test_worker_retries_three_times_before_recording_one_failure:
self.assertEqual([1], [item["sequence"] for item in state["recent_captures"]])

# In test_worker_stops_after_five_consecutive_failed_frames:
self.assertEqual([], state["recent_captures"])
```

- [ ] **Step 2: Run the manager tests and verify the new contract fails**

Run:

```bash
python3 -m unittest tests.test_dataset_capture.CaptureManagerTest tests.test_dataset_capture.Esp32ClientTest -v
```

Expected: FAIL because `RECENT_CAPTURE_LIMIT`, `session_token`, `recent_captures`, and `read_recent_capture()` do not exist, and the worker still calls `wait_for_stream_release()`.

- [ ] **Step 3: Implement the bounded session model and remove stream-release gating**

In `dataset_capture.py`, import `deque` and `secrets`, remove the now-unused `json` import and all `Esp32Client.fetch_status()`, `_fetch_status()`, and `wait_for_stream_release()` methods, then add:

```python
from collections import deque
import secrets

RECENT_CAPTURE_LIMIT = 30
SESSION_TOKEN_BYTES = 16


@dataclass(frozen=True)
class _RecentCapture:
    sequence: int
    filename: str
    captured_at: str
    path: Path

    def public_metadata(self) -> dict[str, object]:
        return {
            "sequence": self.sequence,
            "filename": self.filename,
            "captured_at": self.captured_at,
        }
```

Initialize manager-owned state with `session_token=None`, `_capture_sequence=0`, and `deque(maxlen=RECENT_CAPTURE_LIMIT)`. In `start()`, after rejecting an already-running worker, assign `secrets.token_hex(SESSION_TOKEN_BYTES)`, reset the sequence and deque, and return a snapshot built while holding the lock.

Use one helper for every returned status so mutable structures do not escape:

```python
def _snapshot_locked(self) -> dict[str, object]:
    state = dict(self._state)
    state["recent_captures"] = [
        record.public_metadata() for record in reversed(self._recent_captures)
    ]
    return state
```

Change the success path in `_run()` from the nested call to:

```python
frame = self._client.capture()
destination = writer.save(frame)
```

Only after both calls succeed, increment `_capture_sequence`, append `_RecentCapture(sequence, destination.name, frame.captured_at.isoformat(), destination)`, and update saved count/latest file under `_lock`. Delete the call to `self._client.wait_for_stream_release(3.0)` entirely.

Add exact recent-byte lookup without accepting caller paths:

```python
def read_recent_capture(self, session_token: str, sequence: int) -> bytes:
    with self._lock:
        if session_token != self._state["session_token"]:
            raise FileNotFoundError("Recent capture is not available")
        record = next(
            (item for item in self._recent_captures if item.sequence == sequence),
            None,
        )
    if record is None:
        raise FileNotFoundError("Recent capture is not available")
    return record.path.read_bytes()
```

Update the existing stop-race test's `BlockingClient` to signal `worker_started` from `capture()` rather than from the removed stream-wait method:

```python
def capture(self):
    self.worker_started.set()
    return super().capture()
```

- [ ] **Step 4: Run storage and manager tests**

Run:

```bash
python3 -m unittest tests.test_dataset_capture -v
```

Expected: all dataset storage, client, manager retry, shutdown, and stop-race tests PASS; the 31-save test reports exactly 30 recent records.

- [ ] **Step 5: Commit the capture model**

```bash
git add dataset_capture.py tests/test_dataset_capture.py
git commit -m "feat: retain recent dataset captures"
```

---

### Task 2: Token-Scoped Recent JPEG Endpoint

**Files:**
- Modify: `dataset_capture_server.py:1-336`
- Test: `tests/test_dataset_capture_server.py:1-459`

**Interfaces:**
- Consumes: `CaptureManager.read_recent_capture(session_token: str, sequence: int) -> bytes` from Task 1.
- Produces: `GET /api/captures/<32-lowercase-hex-token>/<positive-decimal-sequence>` returning exact JPEG bytes or safe JSON 404/500 responses.

- [ ] **Step 1: Add failing endpoint and traversal-boundary tests**

Extend `FakeManager` with `session_token`, `recent_captures`, and an internal byte map:

```python
self.state.update({"session_token": None, "recent_captures": []})
self.capture_payloads = {}
self.capture_read_calls = []

def read_recent_capture(self, session_token, sequence):
    self.capture_read_calls.append((session_token, sequence))
    try:
        return self.capture_payloads[(session_token, sequence)]
    except KeyError as error:
        raise FileNotFoundError("Recent capture is not available") from error
```

Add a successful route test using `token = "a" * 32` and
`self.manager.capture_payloads[(token, 7)] = b"\xff\xd8saved\xff\xd9"`. Request `/api/captures/{token}/7` directly with `urllib`, then assert:

```python
self.assertEqual(200, response.status)
self.assertEqual("image/jpeg", response.headers["Content-Type"])
self.assertEqual("no-store", response.headers["Cache-Control"])
self.assertEqual(str(len(payload)), response.headers["Content-Length"])
self.assertEqual(payload, response.read())
```

Use these malformed paths and assert JSON 404 without manager lookup:

```python
malformed_paths = (
    f"/api/captures/{'A' * 32}/1",
    f"/api/captures/{'a' * 31}/1",
    "/api/captures/../1",
    f"/api/captures/{'a' * 32}/0",
    f"/api/captures/{'a' * 32}/-1",
    f"/api/captures/{'a' * 32}/+1",
    f"/api/captures/{'a' * 32}",
    f"/api/captures/{'a' * 32}/1/extra",
)
for path in malformed_paths:
    with self.subTest(path=path):
        status, content_type, _, body = self.request("GET", path)
        self.assertEqual(404, status)
        self.assertEqual("application/json; charset=utf-8", content_type)
        self.assertIn("error", json.loads(body))
self.assertEqual([], self.manager.capture_read_calls)
```

Request a validly shaped unknown token/sequence, assert that the manager call
is `[("b" * 32, 9)]`, and assert HTTP 404. Add
`self.manager.read_capture_error = RuntimeError("private image detail")` to
the fake, raise it before map lookup, and assert that a valid route returns only
`{"error":"Internal server error"}` with HTTP 500 and does not expose the
private message.

- [ ] **Step 2: Run the server tests and verify the route is missing**

Run:

```bash
python3 -m unittest tests.test_dataset_capture_server.DatasetCaptureServerTest -v
```

Expected: FAIL because the new route returns the existing JSON 404 and no JPEG headers.

- [ ] **Step 3: Implement strict route parsing and JPEG response handling**

Import `re` and add the module-level route expression:

```python
RECENT_CAPTURE_PATH = re.compile(
    r"^/api/captures/([0-9a-f]{32})/([1-9][0-9]*)$"
)
```

Add a handler method that never accepts a path from the request:

```python
def _send_jpeg(self, payload: bytes) -> None:
    self.send_response(200)
    self.send_header("Content-Type", "image/jpeg")
    self.send_header("Cache-Control", "no-store")
    self.send_header("Content-Length", str(len(payload)))
    self.end_headers()
    self.wfile.write(payload)
```

In `do_GET()`, match the URL-split path after the existing `/` and
`/api/status` branches. If it matches, call
`active_manager.read_recent_capture(match.group(1), int(match.group(2)))`.
Map `FileNotFoundError` to HTTP 404 with `{"error":"Recent capture is not available"}`;
map every other unexpected exception to the existing safe HTTP 500 body; send
the JPEG only on success. Any nonmatching capture-style path falls through to
the ordinary route-not-found JSON response.

- [ ] **Step 4: Run endpoint and existing API tests**

Run:

```bash
python3 -m unittest tests.test_dataset_capture_server.DatasetCaptureServerTest -v
```

Expected: all endpoint, status, POST validation, safe-error, and unknown-route tests PASS.

- [ ] **Step 5: Commit the recent-image endpoint**

```bash
git add dataset_capture_server.py tests/test_dataset_capture_server.py
git commit -m "feat: serve recent dataset captures"
```

---

### Task 3: Responsive Two-Panel Page and Persistent Live Preview

**Files:**
- Create: `dataset_capture_page.py`
- Modify: `dataset_capture_server.py:1-247`
- Modify: `tests/test_dataset_capture_server.py:98-345`

**Interfaces:**
- Consumes: `_stream_url(device_url: str) -> str` and `DEFAULT_INTERVAL_MS` from the server.
- Produces: `render_capture_page(stream_url: str, default_interval_ms: int) -> bytes`; DOM IDs `preview`, `previewStatus`, `latestCapture`, `galleryStatus`, `recentGallery`, and all existing control/status IDs.

- [ ] **Step 1: Rewrite the failing page-contract assertions**

Update the HTML assertions to require:

```python
self.assertIn('class="visual-grid"', page)
self.assertIn('id="preview"', page)
self.assertIn('id="previewStatus"', page)
self.assertIn('id="latestCapture"', page)
self.assertIn('id="recentGallery"', page)
self.assertIn('aria-label="最近保存的图片"', page)
self.assertIn('@media (max-width: 700px)', page)
self.assertNotIn('state.running) {\n        stopPreview()', page)
self.assertNotIn('await new Promise(resolve => setTimeout(resolve, 200))', page)
```

Replace the old “start conflict keeps preview disconnected” Node test with
`test_start_stop_and_status_transitions_keep_preview_connected`. Its harness
must resolve initialization, start, a running status poll, stop, and a stopped
status poll while asserting the preview source is assigned exactly once and is
never removed.

Add `test_preview_errors_schedule_only_one_reconnect` with a fake timer queue.
Fire the preview `error` listener twice before running the queued timer and
assert one timer and one additional `src` assignment. Fire `load` and assert
`previewStatus.textContent == "直播中"`.

- [ ] **Step 2: Run the page behavior tests and verify the old lifecycle fails**

Run:

```bash
python3 -m unittest \
  tests.test_dataset_capture_server.DatasetCaptureServerTest.test_index_contains_chinese_controls_and_stream_lifecycle \
  tests.test_dataset_capture_server.DatasetCaptureServerTest.test_start_stop_and_status_transitions_keep_preview_connected \
  tests.test_dataset_capture_server.DatasetCaptureServerTest.test_preview_errors_schedule_only_one_reconnect -v
```

Expected: FAIL because the page is single-column, capture deliberately removes the preview source, and no reconnect status/timer exists.

- [ ] **Step 3: Extract and render a focused page module**

Move `INDEX_HTML_TEMPLATE` out of `dataset_capture_server.py` into
`dataset_capture_page.py`. The new module imports `json` and exposes:

```python
def render_capture_page(stream_url: str, default_interval_ms: int) -> bytes:
    return INDEX_HTML_TEMPLATE.replace(
        "__STREAM_URL__", json.dumps(stream_url)
    ).replace(
        "__DEFAULT_INTERVAL_MS__", str(default_interval_ms)
    ).encode("utf-8")
```

Build the page with this exact high-level DOM:

```html
<main class="card">
  <h1>ESP32 数据集采集</h1>
  <section class="visual-grid">
    <article class="visual-panel">
      <h2>实时画面</h2>
      <img id="preview" width="128" height="128" alt="摄像头实时预览">
      <p id="previewStatus" class="muted">正在连接直播</p>
    </article>
    <article class="visual-panel">
      <h2>已保存图片</h2>
      <img id="latestCapture" width="128" height="128" alt="最新保存的图片">
      <p id="galleryStatus" class="muted">尚未开始采集</p>
      <div id="recentGallery" class="thumbnail-grid"
           aria-label="最近保存的图片"></div>
    </article>
  </section>
  <section class="capture-controls">
    <label>数据集名称
      <input id="datasetName" maxlength="64" autocomplete="off">
    </label>
    <label>拍照间隔（毫秒）
      <input id="intervalMs" type="number" min="200" max="60000"
             value="__DEFAULT_INTERVAL_MS__">
    </label>
    <div class="controls">
      <button id="startButton" type="button" disabled>开始连续拍照</button>
      <button id="stopButton" type="button" disabled>停止并保存</button>
    </div>
  </section>
  <dl>
    <dt>运行状态</dt><dd id="runningState">读取中</dd>
    <dt>已保存</dt><dd id="savedCount">0</dd>
    <dt>失败数</dt><dd id="failedCount">0</dd>
    <dt>最新文件</dt><dd id="latestFile">—</dd>
    <dt>最后错误</dt><dd id="lastError">—</dd>
  </dl>
</main>
```

Use a desktop two-column `.visual-grid`, square non-cropping images, a compact
thumbnail grid, and `@media (max-width: 700px) { .visual-grid { grid-template-columns: 1fr; } }`.

In `dataset_capture_server.py`, import `render_capture_page`, delete the inline
template, and create `index_html = render_capture_page(_stream_url(device_url), DEFAULT_INTERVAL_MS)`.

- [ ] **Step 4: Make preview ownership independent from capture/status state**

Use one reconnect timer and no capture-triggered preview teardown:

```javascript
let previewConnected = false;
let previewReconnectTimer = null;

function startPreview() {
  if (previewConnected) return;
  preview.src = `${streamUrl}?t=${Date.now()}`;
  previewConnected = true;
  previewStatus.textContent = "正在连接直播";
}

function schedulePreviewReconnect() {
  previewConnected = false;
  previewStatus.textContent = "直播断开，正在重连";
  if (previewReconnectTimer !== null) return;
  previewReconnectTimer = setTimeout(() => {
    previewReconnectTimer = null;
    startPreview();
  }, 1000);
}

preview.addEventListener("load", () => {
  previewStatus.textContent = "直播中";
});
preview.addEventListener("error", schedulePreviewReconnect);
```

Call `startPreview()` during page initialization, independent of the first
status response. Delete `stopPreview()`, every running-state preview branch,
the 200 ms start delay, the status-error preview teardown, and the stop-handler
`startPreview()` call. Retain the existing transition/generation guards for
start/stop button correctness.

- [ ] **Step 5: Run the persistent-preview page tests**

Run:

```bash
python3 -m unittest tests.test_dataset_capture_server.DatasetCaptureServerTest -v
```

Expected: all page, API, endpoint, and preview lifecycle tests PASS; source assignment remains one across ordinary capture transitions.

- [ ] **Step 6: Commit the page extraction and live preview**

```bash
git add dataset_capture_page.py dataset_capture_server.py tests/test_dataset_capture_server.py
git commit -m "feat: keep dataset preview live"
```

---

### Task 4: Bounded Browser Gallery Reconciliation

**Files:**
- Modify: `dataset_capture_page.py`
- Modify: `tests/test_dataset_capture_server.py`

**Interfaces:**
- Consumes: status fields `session_token: str | null` and `recent_captures: Array<{sequence:number, filename:string, captured_at:string}>`; image route from Task 2.
- Produces: at most 30 retained blobs/object URLs/thumbnail nodes; newest available JPEG shown in `latestCapture`; one serialized 500 ms status/reconciliation loop.

- [ ] **Step 1: Add a failing Node harness for session changes, deduplication, and eviction**

Extend the existing fake DOM with `document.createElement`, `replaceChildren`,
`append`, a fake `Image` that resolves `onload`, and URL accounting:

```javascript
let nextObjectUrl = 0;
const revokedUrls = [];
global.URL = {
  createObjectURL() { nextObjectUrl += 1; return `blob:${nextObjectUrl}`; },
  revokeObjectURL(value) { revokedUrls.push(value); },
};
global.Image = class {
  set src(value) { this._src = value; queueMicrotask(() => this.onload()); }
};
```

Return status for token `"a".repeat(32)` with sequences 30 down to 1, then a
second status with 31 down to 2. Return one JPEG-like blob response per image
route and record every fetch path. Assert:

```javascript
assert.equal(imageFetches.filter(path => path.endsWith("/30")).length, 1);
assert.equal(elements.get("recentGallery").children.length, 30);
assert.equal(elements.get("latestCapture").src, "blob:31");
assert.deepEqual(revokedUrls, ["blob:30"]);
```

Then return token `"b".repeat(32)` with one capture. Assert all 30 prior object
URLs are revoked, the thumbnail grid has one child, and the main image uses the
new session's URL. Add a delayed image response while triggering another
status interval and assert the same token/sequence URL is fetched only once.
Add an image failure assertion that keeps the previous main image and allows a
later sequence to render.

- [ ] **Step 2: Run the gallery lifecycle test and verify it fails**

Run:

```bash
python3 -m unittest \
  tests.test_dataset_capture_server.DatasetCaptureServerTest.test_recent_gallery_is_bounded_deduplicated_and_session_scoped -v
```

Expected: FAIL because no recent metadata is reconciled and no object URLs or thumbnail nodes are managed.

- [ ] **Step 3: Implement serialized status polling and session-scoped gallery state**

Add the following page state:

```javascript
const loadedCaptures = new Map();
const pendingCaptures = new Set();
let gallerySessionToken = null;
let statusPollRunning = false;

function clearGallery() {
  loadedCaptures.forEach(item => URL.revokeObjectURL(item.url));
  loadedCaptures.clear();
  pendingCaptures.clear();
  recentGallery.replaceChildren();
  latestCapture.removeAttribute("src");
}
```

Key maps and pending entries by numeric capture sequence because every token
change first calls `clearGallery()`. `renderGallery(records)` must create image
nodes only from `loadedCaptures`, in the server's newest-first order, cap the
slice at 30, replace the grid children, and set `latestCapture.src` to the first
loaded record. Use `textContent` for filenames/timestamps; do not insert server
strings with `innerHTML`.

- [ ] **Step 4: Fetch, decode, reconcile, and revoke exact saved JPEGs**

Implement the core operations with token checks around asynchronous work:

```javascript
async function loadRecentCapture(token, record) {
  if (loadedCaptures.has(record.sequence) || pendingCaptures.has(record.sequence)) return;
  pendingCaptures.add(record.sequence);
  try {
    const response = await fetch(
      `/api/captures/${token}/${record.sequence}`,
      {cache: "no-store"},
    );
    if (!response.ok) throw new Error(`图片请求失败 (${response.status})`);
    const url = URL.createObjectURL(await response.blob());
    const candidate = new Image();
    try {
      await new Promise((resolve, reject) => {
        candidate.onload = resolve;
        candidate.onerror = () => reject(new Error("图片解码失败"));
        candidate.src = url;
      });
    } catch (error) {
      URL.revokeObjectURL(url);
      throw error;
    }
    if (token !== gallerySessionToken) {
      URL.revokeObjectURL(url);
      return;
    }
    loadedCaptures.set(record.sequence, {...record, url});
  } finally {
    pendingCaptures.delete(record.sequence);
  }
}
```

`reconcileGallery(state)` must validate `session_token` and
`recent_captures`, clear on token change, take only the first 30 records, revoke
and delete loaded sequences absent from the desired set, await missing loads,
then call `renderGallery(records)`. On a load error, keep the previous main
image and set `galleryStatus` to the error; later records and polls continue.
When no capture is loaded, show “等待第一张已保存图片”; otherwise show
“显示最近 N 张（本轮已保存 M 张）”.

Wrap status synchronization so overlapping `setInterval` ticks return early:

```javascript
async function synchronizeStatus() {
  if (statusPollRunning) return null;
  statusPollRunning = true;
  try {
    const state = await refreshStatus();
    if (state !== null) await reconcileGallery(state);
    return state;
  } catch (error) {
    lastError.textContent = error.message;
    return null;
  } finally {
    statusPollRunning = false;
  }
}
```

Start and stop POST responses must also be reconciled before their handlers
finish so a new session clears the old gallery immediately and the final saved
record remains visible after stop.

- [ ] **Step 5: Run browser lifecycle and full server tests**

Run:

```bash
python3 -m unittest tests.test_dataset_capture_server -v
```

Expected: all tests PASS; the Node harness proves no duplicate fetches, exactly 30 thumbnails, revocation on eviction/session change, and preservation after image errors.

- [ ] **Step 6: Commit the bounded browser gallery**

```bash
git add dataset_capture_page.py tests/test_dataset_capture_server.py
git commit -m "feat: display recent dataset gallery"
```

---

### Task 5: Operator Documentation and End-to-End Verification

**Files:**
- Modify: `README.md:98-130`
- Modify: `tests/test_dataset_capture_server.py:460-470`

**Interfaces:**
- Consumes: completed local capture console behavior from Tasks 1–4.
- Produces: operator instructions that match simultaneous live preview, exact saved-image display, 30-thumbnail browser bound, and complete 400–600-image disk output.

- [ ] **Step 1: Replace the README regression assertions first**

Change `test_readme_documents_local_capture_console` to require Chinese text
covering these exact facts and reject the obsolete disconnection wording:

```python
self.assertIn("采集期间左侧 MJPEG 预览会继续播放", readme)
self.assertIn("右侧显示最新一张已成功保存的 JPEG", readme)
self.assertIn("最近 30 张缩略图", readme)
self.assertIn("磁盘仍会保存完整数据集", readme)
self.assertNotIn("连续采集期间会断开预览", readme)
```

- [ ] **Step 2: Run the README test and verify the old instructions fail**

Run:

```bash
python3 -m unittest \
  tests.test_dataset_capture_server.DatasetCaptureServerTest.test_readme_documents_local_capture_console -v
```

Expected: FAIL because the README still tells the operator that capture disconnects the preview.

- [ ] **Step 3: Update the local dataset capture instructions**

Replace the obsolete paragraph below the five collection steps with concise
Chinese guidance stating:

```text
采集期间左侧 MJPEG 预览会继续播放，后台单帧采集通过摄像头互斥锁与直播交错取帧。
右侧显示最新一张已成功保存的 JPEG，并保留本轮最近 30 张缩略图，便于及时检查清晰度、构图和样本变化。
30 张仅是网页显示上限；磁盘仍会保存完整数据集及 metadata.csv，因此单个类别采集 400–600 张不会被截断。
```

Also state that brief preview jitter is possible during capture and that the
right side updates only after a disk save succeeds.

- [ ] **Step 4: Run targeted and full host verification**

Run:

```bash
python3 -m unittest tests.test_dataset_capture tests.test_dataset_capture_server -v
python3 -m unittest discover -s tests -v
git diff --check
```

Expected: all host tests PASS and `git diff --check` prints no errors.

- [ ] **Step 5: Build the unchanged ESP-IDF firmware integration**

Run from the repository root in the configured ESP-IDF 5.5.4 environment:

```bash
idf.py build
```

Expected: build completes successfully. This verifies that Python/page changes did not disturb the firmware tree; it does not replace hardware concurrency acceptance.

- [ ] **Step 6: Record the hardware acceptance boundary**

Before claiming device-level acceptance, run the console against the ESP32 at
500 ms and 200 ms, then complete a 600-image category. Record whether the live
preview remains usable, the right image matches the saved file, the page stays
at 30 thumbnails, counts match disk/metadata, reconnect works, and no heap,
PSRAM, panic, watchdog, or frame-buffer exhaustion appears. If no device is
available, report these checks explicitly as pending rather than inferring
success from host tests.

- [ ] **Step 7: Commit documentation and verified behavior**

```bash
git add README.md tests/test_dataset_capture_server.py
git commit -m "docs: explain live dataset gallery"
```

---

## Final Review Checklist

- [ ] `git status --short` shows only intentional feature changes and the user's pre-existing `.gitignore` modification.
- [ ] `python3 -m unittest discover -s tests -v` passes from a clean feature index.
- [ ] `git diff --check` reports no whitespace errors.
- [ ] `idf.py build` passes in ESP-IDF 5.5.4.
- [ ] The implementation contains no stream-release wait in dataset collection.
- [ ] The image endpoint cannot resolve caller-supplied names or paths.
- [ ] Browser and manager resource counts remain capped at 30.
- [ ] Hardware-only results are distinguished from host/build verification.

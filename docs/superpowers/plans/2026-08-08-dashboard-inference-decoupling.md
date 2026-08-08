# Dashboard Inference Decoupling Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Keep dashboard inference text current even when its JPEG fails to decode, retain and retry the right-hand inference snapshot, and remove the manual preview/capture controls and FPS copy.

**Architecture:** The browser tracks rendered metadata and displayed inference images with separate sequence numbers. Metadata updates prediction, scores, and timing immediately; image fetch/decode then independently swaps the snapshot only after validation, while the live MJPEG preview reconnects automatically after an error.

**Tech Stack:** ESP-IDF C component, embedded HTML/CSS/JavaScript, Python `unittest`, Node.js browser-behavior harness, host C tests, Ninja ESP32-S3 build.

## Global Constraints

- Keep both `Live preview` and `Inference snapshot` panels.
- Keep the backend `GET /capture` route for external dataset-capture tooling.
- Remove only the dashboard's `Capture now`, `Pause preview`, and `Streaming at up to 15 FPS` UI and related JavaScript.
- Render prediction, scores, and timing as soon as new inference metadata arrives.
- On image retrieval or decode failure, preserve the previous visible snapshot, report an image-only retry message, revoke failed object URLs, and retry on the next metadata poll.
- Do not modify or stage the user's local `.gitignore` change.

---

### Task 1: Decouple dashboard metadata, snapshot, and preview state

**Files:**
- Modify: `tests/test_http_capture_component.py:18-170`
- Modify: `tests/host/http_capture_component_test.c:528-568`
- Modify: `components/HTTP_CAPTURE/dashboard_page.c:3-120`

**Interfaces:**
- Consumes: `GET /api/inference`, `GET /api/inference/image?sequence=N`, response header `X-Inference-Sequence`, and `http://<host>:81/stream`.
- Produces: `renderResult(metadata) -> void`, `startStream() -> void`, and `pollInference() -> Promise<void>` in the embedded dashboard script; the public C interface `http_capture_dashboard_html(void)` remains unchanged.

- [ ] **Step 1: Change the JavaScript behavior test to require metadata-first rendering and retryable image failure**

Rename the test to `test_dashboard_keeps_results_current_when_image_decode_fails`. Give each fake DOM element child storage and event-listener storage, return one score, and require the new result to render while the previous snapshot remains:

```python
function element(id){
  if(!elements.has(id))elements.set(id,{
    id,textContent:'',src:'',className:'',children:[],listeners:{},
    addEventListener(type,listener){this.listeners[type]=listener;},
    append(...children){this.children.push(...children);},
    appendChild(child){this.children.push(child);},
    replaceChildren(){this.children=[];},
  });
  return elements.get(id);
}
```

Use this ready metadata response:

```javascript
{
  ready:true,sequence:1,prediction:'wet',confidence:0.9,age_ms:2,
  timing:{dsp_ms:26,classification_ms:289,anomaly_ms:0},
  scores:[{label:'wet',value:0.9}],
}
```

After the candidate `Image` invokes `onerror`, assert:

```javascript
if(element('inferenceSnapshot').src!=='blob:old')throw new Error('old image changed');
if(element('prediction').textContent!=='wet (0.90000)')throw new Error('result did not update');
if(element('timing').textContent!=='DSP 26 ms · classification 289 ms · anomaly 0 ms'){
  throw new Error('timing did not update');
}
if(element('scores').children.length!==1)throw new Error('scores did not update');
if(!revoked.includes('blob:new'))throw new Error('candidate URL leaked');
const requestsAfterFailure=requestCount;
await pollInference();
if(requestCount!==requestsAfterFailure+2)throw new Error('image was not retried');
```

Keep `setInterval` stubbed so only explicit polling occurs. Configure every image response with the matching sequence header and every candidate image to fail decoding, which proves the pending-image state clears after failure.

- [ ] **Step 2: Change the host HTML contract test to require the simplified UI**

Replace the positive control assertions with negative assertions while keeping the `/capture` route assertion elsewhere in `main()`:

```c
assert(strstr(request.response_body, "Capture now") == NULL);
assert(strstr(request.response_body, "Pause preview") == NULL);
assert(strstr(request.response_body, "Streaming at up to 15 FPS") == NULL);
assert(strstr(request.response_body, "id=\"captureButton\"") == NULL);
assert(strstr(request.response_body, "id=\"streamButton\"") == NULL);
assert(strstr(request.response_body, "window.open") == NULL);
assert(strstr(request.response_body, "stopStream") == NULL);
assert(strstr(request.response_body, "id=\"livePreview\"") != NULL);
assert(strstr(request.response_body, "id=\"inferenceSnapshot\"") != NULL);
assert(strstr(request.response_body, "liveReconnectTimer") != NULL);
assert(strstr(request.response_body, "setTimeout(startStream,1000)") != NULL);
```

Retain the existing assertions for `streamUrl`, `fetch('/api/inference'`, `/api/inference/image?sequence=`, object URL creation/revocation, safe DOM text assignment, responsive layout, and `fetch(`/capture` absence.

- [ ] **Step 3: Run both focused tests to verify they fail for the old behavior**

Run:

```bash
python3 -m unittest \
  tests.test_http_capture_component.HttpCaptureComponentBehaviorTest.test_dashboard_keeps_results_current_when_image_decode_fails \
  tests.test_http_capture_component.HttpCaptureComponentBehaviorTest.test_routes_capture_ownership_and_preview_controls \
  -v
```

Expected: FAIL because the old script updates result text only after successful image decoding and still contains the controls and FPS text.

- [ ] **Step 4: Remove the dashboard-only controls and implement automatic preview reconnection**

In `dashboard_page.c`, remove `.controls`, all `button` CSS, `liveStatus`, the controls `<div>`, `captureButton`, `streamButton`, `streaming`, `stopStream()`, and their event handlers. Replace preview startup/error handling with one reconnect timer:

```javascript
const streamUrl=`http://${location.hostname}:81/stream`;
let liveReconnectTimer=null;
function startStream(){
  liveReconnectTimer=null;
  livePreview.src=`${streamUrl}?t=${Date.now()}`;
}
livePreview.addEventListener('error',()=>{
  if(liveReconnectTimer===null){
    liveReconnectTimer=setTimeout(startStream,1000);
  }
});
```

Keep the initial `startStream()` call. Do not remove or alter the HTTP server's `/capture` route.

- [ ] **Step 5: Split rendered metadata state from displayed and pending image state**

Replace `displayedSequence`/`pendingSequence` with:

```javascript
let renderedSequence=0;
let displayedImageSequence=0;
let pendingImageSequence=0;
let inferenceObjectUrl=null;
let pollRunning=false;
```

After parsing ready metadata, render it before any image request:

```javascript
if(metadata.sequence!==renderedSequence){
  renderResult(metadata);
  renderedSequence=metadata.sequence;
}
if(metadata.sequence===displayedImageSequence){
  inferenceStatus.textContent=`Sequence ${metadata.sequence} · ${metadata.age_ms} ms ago`;
  return;
}
if(metadata.sequence===pendingImageSequence)return;
pendingImageSequence=metadata.sequence;
```

Use `pendingImageSequence` for 409, response-sequence mismatch, decode failure, supersession, catch cleanup, and successful swap. On success, assign `displayedImageSequence=metadata.sequence` without calling `renderResult()` again. Keep candidate decode-before-swap and revoke the failed, superseded, and previous object URLs exactly once.

- [ ] **Step 6: Run the focused tests to verify the new behavior passes**

Run the same command from Step 3.

Expected: both tests PASS; the Node test observes current prediction/timing/scores, an unchanged old snapshot, revoked failed URLs, and a new image request on the next poll.

- [ ] **Step 7: Commit the dashboard behavior change**

```bash
git add components/HTTP_CAPTURE/dashboard_page.c \
  tests/test_http_capture_component.py \
  tests/host/http_capture_component_test.c
git commit -m "fix: keep dashboard inference results current"
```

### Task 2: Document and verify the simplified dashboard

**Files:**
- Modify: `README.md:111-139`

**Interfaces:**
- Consumes: the dashboard behavior produced by Task 1.
- Produces: user instructions distinguishing automatic dashboard behavior from the retained direct `GET /capture` endpoint.

- [ ] **Step 1: Update dashboard usage documentation**

Replace the manual pause/capture instructions and atomic-pair wording with:

```markdown
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
```

Keep the `Direct endpoints` entry documenting `GET /capture`.

- [ ] **Step 2: Run the complete host test suite**

Run:

```bash
python3 -m unittest discover -s tests -v
```

Expected: all tests PASS with no failures or errors.

- [ ] **Step 3: Build the ESP32-S3 firmware**

Run:

```bash
source ~/esp/v5.5.4/esp-idf/export.sh
ninja -C build -j2
```

Expected: exit code 0 and a successfully linked `build/edgeDeploy_GS.bin` with no compiler warnings promoted to errors.

- [ ] **Step 4: Verify the emitted dashboard contract and working tree scope**

Run:

```bash
python3 -m unittest tests.test_http_capture_component -v
git diff --check
git status --short
```

Expected: tests PASS, `git diff --check` emits no output, and only `README.md` plus the pre-existing user-owned `.gitignore` change remain unstaged after Task 1's commit.

- [ ] **Step 5: Commit documentation without staging `.gitignore`**

```bash
git add README.md
git commit -m "docs: describe automatic inference dashboard"
```

- [ ] **Step 6: Perform hardware acceptance after flashing**

Flash the built firmware using the user's selected serial port, join `ESP32S3-CAPTURE`, and open `http://192.168.4.1/`. Confirm serial predictions continue, dashboard prediction/scores/timing update within one poll, the right snapshot remains visible or retries after a decode failure, the live preview reconnects after a transient stream interruption, and no capture/pause buttons or FPS text appear.

# Live Dataset Capture Gallery Design

## Purpose

The local dataset capture console will keep the ESP32 MJPEG preview visible
while continuous dataset capture is running. The page will show the exact most
recent JPEG saved to disk and a bounded gallery of the 30 most recent saved
JPEGs from the current capture session.

The dataset on disk remains complete. The 30-image limit applies only to the
browser gallery, so a category may contain 400–600 or more images without the
page accumulating an equally large DOM or browser-memory footprint.

## Current State and Constraints

`dataset_capture_server.py` currently disconnects the MJPEG `<img>` before it
starts continuous capture. `CaptureManager` then waits until the device reports
that the stream client has disconnected before requesting `/capture`. This
guarantees exclusive access but prevents the operator from judging framing,
motion, and sample quality during collection.

The current firmware already serializes every camera-frame owner with the
CAMERA component mutex. The inference implementation on the latest main branch
has also established the intended concurrency pattern: the MJPEG handler
releases the camera mutex between stream frames, and a periodic background
consumer can acquire a frame during that gap. Dataset collection will reuse
that pattern rather than introducing a second camera path or extracting frames
from the browser preview.

The saved image is the source of truth for the right-hand display. A frame seen
in the live MJPEG preview must never be presented as though it were a JPEG that
was successfully written to the dataset.

## Considered Approaches

### Selected: latest saved image plus a bounded recent gallery

The page shows the latest saved JPEG at an inspectable size and the 30 most
recent saved JPEGs as thumbnails. The local server retains only the 30 current
session records needed to serve that gallery. Browser image URLs are created
only for these records and are revoked when records leave the window.

This gives the operator short-term visual history while keeping CPU, network,
DOM, and memory use bounded independently of total dataset size.

### Rejected: latest saved image only

This is the lightest option, but it makes it difficult to spot a run of blurry,
duplicated, poorly framed, or insufficiently varied samples.

### Deferred: virtualized gallery for every saved image

A complete 400–600 image browser would require pagination or virtual scrolling,
selection state, and directory-history semantics. It is not needed for live
capture judgment. Complete files remain available under `data/<dataset>/` for
offline review, and a full gallery can be designed separately if needed.

## Page Layout

At desktop width, the console uses two primary columns:

1. **Live preview** on the left. The existing MJPEG source remains connected
   while capture is active. Its status indicates connecting, live, or retrying.
2. **Saved captures** on the right. The newest successfully saved JPEG appears
   as the main image. Beneath it, a newest-first grid contains at most 30
   thumbnails from the current session.

The dataset name, interval, start/stop controls, counts, latest filename, and
last error remain visible in a compact status and controls section. At narrow
viewport widths, the two visual columns stack while preserving live preview
first and saved captures second.

The 128×128 source images may be displayed larger for inspection, but the page
must preserve their square aspect ratio and avoid cropping. The empty gallery
states explicitly distinguish “not started” from “waiting for first saved
image.”

The main saved image follows the newest successful capture. Thumbnails provide
sequence context rather than a separate dataset-management workflow; deletion,
label editing, and full-history browsing are outside this change.

## Capture Concurrency

Starting continuous capture will no longer remove the preview `src`, delay for
preview teardown, or call `wait_for_stream_release()`. The capture worker will
request `/capture` at the configured interval while the MJPEG client remains
connected.

The ESP32 CAMERA mutex remains the only camera ownership mechanism:

1. The stream handler acquires one frame, sends it, releases the frame and
   mutex, and observes its target frame period.
2. A `/capture` request waits for the mutex, acquires a fresh frame, returns the
   JPEG, and releases the frame and mutex.
3. The stream resumes with a later frame.

Short preview jitter is acceptable when a capture request wins the mutex. The
preview must not intentionally disconnect merely because capture is running.
If the MJPEG connection fails due to transient contention or networking, the
browser retries it with a single bounded reconnect timer while capture and
status polling continue.

No firmware API change is required for the initial implementation. Hardware
acceptance is still mandatory because host tests can verify page and ownership
logic but cannot prove timing on the ESP32 camera driver and SoftAP.

## Recent Capture Model

`CaptureManager` will assign a fresh opaque session token whenever capture
starts and a monotonically increasing capture sequence to each successfully
saved JPEG in that session. The token is a fixed-length lowercase hexadecimal
value generated with Python's `secrets` module; it identifies a run across
delayed requests and local-server restarts but is not an authorization secret.
A recent-capture record contains only public metadata needed by the page plus
an internal immutable path:

- capture sequence;
- saved filename;
- capture timestamp;
- internal saved path.

The manager owns a `deque` capped at 30 records. It is cleared when a new
capture session starts, even if the operator reuses an existing dataset name.
The dataset writer's on-disk index may continue from an existing directory;
session token, capture sequence, and filename index are deliberately separate
concepts.

The manager appends a record only after `DatasetWriter.save()` returns
successfully. Failed capture, validation, or disk writes increment existing
failure state but never create gallery entries.

The status snapshot adds the current `session_token` and a JSON-safe
`recent_captures` list ordered newest first. Internal filesystem paths are not
exposed through this list. Existing status fields remain available for
compatibility.

## Local Image Endpoint

The loopback server adds a read-only endpoint for a recent capture:

```text
GET /api/captures/<session-token>/<capture-sequence>
```

The handler accepts only the exact lowercase hexadecimal token form and a
canonical positive decimal sequence, asks the manager for the corresponding
record in its current session and bounded window, and serves the immutable JPEG
with `Content-Type: image/jpeg`, `Cache-Control: no-store`, and an exact
`Content-Length`.

Unknown, expired, malformed, or previous-session identifiers return an error
and never cause a caller-provided path to be resolved. Including the session
token prevents a delayed request for capture 1 from one run from being
satisfied by capture 1 of the next run, including after a local-server restart.
The endpoint therefore does not accept dataset names or filenames and cannot be
used for directory traversal. A record that expires between status polling and
its image request may return not found; the browser treats that as a skipped
thumbnail and continues.

## Browser Data Flow and Resource Bounds

The existing 500 ms status polling remains sufficient. Only one status poll
and one reconciliation pass may be active at a time.

For each successful status response, the browser:

1. checks `session_token`, clearing and revoking the previous gallery when the
   session changes;
2. reads the newest-first `recent_captures` metadata;
3. identifies sequences not already loaded or in flight;
4. fetches each missing JPEG from the loopback image endpoint using the token
   and sequence;
5. creates and decodes an object URL before rendering it;
6. orders successfully decoded records according to server metadata;
7. points the main saved image at the newest available record;
8. removes records outside the server's 30-entry window and revokes their
   object URLs.

Repeated status responses do not redownload an existing sequence. An in-flight
set prevents overlapping polls from requesting the same sequence twice. DOM
nodes, retained blobs, object URLs, and server-side recent records are all
bounded at 30.

The live preview is loaded directly from the ESP32 stream endpoint, as it is
today. Saved captures are loaded only through the loopback server after disk
save succeeds. These paths remain visually and semantically distinct.

## State Transitions and Error Handling

- **Initialization:** controls stay disabled until the first valid status
  response. The live preview starts independently of capture state.
- **Start:** validate name and interval, clear the current recent-gallery
  state, start the worker, and leave the preview connected.
- **Running:** status, counts, errors, latest saved image, and thumbnails update
  without changing preview ownership.
- **Stop:** stop and join the worker as before. The final main image and recent
  gallery remain visible; the preview was never intentionally removed.
- **Automatic failure stop:** preserve the latest complete image and gallery,
  report the terminal capture error, and leave preview reconnection active.
- **Preview failure:** report retrying and schedule at most one reconnect timer.
  Do not stop dataset capture.
- **Gallery image failure:** retain the previous main image, mark or skip the
  failed new thumbnail, and allow later status polls and captures to proceed.
- **Status failure:** retain the last complete UI state, disable unsafe control
  transitions as appropriate, and report the error without destroying loaded
  images.

## Testing

Python manager tests will verify:

- stream-release waiting is not part of the capture loop;
- a recent record appears only after a successful disk save;
- records are newest-first and capped at exactly 30;
- a new session clears the previous session's records;
- failed captures do not add records;
- recent-image lookup rejects expired and previous-session identifiers.

Loopback HTTP tests will verify:

- the page contains the two visual panels and 30-image gallery behavior;
- starting capture never removes the MJPEG preview source;
- running status never causes an intentional preview disconnect;
- preview retry state cannot create duplicate reconnect timers;
- image responses contain the exact saved JPEG and required headers;
- path-like, negative, zero, unknown, and stale IDs cannot read arbitrary files;
- switching session tokens clears the browser gallery and revokes its object
  URLs;
- status reconciliation does not redownload known or in-flight images;
- object URLs are revoked when records leave the 30-entry window;
- existing start, stop, validation, shutdown, and error behavior remains intact.

The full host suite and ESP-IDF firmware build must still pass.

## Hardware Acceptance

With a Mac connected to the ESP32 SoftAP:

1. Start a dataset session at 500 ms and confirm the left MJPEG preview remains
   usable while the exact saved images update on the right.
2. Repeat at the minimum supported 200 ms interval and confirm contention may
   reduce preview smoothness but does not intentionally disconnect it.
3. Collect at least 600 images in one category and confirm the directory and
   metadata contain the complete run while the page retains no more than 30
   thumbnails.
4. Confirm saved and failed counts, newest filename, right-hand image, and
   on-disk files agree.
5. Confirm stop, restart, transient stream reconnection, and an induced capture
   failure do not freeze the page or exhaust browser, heap, PSRAM, or camera
   frame buffers.

If the 200 ms hardware run consistently starves `/capture` or breaks MJPEG
recovery, firmware-side scheduling or frame publication will require a separate
design. The initial implementation will not add that complexity without
hardware evidence.

## Non-Goals

- Displaying or virtualizing all 400–600 images in the live page.
- Deleting, relabeling, or moving dataset images from the browser.
- Extracting dataset samples from the browser's MJPEG display.
- Changing image resolution, JPEG quality, metadata schema, or dataset naming.
- Replacing the existing ESP32 camera mutex or adding a new firmware endpoint
  before hardware testing demonstrates a need.

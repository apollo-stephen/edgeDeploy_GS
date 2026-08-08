# Dashboard Inference Decoupling Design

## Problem

The device is classifying successfully, but the dashboard leaves `Latest
result` unchanged when the inference JPEG cannot be decoded. The current
browser flow deliberately calls `renderResult(metadata)` only after a candidate
image has decoded. Its regression test also requires the old result/image pair
to remain unchanged on a decode failure.

This makes an image transport or decoding fault look like an inference fault.
The screenshot confirms that metadata polling reached the image step: the
specific `image decode failed` message can only occur after metadata returned a
ready sequence and the image endpoint returned a successful, matching response.

## Chosen behavior

Keep both visual panels. The live preview starts automatically, and the right
panel continues to show the exact JPEG associated with inference. Remove the
`Capture now` and `Pause preview` controls and remove the normal `Streaming at
up to 15 FPS` status line.

Keep the backend `/capture` route because external dataset-capture tooling uses
it. Only its dashboard button and event handler are removed.

## Data flow

Track metadata and image publication independently:

1. Poll `/api/inference` once per second.
2. When a new ready sequence arrives, render its prediction, scores, and timing
   immediately and record the rendered metadata sequence.
3. If the image sequence is behind, request
   `/api/inference/image?sequence=N` and validate the response sequence.
4. Decode the image into a candidate object URL before replacing the visible
   inference snapshot.
5. If image retrieval or decoding fails, retain the previous visible snapshot,
   show an image-only retry message, and try again on the next poll. The textual
   result remains current.

Separate `renderedSequence`, `displayedImageSequence`, and `pendingImageSequence`
state prevents a successful metadata update from suppressing image retries.

## Live preview behavior

Assign the MJPEG stream URL on page load. If the image emits an error, schedule
one reconnect with a cache-busting query value. No visible normal-rate or pause
status is needed, and repeated errors must not create overlapping reconnect
timers.

## Error handling

- Metadata failures may report a dashboard update error because prediction data
  is unavailable.
- Image failures affect only the inference image status and do not roll back
  prediction text, scores, or timing.
- HTTP 409 remains a normal stale-sequence race; clear pending image state and
  retry using the next metadata response.
- Candidate object URLs are revoked on failure or supersession, and replaced
  visible URLs are revoked after a successful swap.

## Alternatives considered

1. Remove the inference snapshot. This avoids image failures but loses the
   exact classified frame, which the user requires.
2. Keep the atomic metadata/image pair and only remove controls. This preserves
   the current failure mode, so it does not solve the reported problem.
3. Assign the response blob directly without candidate decoding. This is
   simpler but can replace a good snapshot with a broken image.

## Tests and verification

- Change the Node dashboard behavior test so a simulated image decode failure
  must still update prediction, scores, and timing while preserving the old
  image and revoking the failed object URL.
- Change the host page assertions to require the two controls and FPS text to
  be absent while automatic stream startup, image retry, and inference routes
  remain present.
- Run the focused dashboard tests, the full Python suite, and an ESP32-S3
  firmware build. Hardware verification must confirm that serial inference and
  dashboard text continue updating even if the snapshot temporarily fails.

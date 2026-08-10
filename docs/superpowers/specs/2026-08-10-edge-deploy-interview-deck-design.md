# EdgeDeploy Four-Slide Interview Deck Design

## Communication job

By the end of a four-to-six-minute interview presentation, an embedded/RTOS
interviewer should understand that EdgeDeploy is a completed and evidence-backed
ESP32-S3 edge-classification system whose strongest engineering work is resource
ownership, inference acceleration, low-level fault diagnosis, and optional
runtime observability.

## Scope and factual boundaries

- Deliver one editable 16:9 PowerPoint deck with exactly four slides.
- Use the accepted `v0.3.0-runtime-health-dashboard` source and evidence.
- Describe the model as an INT8 MobileNetV1 three-class classifier, not YOLO or
  object detection.
- Describe the dual-core design as CPU1 inference affinity and load isolation,
  not as a modified scheduler or proven hard-real-time system.
- Do not claim P99, WCET, deadline guarantees, multi-hour soak results,
  automatic recovery, cloud reporting, OTA, a display task, or a queue-based
  capture pipeline.
- User-supplied board photographs, dashboard screenshots, and demo images are
  intentionally left as clearly labelled replaceable image placeholders.

## Visual system

- Use the Codex Grid light layout system as the composition reference: white
  canvas, black primary type, pale-gray structural areas, thin gray rules, and a
  restrained light-blue/blue accent.
- Use a Chinese-capable sans-serif font with Arial fallback while preserving the
  template's hierarchy. Deck title is at least 50 pt, slide titles at least 35
  pt, subheads at least 24 pt, and body text at least 16 pt.
- Avoid gradients, heavy shadows, decorative icons, dense card dashboards, and
  repeated slide silhouettes.
- Build all system, sequence, state, and debugging diagrams from editable native
  PowerPoint shapes. Create connectors before nodes so arrows stay behind
  labels. Use direct labels and no legend when the diagram is self-explanatory.
- Every slide includes speaker-note sources for non-trivial claims.

## Slide 1 — A complete edge-classification loop on ESP32-S3

Narrative job: establish what was built and why it is credible.

Composition: adapt Codex Grid slide 08. Use a minimal large title and short
positioning copy on the left. Keep a large 4:3 image placeholder on the right,
labelled `替换：实板 / 推理网页演示图`. Add a restrained evidence line rather
than a grid of feature cards.

Visible content:

- Title: `ESP32-S3 端侧视觉分类与运行监控`
- Subtitle: `OV5640 · FreeRTOS · INT8 MobileNetV1 · 本地 HTTP 仪表盘`
- One-sentence outcome: native 128×128 JPEG capture, on-device three-class
  classification, same-frame result publication, and optional health monitoring.
- Evidence line: `v0.3.0 已完成构建、烧录、手机端验收与 59 项回归测试`.

## Slide 2 — One mutex protects capture; one sequence protects meaning

Narrative job: explain the true runtime architecture and the two distinct
consistency problems it solves.

Composition: adapt Codex Grid slide 15, preserving its left interpretation rail
and dominant right-side field while replacing the four topic rows with one
architecture diagram. The right field contains a left-to-right native-shape
diagram:

`OV5640 → CAMERA mutex → CPU1 inference → JPEG decode / 96×96 resize → INT8
MobileNetV1 → double buffer + sequence → HTTP dashboard`.

Branch `MJPEG /capture` from CAMERA mutex. Place the on-demand, unpinned
`runtime_health` task below the HTTP/dashboard side. Use subtle CPU0/CPU1 labels
as context, not hard partitions.

The left rail contains three short points:

1. camera framebuffer is released after decode, before classification;
2. staging/published JPEG buffers are swapped under the snapshot mutex;
3. HTTP copies the JPEG for the requested sequence and rejects stale requests.

Add a small replaceable image frame only if it does not reduce diagram
legibility; otherwise reserve all user imagery for slides 1 and 4.

## Slide 3 — The bottleneck was the execution path, not the watchdog

Narrative job: demonstrate quantified optimization and root-cause debugging.

Composition: adapt Codex Grid slide 20. Replace the chart with a two-bar
before/after comparison occupying the left half:

- reference INT8 convolution: `13.6 s`;
- bundled ESP-NN path: `0.289 s`;
- annotation: `约 47×`.

The upper-right message explains that ESP-NN fixed the compute path while CPU1
affinity separated inference from Wi-Fi/HTTP load. The lower-right field is a
compact editable fault chain:

`CORRUPT HEAP at reset/free → inspect scratch address → only 4-byte aligned →
ESP-NN requires 16-byte alignment → aligned calloc → regression + board pass`.

Include `EI_MAX_OVERFLOW_BUFFER_COUNT: 30 → 256` as a secondary one-line lesson,
explicitly described as pointer bookkeeping rather than an out-of-memory fix.

## Slide 4 — Observability is optional, bounded, and verified

Narrative job: close with operational design, evidence, and honest boundaries.

Composition: adapt Codex Grid slide 11. Preserve its two-column comparison
silhouette: the left side becomes a compact editable health lifecycle/state
diagram, while the right side becomes the user image placeholder. The top
interpretation band states why the monitor is optional and bounded.

`OFF → STARTING → HEALTHY ↔ DEGRADED`, with manual stop or 10-second lease expiry
returning to `OFF`. Label the degradation rules: no successful result for six
seconds or three consecutive failures.

The right side contains a large portrait image placeholder labelled
`替换：手机健康仪表盘截图`. Along the bottom, use a single evidence strip with
three quantified facts rather than three UI cards:

- `59 / 59` host tests;
- inference/health task stack remainder `5.1 / 3.1 KiB` in the accepted sample;
- PSRAM free/minimum `7.88 / 7.37 MB` in the accepted sample.

Close with a small boundary line: the accepted scope does not claim deadline,
P99, multi-hour soak, fault injection, or automatic recovery.

## Image placeholder contract

- Placeholders have a thin dashed blue outline, light fill, centered replacement
  label, and a short recommended content caption.
- They remain standard PowerPoint shapes so the user can replace them without
  changing the surrounding layout.
- No synthetic board, dashboard, or garbage-classification image is generated.

## Source set

- `README.zh-CN.md`
- `components/CAMERA/CAMERA.c`
- `components/INFERENCE/inference.cpp`
- `components/HEALTH/health.c`
- `components/HTTP_CAPTURE/http_capture.c`
- `docs/experience/edge-impulse-esp32s3-deployment.md`
- `docs/acceptance/v0.3.0-runtime-health-dashboard.md`

## Acceptance criteria

- Exactly four slides, 16:9, editable in PowerPoint.
- All requested diagrams are present and editable.
- All user-provided image areas are explicit placeholders.
- No title wraps, body text is at least 16 pt, and no object extends outside the
  canvas.
- Every slide renders without unintended overlap, clipping, broken connectors,
  missing glyphs, or unresolved template sample text.
- Speaker notes include source blocks for all non-trivial technical claims.
- The deck is rendered and inspected slide by slide, and the overflow checker
  passes before delivery.

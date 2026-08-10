# EdgeDeploy Four-Slide Interview Deck Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Produce a four-slide, editable 16:9 PowerPoint that explains the accepted EdgeDeploy architecture, performance optimization, low-level fault diagnosis, optional health monitoring, and verification evidence.

**Architecture:** A single JavaScript ES module built with `@oai/artifact-tool` will create all four slides and export the final PPTX. Shared helpers will own typography, slide chrome, editable connectors, nodes, image placeholders, source notes, and deterministic preview export; every slide will remain independently renderable and inspectable.

**Tech Stack:** Node.js, `@oai/artifact-tool`, native PowerPoint shapes/charts/notes, bundled presentation render and overflow tools.

## Global Constraints

- The deck contains exactly four 1280×720 slides.
- The accepted model is an INT8 MobileNetV1 three-class classifier, not YOLO or object detection.
- The dual-core claim is CPU1 inference affinity and load isolation, not a custom scheduler or hard-real-time guarantee.
- Use white, black, pale gray, and restrained light-blue/blue accents from Codex Grid.
- Deck titles are at least 50 pt, slide titles at least 35 pt, subheads at least 24 pt, and body text at least 16 pt.
- System, sequence, state, and debugging diagrams use editable native PowerPoint shapes and connectors.
- User board/dashboard imagery remains replaceable PowerPoint placeholder shapes.
- Every slide has a `[Sources]` block in speaker notes.
- Preserve the user's unrelated `.gitignore` modification.

---

### Task 1: Create the artifact-tool deck workspace and shared primitives

**Files:**
- Create: `/private/tmp/edgedeploy-interview-deck-20260810/deck.mjs`
- Create: `/private/tmp/edgedeploy-interview-deck-20260810/source-notes.txt`
- Create: `/private/tmp/edgedeploy-interview-deck-20260810/output/`

**Interfaces:**
- Consumes: `Presentation`, `PresentationFile` from `@oai/artifact-tool`.
- Produces: `addText(slide, config)`, `addNode(slide, config)`, `addConnector(slide, config)`, `addImagePlaceholder(slide, config)`, `addSlideNumber(slide, number)`, `addSources(slide, urls)`, and `writeBlob(path, blob)`.

- [ ] **Step 1: Initialize the artifact-tool workspace**

Run:

```bash
node "$SKILL_DIR/container_tools/setup_artifact_tool_workspace.mjs" \
  --workspace /private/tmp/edgedeploy-interview-deck-20260810
```

Expected: the workspace contains the artifact-tool package and package metadata.

- [ ] **Step 2: Create the shared deck module**

Use `apply_patch` to add `deck.mjs` with:

```js
import fs from "node:fs/promises";
import { Presentation, PresentationFile } from "@oai/artifact-tool";

const deck = Presentation.create({ slideSize: { width: 1280, height: 720 } });
const C = {
  canvas: "#FFFFFF", ink: "#000000", muted: "#5E6673",
  panel: "#EDEDED", rule: "#B8BCC4", accent: "#6DCBF4",
  accentStrong: "#3D8DFF", paleBlue: "#EAF5FB",
};
const FONT = "Arial";
```

Implement the named helpers using config-first native PowerPoint shapes. `addConnector` must be called before the nodes it links and must use solid lines with an arrowhead at the destination. `addImagePlaceholder` must create a pale-blue rectangle with a dashed blue border and centered replacement copy.

- [ ] **Step 3: Add deterministic export and note helpers**

Implement:

```js
async function writeBlob(path, blob) {
  await fs.writeFile(path, new Uint8Array(await blob.arrayBuffer()));
}

function addSources(slide, urls) {
  slide.addNotes(`[Sources]\n${urls.map((url) => `- ${url}`).join("\n")}`);
}
```

At the end of `main()`, export every slide to PNG and layout JSON, export a montage, inspect all slide/text/shape/chart/notes objects, and save `EdgeDeploy-ESP32S3-面试项目介绍.pptx`.

- [ ] **Step 4: Run a syntax check**

Run:

```bash
node --check /private/tmp/edgedeploy-interview-deck-20260810/deck.mjs
```

Expected: exit status 0 with no output.

### Task 2: Build slides 1 and 2

**Files:**
- Modify: `/private/tmp/edgedeploy-interview-deck-20260810/deck.mjs`

**Interfaces:**
- Consumes: shared shape, connector, placeholder, note, and chrome helpers from Task 1.
- Produces: `buildSlide1(deck)` and `buildSlide2(deck)`.

- [ ] **Step 1: Implement the opening slide**

Adapt Codex Grid slide 08 proportions. Add the exact visible title `ESP32-S3 端侧视觉分类与运行监控`, subtitle `OV5640 · FreeRTOS · INT8 MobileNetV1 · 本地 HTTP 仪表盘`, one-sentence outcome, `v0.3.0` evidence line, and the right-side placeholder `替换：实板 / 推理网页演示图`.

- [ ] **Step 2: Implement the architecture slide connectors**

Create all connectors first for the following graph:

```text
OV5640 -> CAMERA mutex -> CPU1 inference -> JPEG decode / 96×96 resize
       -> INT8 MobileNetV1 -> double buffer + sequence -> HTTP dashboard
CAMERA mutex -> MJPEG /capture
runtime_health -> HTTP dashboard
```

Use one connector color for the main data path, gray for branches, and a dashed blue connector for health telemetry.

- [ ] **Step 3: Add architecture nodes and interpretation rail**

Add short labels only. The left rail states: release camera framebuffer after decode; swap staging/published JPEG under the snapshot mutex; copy the JPEG for the requested sequence and reject stale sequence requests.

- [ ] **Step 4: Render slides 1 and 2**

Run the module and inspect `slide-01.png` and `slide-02.png` at full size. Expected: all Chinese glyphs render, no title wraps, arrows do not cross labels, and the placeholder is visibly replaceable.

### Task 3: Build slides 3 and 4

**Files:**
- Modify: `/private/tmp/edgedeploy-interview-deck-20260810/deck.mjs`

**Interfaces:**
- Consumes: shared helpers and the first two slide patterns.
- Produces: `buildSlide3(deck)` and `buildSlide4(deck)`.

- [ ] **Step 1: Implement the performance comparison chart**

Add an editable column chart with categories `参考 INT8 卷积` and `ESP-NN 优化`, values `13.6` and `0.289`, no legend, data labels, and a prominent `约 47×` annotation. The y-axis unit is seconds.

- [ ] **Step 2: Implement the debugging chain**

Create connectors first, then six compact nodes:

```text
CORRUPT HEAP at reset/free -> inspect scratch address -> 4-byte aligned
-> ESP-NN requires 16-byte alignment -> aligned calloc -> regression + board pass
```

Add the secondary line `overflow 指针槽：30 → 256（约 1 KiB bookkeeping）`.

- [ ] **Step 3: Implement the health state diagram and placeholder**

Create connectors first, then nodes for `OFF`, `STARTING`, `HEALTHY`, and `DEGRADED`. Label the transitions with first success, six-second staleness / three consecutive failures, recovery, manual stop, and ten-second lease expiry. Add the right-side placeholder `替换：手机健康仪表盘截图`.

- [ ] **Step 4: Add final evidence and boundary copy**

Add one bottom evidence strip containing `59 / 59`, `5.1 / 3.1 KiB`, and `7.88 / 7.37 MB`, with concise labels. Add the boundary sentence that this release does not claim deadline, P99, multi-hour soak, fault injection, or automatic recovery.

- [ ] **Step 5: Render slides 3 and 4**

Run the module and inspect `slide-03.png` and `slide-04.png` at full size. Expected: chart labels are legible, debug arrows remain behind nodes, the state diagram reads left-to-right, and the portrait placeholder is large enough for the accepted phone screenshot.

### Task 4: Perform full visual and structural QA

**Files:**
- Modify: `/private/tmp/edgedeploy-interview-deck-20260810/deck.mjs` only if QA identifies a defect.
- Create: `/Users/stephenapollo/Desktop/proj/edgeDeploy_GS/EdgeDeploy-ESP32S3-面试项目介绍.pptx`

**Interfaces:**
- Consumes: the complete four-slide presentation.
- Produces: the final PPTX and verified render evidence.

- [ ] **Step 1: Render the exported PPTX independently**

Run:

```bash
python "$SKILL_DIR/container_tools/render_slides.py" \
  /Users/stephenapollo/Desktop/proj/edgeDeploy_GS/EdgeDeploy-ESP32S3-面试项目介绍.pptx
```

Expected: four slide PNGs render successfully.

- [ ] **Step 2: Create and inspect the montage**

Run:

```bash
python "$SKILL_DIR/container_tools/create_montage.py" \
  --input_dir /Users/stephenapollo/Desktop/proj/edgeDeploy_GS/EdgeDeploy-ESP32S3-面试项目介绍 \
  --output_file /private/tmp/edgedeploy-interview-deck-20260810/final-montage.png
```

Inspect the montage for narrative flow and inspect each PNG at full size for wrapping, clipping, incorrect labels, and inconsistent spacing.

- [ ] **Step 3: Run the overflow checker**

Run:

```bash
python "$SKILL_DIR/container_tools/slides_test.py" \
  /Users/stephenapollo/Desktop/proj/edgeDeploy_GS/EdgeDeploy-ESP32S3-面试项目介绍.pptx
```

Expected: no content-overflow failures.

- [ ] **Step 4: Verify deck contents and notes**

Inspect the final deck and verify: four slides, no sample template text, all four titles, two image placeholders, architecture diagram, performance chart, debug chain, health state diagram, and one `[Sources]` note block per slide.

- [ ] **Step 5: Deliver the final deck**

Return only the final PPTX as the presentation output. Do not expose temporary scripts, layout JSON, montage files, or scratch notes unless requested.

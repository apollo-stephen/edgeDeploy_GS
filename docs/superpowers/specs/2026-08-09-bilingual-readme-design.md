# Bilingual README Design

## Goal

Present EdgeDeploy GS clearly to both English- and Chinese-speaking readers,
with English as the default GitHub landing page and an obvious language link
between equivalent documents.

## File structure

- `README.md` is the default English document.
- `README.zh-CN.md` is the complete Simplified Chinese document.
- Both documents begin with reciprocal language links: `English | 中文`.
- Both documents embed the repository-root `演示.gif` near the introduction.
- The two documents use the same section order so readers can switch languages
  without losing their place conceptually.

GitHub Markdown has no native language tabs. Separate files with reciprocal
links provide predictable navigation without scripts, generated pages, or an
additional documentation hierarchy.

## Content structure

Each language version contains:

1. Project title and one-paragraph overview.
2. Demonstration GIF.
3. Main capabilities.
4. Hardware and software requirements.
5. Camera and modev2 inference pipeline.
6. SoftAP configuration.
7. Build, flash, and serial-monitor commands.
8. Dashboard usage and inference behavior.
9. Local continuous dataset collection.
10. HTTP endpoint reference.
11. Host test command and hardware verification boundary.

The active deployment is `modev2`; `modev1` remains available for rollback but
is not compiled. The README must describe the corrected resize workspace: the
128x128 RGB capture and FIT_SHORTEST intermediate image require a 49,152-byte
workspace, while the classifier consumes only the final 96x96 RGB region.

## Presentation

The writing should be concise and practical. Commands remain copyable code
blocks. Tables are used only where they improve scanning, such as HTTP endpoint
reference. The GIF appears once in each localized document, references the same
file, and has descriptive alternative text in the corresponding language.

The documents must not claim that a successful build alone proves hardware
operation. They distinguish host/build verification from on-device acceptance,
including camera detection, inference output, dashboard snapshot display,
SoftAP availability, and sustained resource stability.

## Validation

- Verify every relative link and image target exists.
- Check that the English and Chinese documents have matching section coverage.
- Run the repository's complete host test suite because the README update is
  being submitted together with the current inference snapshot fix.
- Build the ESP32-S3 firmware before committing the completed feature branch.

## Git integration

The completed documentation, demonstration GIF, inference snapshot fix, and
their regression tests are committed on `feature/modev2-inference`. The user's
local `.gitignore` modification is excluded. After verification, integration
into `main` is performed only after the user selects the local-merge option.

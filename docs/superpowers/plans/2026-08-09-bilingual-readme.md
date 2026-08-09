# Bilingual README Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Publish equivalent English and Simplified Chinese project guides with reciprocal language links and the supplied demonstration GIF, then submit the verified modev2 feature branch for integration into `main`.

**Architecture:** Keep `README.md` as the English GitHub landing page and add `README.zh-CN.md` as its Chinese counterpart. Both documents reference the same root-level `演示.gif`, follow the same conceptual section order, and accurately describe the active modev2 firmware and corrected inference snapshot pipeline.

**Tech Stack:** GitHub-flavored Markdown, Python `unittest`, ESP-IDF 5.5.4, Git.

## Global Constraints

- English remains the default language in `README.md`.
- The language selector is `[English](README.md) | [中文](README.zh-CN.md)` in both files.
- Keep `演示.gif` at the repository root and embed it once in each localized README.
- `modev2` is active; `modev1` remains available for rollback and is not compiled.
- Describe 128x128 JPEG capture, a 49,152-byte FIT_SHORTEST RGB workspace, and final 96x96 model input.
- Preserve the user's local `.gitignore` modification; never stage or commit it.
- Do not merge into `main` until the user explicitly selects local merge.

---

### Task 1: Commit the validated inference snapshot fix

**Files:**
- Modify: `components/INFERENCE/inference.cpp`
- Modify: `components/HTTP_CAPTURE/dashboard_page.c`
- Test: `tests/host/inference_component_test.cpp`
- Test: `tests/test_http_capture_component.py`

**Interfaces:**
- Consumes: Edge Impulse `resize_image_using_mode(..., EI_CLASSIFIER_RESIZE_FIT_SHORTEST)` and the existing dashboard metadata/image endpoints.
- Produces: a resize destination allocation of `max(capture_rgb_bytes, model_rgb_bytes)` and a centered, blue, 800-weight `.prediction` style.

- [ ] **Step 1: Review the existing focused regression changes**

Confirm `kResizeWorkspaceBytes` is the larger of the capture and model RGB sizes, and the allocation test requires the same value. Confirm the dashboard style is exactly:

```css
.prediction {
    font-size: 1.25rem;
    font-weight: 800;
    text-align: center;
    color: #155eef;
    margin: .25rem 0;
}
```

- [ ] **Step 2: Run focused tests**

Run:

```bash
python3 -m unittest tests.test_inference_component tests.test_http_capture_component
```

Expected: `Ran 11 tests` followed by `OK`.

- [ ] **Step 3: Check and commit only the snapshot fix**

Run:

```bash
git diff --check -- components/INFERENCE/inference.cpp components/HTTP_CAPTURE/dashboard_page.c tests/host/inference_component_test.cpp tests/test_http_capture_component.py
git add components/INFERENCE/inference.cpp components/HTTP_CAPTURE/dashboard_page.c tests/host/inference_component_test.cpp tests/test_http_capture_component.py
git commit -m "fix: preserve inference snapshot during resize"
```

Expected: the commit contains the four listed files and excludes `.gitignore`.

### Task 2: Add a failing localization contract test

**Files:**
- Create: `tests/test_readme_localization.py`
- Read: `README.md`
- Expected later: `README.zh-CN.md`
- Asset: `演示.gif`

**Interfaces:**
- Consumes: repository-root Markdown files and the supplied GIF asset.
- Produces: a host-side contract that verifies language navigation, localized demonstration image markup, and required section coverage.

- [ ] **Step 1: Write the localization contract test**

Create `tests/test_readme_localization.py` with:

```python
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
LANGUAGE_SELECTOR = "[English](README.md) | [中文](README.zh-CN.md)"


class ReadmeLocalizationTest(unittest.TestCase):
    def test_language_navigation_and_demo_asset(self):
        english = (ROOT / "README.md").read_text(encoding="utf-8")
        chinese = (ROOT / "README.zh-CN.md").read_text(encoding="utf-8")

        self.assertTrue((ROOT / "演示.gif").is_file())
        self.assertIn(LANGUAGE_SELECTOR, english)
        self.assertIn(LANGUAGE_SELECTOR, chinese)
        self.assertIn("![EdgeDeploy GS demonstration](演示.gif)", english)
        self.assertIn("![EdgeDeploy GS 演示](演示.gif)", chinese)

    def test_both_languages_cover_required_topics(self):
        documents = {
            "README.md": [
                "## Features",
                "## Requirements",
                "## How it works",
                "## Configure the SoftAP",
                "## Build, flash, and monitor",
                "## Use the dashboard",
                "## Collect a dataset locally",
                "## HTTP API",
                "## Verification",
            ],
            "README.zh-CN.md": [
                "## 主要功能",
                "## 环境与硬件要求",
                "## 工作原理",
                "## 配置 SoftAP",
                "## 构建、烧录与串口监视",
                "## 使用推理网页",
                "## 本机连续采集数据集",
                "## HTTP API",
                "## 验证",
            ],
        }
        for filename, headings in documents.items():
            content = (ROOT / filename).read_text(encoding="utf-8")
            for heading in headings:
                with self.subTest(filename=filename, heading=heading):
                    self.assertIn(heading, content)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run the new test and confirm it fails**

Run:

```bash
python3 -m unittest tests.test_readme_localization
```

Expected: error because `README.zh-CN.md` does not exist yet.

### Task 3: Write equivalent English and Chinese project guides

**Files:**
- Modify: `README.md`
- Create: `README.zh-CN.md`
- Add: `演示.gif`
- Test: `tests/test_readme_localization.py`

**Interfaces:**
- Consumes: the language selector, GIF filename, and topic contract defined in Task 2.
- Produces: two directly linked, equivalent GitHub project guides.

- [ ] **Step 1: Rewrite the English landing page**

Use this exact opening structure:

```markdown
# EdgeDeploy GS

[English](README.md) | [中文](README.zh-CN.md)

EdgeDeploy GS is an ESP32-S3 edge-vision firmware that captures native
128x128 JPEG frames from an OV5640 camera, serves a local Wi-Fi dashboard,
and runs a deployment-version-2 Edge Impulse waste classifier on-device.

![EdgeDeploy GS demonstration](演示.gif)
```

Continue with the exact top-level headings required by the localization test.
Retain the current verified facts and commands, including ESP-IDF 5.5.4,
ESP32-S3 with 16 MB flash and Octal PSRAM, SoftAP defaults, the three model
labels, `idf.py` commands, dataset capture instructions, and host test command.
Use an HTTP API table with `/capture`, `/api/status`, `/api/inference`, and
`/api/inference/image?sequence=N`.

In `How it works`, state explicitly:

```text
The camera frame is decoded into a 49,152-byte RGB888 capture buffer. The
FIT_SHORTEST resize operation uses a separate 49,152-byte workspace because it
first stores the 128x128 crop there, then resizes in place. The classifier
reads only the final 96x96 RGB region.
```

- [ ] **Step 2: Write the Simplified Chinese counterpart**

Use this exact opening structure:

```markdown
# EdgeDeploy GS

[English](README.md) | [中文](README.zh-CN.md)

EdgeDeploy GS 是运行在 ESP32-S3 上的边缘视觉固件：它通过 OV5640
摄像头采集原生 128×128 JPEG 图像，提供本地 Wi-Fi 推理网页，并在设备端
运行第二版 Edge Impulse 垃圾分类模型。

![EdgeDeploy GS 演示](演示.gif)
```

Use the exact Chinese headings required by the localization test. Translate
the English document faithfully rather than shortening it; keep commands,
paths, model labels, configuration keys, URLs, and JSON endpoint names
unchanged. Explain the 49,152-byte resize-workspace behavior in Chinese.

- [ ] **Step 3: Run the localization test and confirm it passes**

Run:

```bash
python3 -m unittest tests.test_readme_localization
```

Expected: `Ran 2 tests` followed by `OK`.

- [ ] **Step 4: Check links, image references, and matching coverage**

Run:

```bash
python3 -c 'from pathlib import Path; root=Path("."); assert (root/"README.md").is_file(); assert (root/"README.zh-CN.md").is_file(); assert (root/"演示.gif").is_file(); print("README assets OK")'
git diff --check -- README.md README.zh-CN.md tests/test_readme_localization.py
```

Expected: `README assets OK` and no whitespace errors.

- [ ] **Step 5: Commit the localized documentation and GIF**

Run:

```bash
git add README.md README.zh-CN.md tests/test_readme_localization.py 演示.gif
git commit -m "docs: add bilingual project guide"
```

Expected: the commit contains exactly the two README files, localization test,
and GIF; `.gitignore` remains unstaged.

### Task 4: Verify and prepare branch integration

**Files:**
- Verify: all tracked source, tests, documentation, and firmware build inputs.
- Preserve: `.gitignore` local modification.

**Interfaces:**
- Consumes: the committed snapshot fix and bilingual documentation.
- Produces: a green `feature/modev2-inference` branch ready for the user's integration choice.

- [ ] **Step 1: Run the complete host test suite**

Run:

```bash
python3 -m unittest discover -s tests
```

Expected: all tests pass. The test count increases from 57 to 59.

- [ ] **Step 2: Build the ESP32-S3 firmware**

Run:

```bash
IDF_PATH=/Users/stephenapollo/.espressif/v5.5.4/esp-idf ninja -C build -j2
```

Expected: exit code 0, successful `edgeDeploy_GS.bin` generation, and no
partition overflow.

- [ ] **Step 3: Confirm branch cleanliness and commit scope**

Run:

```bash
git status --short
git log --oneline main..feature/modev2-inference
git diff --check main...feature/modev2-inference
```

Expected: only the user's `.gitignore` modification remains in the working
tree; the feature branch contains the modev2, dashboard, snapshot, test, design,
plan, bilingual README, and GIF commits.

- [ ] **Step 4: Present the integration menu**

Present exactly:

```text
Implementation complete. What would you like to do?

1. Merge back to main locally
2. Push and create a Pull Request
3. Keep the branch as-is (I'll handle it later)

Which option?
```

Do not merge, push, or delete the branch until the user chooses.

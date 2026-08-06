"""Browser page for the local ESP32 dataset capture console."""

import json


INDEX_HTML_TEMPLATE = """<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>ESP32 数据集采集</title>
  <style>
    * { box-sizing: border-box; }
    body { font-family: system-ui, sans-serif; max-width: 70rem; margin: 2rem auto; padding: 0 1rem; background: #f5f7fa; color: #172033; }
    .card { display: grid; gap: 1rem; background: white; border-radius: 16px; padding: 1.25rem; box-shadow: 0 8px 28px #18243a1f; }
    .visual-grid { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 1rem; }
    .visual-panel { border: 1px solid #dce3ed; border-radius: 12px; padding: 1rem; text-align: center; background: #fbfcfe; }
    .visual-panel h2 { margin: 0 0 .5rem; font-size: 1rem; }
    .visual-panel > img { display: block; width: 256px; height: 256px; max-width: 100%; margin: .5rem auto; image-rendering: pixelated; object-fit: contain; background: #e7ebf0; border-radius: 10px; }
    .muted { min-height: 1.4rem; color: #59677d; font-size: .9rem; }
    .thumbnail-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(56px, 1fr)); gap: .45rem; min-height: 64px; }
    .thumbnail-grid img { display: block; width: 100%; aspect-ratio: 1; object-fit: contain; image-rendering: pixelated; background: #e7ebf0; border-radius: 7px; }
    .capture-controls { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: .75rem; align-items: end; }
    .controls { display: flex; gap: .75rem; flex-wrap: wrap; grid-column: 1 / -1; }
    label { display: grid; gap: .25rem; }
    input, button { font: inherit; padding: .65rem; }
    button { border: 0; border-radius: 9px; font-weight: 650; cursor: pointer; background: #155eef; color: white; }
    button:disabled { cursor: not-allowed; opacity: .55; }
    dl { display: grid; grid-template-columns: max-content 1fr; gap: .4rem 1rem; }
    dt { font-weight: 600; }
    dd { margin: 0; overflow-wrap: anywhere; }
    #lastError { color: #b42318; }
    @media (max-width: 700px) {
      .visual-grid, .capture-controls { grid-template-columns: 1fr; }
    }
  </style>
</head>
<body>
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
        <div id="recentGallery" class="thumbnail-grid" aria-label="最近保存的图片"></div>
      </article>
    </section>
    <section class="capture-controls">
      <label>数据集名称
        <input id="datasetName" maxlength="64" autocomplete="off">
      </label>
      <label>拍照间隔（毫秒）
        <input id="intervalMs" type="number" min="200" max="60000" value="__DEFAULT_INTERVAL_MS__">
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
  <script>
    const streamUrl = __STREAM_URL__;
    const preview = document.getElementById("preview");
    const previewStatus = document.getElementById("previewStatus");
    const latestCapture = document.getElementById("latestCapture");
    const galleryStatus = document.getElementById("galleryStatus");
    const recentGallery = document.getElementById("recentGallery");
    const datasetName = document.getElementById("datasetName");
    const intervalMs = document.getElementById("intervalMs");
    const startButton = document.getElementById("startButton");
    const stopButton = document.getElementById("stopButton");
    const runningState = document.getElementById("runningState");
    const savedCount = document.getElementById("savedCount");
    const failedCount = document.getElementById("failedCount");
    const latestFile = document.getElementById("latestFile");
    const lastError = document.getElementById("lastError");
    let transition = "initializing";
    let refreshGeneration = 0;
    let previewConnected = false;
    let previewReconnectTimer = null;
    let gallerySessionToken = null;
    let statusPollRunning = false;
    const loadedCaptures = new Map();
    const pendingCaptures = new Set();

    startButton.disabled = true;
    stopButton.disabled = true;

    function startPreview() {
      if (previewConnected) {
        return;
      }
      preview.src = `${streamUrl}?t=${Date.now()}`;
      previewConnected = true;
      previewStatus.textContent = "正在连接直播";
    }

    function schedulePreviewReconnect() {
      previewConnected = false;
      previewStatus.textContent = "直播断开，正在重连";
      if (previewReconnectTimer !== null) {
        return;
      }
      previewReconnectTimer = setTimeout(() => {
        previewReconnectTimer = null;
        startPreview();
      }, 1000);
    }

    function clearGallery() {
      loadedCaptures.forEach(item => URL.revokeObjectURL(item.url));
      loadedCaptures.clear();
      pendingCaptures.clear();
      recentGallery.replaceChildren();
      latestCapture.removeAttribute("src");
    }

    async function loadRecentCapture(token, record) {
      const pendingKey = `${token}:${record.sequence}`;
      if (loadedCaptures.has(record.sequence) || pendingCaptures.has(pendingKey)) {
        return;
      }
      pendingCaptures.add(pendingKey);
      try {
        const response = await fetch(
          `/api/captures/${token}/${record.sequence}`,
          {cache: "no-store"},
        );
        if (!response.ok) {
          throw new Error(`图片请求失败 (${response.status})`);
        }
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
        pendingCaptures.delete(pendingKey);
      }
    }

    function renderGallery(records) {
      const thumbnails = [];
      let newestLoaded = null;
      records.forEach(record => {
        const loaded = loadedCaptures.get(record.sequence);
        if (!loaded) {
          return;
        }
        if (newestLoaded === null) {
          newestLoaded = loaded;
        }
        const thumbnail = document.createElement("img");
        thumbnail.src = loaded.url;
        thumbnail.alt = `已保存图片 ${loaded.filename}`;
        thumbnail.title = `${loaded.filename} · ${loaded.captured_at}`;
        thumbnails.push(thumbnail);
      });
      recentGallery.replaceChildren(...thumbnails);
      if (newestLoaded === null) {
        latestCapture.removeAttribute("src");
      } else {
        latestCapture.src = newestLoaded.url;
      }
    }

    async function reconcileGallery(state) {
      const token = typeof state.session_token === "string"
        ? state.session_token
        : null;
      if (token === null) {
        if (gallerySessionToken !== null) {
          clearGallery();
          gallerySessionToken = null;
        }
        galleryStatus.textContent = state.running
          ? "等待第一张已保存图片"
          : "尚未开始采集";
        return;
      }
      if (token !== gallerySessionToken) {
        clearGallery();
        gallerySessionToken = token;
      }

      const records = Array.isArray(state.recent_captures)
        ? state.recent_captures.filter(record =>
          Number.isInteger(record.sequence) && record.sequence > 0 &&
          typeof record.filename === "string" &&
          typeof record.captured_at === "string"
        ).slice(0, 30)
        : [];
      const desiredSequences = new Set(records.map(record => record.sequence));
      loadedCaptures.forEach((loaded, sequence) => {
        if (!desiredSequences.has(sequence)) {
          URL.revokeObjectURL(loaded.url);
          loadedCaptures.delete(sequence);
        }
      });

      let firstError = null;
      await Promise.all(records.map(async record => {
        try {
          await loadRecentCapture(token, record);
        } catch (error) {
          if (firstError === null) {
            firstError = error;
          }
        }
      }));
      if (token !== gallerySessionToken) {
        return;
      }
      renderGallery(records);
      if (firstError !== null) {
        galleryStatus.textContent = `部分图片加载失败：${firstError.message}`;
      } else if (loadedCaptures.size === 0) {
        galleryStatus.textContent = "等待第一张已保存图片";
      } else {
        galleryStatus.textContent =
          `显示最近 ${loadedCaptures.size} 张（本轮已保存 ${state.saved_count} 张）`;
      }
    }

    async function requestJson(path, options = {}) {
      const response = await fetch(path, {
        cache: "no-store",
        headers: {"Content-Type": "application/json"},
        ...options,
      });
      const result = await response.json();
      if (!response.ok) {
        throw new Error(result.error || `请求失败 (${response.status})`);
      }
      return result;
    }

    function renderStatus(state) {
      runningState.textContent = state.running ? "采集中" : "已停止";
      savedCount.textContent = state.saved_count;
      failedCount.textContent = state.failed_count;
      latestFile.textContent = state.latest_file || "—";
      lastError.textContent = state.last_error || "—";
      if (transition === "idle") {
        startButton.disabled = state.running;
        stopButton.disabled = !state.running;
      } else {
        startButton.disabled = true;
        stopButton.disabled = true;
      }
    }

    async function refreshStatus() {
      const generation = ++refreshGeneration;
      const state = await requestJson("/api/status");
      if (generation !== refreshGeneration ||
          transition === "starting" ||
          transition === "stopping") {
        return null;
      }
      renderStatus(state);
      return state;
    }

    async function synchronizeStatus() {
      if (statusPollRunning) {
        return null;
      }
      statusPollRunning = true;
      try {
        const state = await refreshStatus();
        if (state === null) {
          return null;
        }
        if (transition === "initializing") {
          transition = "idle";
          renderStatus(state);
        }
        await reconcileGallery(state);
        return state;
      } catch (error) {
        startButton.disabled = true;
        stopButton.disabled = true;
        lastError.textContent = error.message;
        return null;
      } finally {
        statusPollRunning = false;
      }
    }

    preview.addEventListener("load", () => {
      previewStatus.textContent = "直播中";
    });
    preview.addEventListener("error", schedulePreviewReconnect);

    startButton.addEventListener("click", async () => {
      transition = "starting";
      refreshGeneration += 1;
      startButton.disabled = true;
      stopButton.disabled = true;
      try {
        const state = await requestJson("/api/capture/start", {
          method: "POST",
          body: JSON.stringify({
            dataset_name: datasetName.value,
            interval_ms: Number(intervalMs.value),
          }),
        });
        transition = "idle";
        refreshGeneration += 1;
        renderStatus(state);
        await reconcileGallery(state);
      } catch (error) {
        transition = "initializing";
        refreshGeneration += 1;
        await synchronizeStatus();
        lastError.textContent = error.message;
      }
    });

    stopButton.addEventListener("click", async () => {
      transition = "stopping";
      refreshGeneration += 1;
      startButton.disabled = true;
      stopButton.disabled = true;
      try {
        const state = await requestJson("/api/capture/stop", {
          method: "POST",
          body: JSON.stringify({}),
        });
        transition = "idle";
        refreshGeneration += 1;
        renderStatus(state);
        await reconcileGallery(state);
      } catch (error) {
        transition = "initializing";
        refreshGeneration += 1;
        await synchronizeStatus();
        lastError.textContent = error.message;
      }
    });

    startPreview();
    synchronizeStatus()
      .finally(() => {
        setInterval(synchronizeStatus, 500);
      });
  </script>
</body>
</html>
"""


def render_capture_page(stream_url: str, default_interval_ms: int) -> bytes:
    """Render the capture page with escaped runtime values."""
    return INDEX_HTML_TEMPLATE.replace(
        "__STREAM_URL__",
        json.dumps(stream_url),
    ).replace(
        "__DEFAULT_INTERVAL_MS__",
        str(default_interval_ms),
    ).encode("utf-8")

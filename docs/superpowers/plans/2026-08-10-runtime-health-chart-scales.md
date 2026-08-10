# Runtime Health Chart Scales Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make both runtime-health charts readable on a phone by adding rounded numeric axes, time-span labels, grid lines, and latest-value summaries.

**Architecture:** Keep the existing firmware-embedded HTML and plain SVG approach. Add one pure JavaScript scale helper shared by the latency and memory charts, keep both memory series on one scale, and extend the existing Node-based dashboard behavior harness to verify the rendered DOM and finite SVG paths.

**Tech Stack:** ESP-IDF 5.5.4, C string-embedded HTML/CSS/JavaScript, browser SVG, Python `unittest`, Node.js behavior harness.

## Global Constraints

- Do not add a JavaScript chart library or any network dependency.
- Retain at most 60 distinct health samples in browser memory and no history on the board.
- Show three horizontal grid lines and top, middle, and bottom Y-axis values.
- Show the actual oldest retained sample age at the left and `现在` at the right.
- Show `当前 N ms` for latency and current KiB values for both memory series.
- Disabling health monitoring clears paths, tick labels, time labels, and current summaries.
- Never render `NaN` or `Infinity` into SVG coordinates or visible labels.

---

### Task 1: Render readable scales and current values

**Files:**
- Modify: `tests/test_http_capture_component.py:223-344`
- Modify: `components/HTTP_CAPTURE/dashboard_page.c:43-67`
- Modify: `components/HTTP_CAPTURE/dashboard_page.c:108-121`
- Modify: `components/HTTP_CAPTURE/dashboard_page.c:137-151`
- Modify: `components/HTTP_CAPTURE/dashboard_page.c:217-264`

**Interfaces:**
- Consumes: `healthHistory: Array<HealthSnapshot>` with `monitor_uptime_ms`, `inference.last_duration_ms`, `memory.internal.free_bytes`, and `memory.internal.largest_free_block_bytes`.
- Produces: `niceChartScale(values: number[]): {minimum: number, midpoint: number, maximum: number} | null`.
- Produces: `chartPath(values: number[], scale: ChartScale): string` using the shared plot rectangle.
- Produces DOM values: `latencyCurrent`, `latencyTickTop`, `latencyTickMiddle`, `latencyTickBottom`, `latencyAge`, `memoryFreeValue`, `memoryLargestValue`, `memoryTickTop`, `memoryTickMiddle`, `memoryTickBottom`, and `memoryAge`.

- [x] **Step 1: Extend the behavior harness with failing scale assertions**

After appending samples 1 through 65, add literal assertions that require the new visible behavior:

```javascript
if (element('latencyCurrent').textContent !== '当前 120 ms') {
  throw new Error('latest latency value missing');
}
if (element('latencyAge').textContent !== '59 秒前' ||
    element('memoryAge').textContent !== '59 秒前') {
  throw new Error('chart time span missing');
}
for (const id of [
  'latencyTickTop', 'latencyTickMiddle', 'latencyTickBottom',
  'memoryTickTop', 'memoryTickMiddle', 'memoryTickBottom'
]) {
  const label = element(id).textContent;
  if (!label || label === '—' || !Number.isFinite(Number(label))) {
    throw new Error(`invalid chart tick ${id}: ${label}`);
  }
}
if (element('memoryFreeValue').textContent !== '154.4 KiB' ||
    element('memoryLargestValue').textContent !== '80.1 KiB') {
  throw new Error('latest memory values missing');
}
```

After disabling monitoring, require all new dynamic labels to reset to an em dash:

```javascript
for (const id of [
  'latencyCurrent', 'latencyTickTop', 'latencyTickMiddle',
  'latencyTickBottom', 'latencyAge', 'memoryFreeValue',
  'memoryLargestValue', 'memoryTickTop', 'memoryTickMiddle',
  'memoryTickBottom', 'memoryAge'
]) {
  if (element(id).textContent !== '—') {
    throw new Error(`chart label was not cleared: ${id}`);
  }
}
```

- [x] **Step 2: Run the focused test and verify RED**

Run:

```bash
python3 -m unittest tests.test_http_capture_component.HttpCaptureComponentBehaviorTest.test_dashboard_controls_health_and_bounds_chart_history -v
```

Expected: FAIL because `latencyCurrent` and the axis-label elements are not populated by the current dashboard.

- [x] **Step 3: Add the SVG frame and latest-value elements**

In `dashboard_page.c`, change each chart to use a header row and a `0 0 300 118` view box. Use a plot rectangle from X `44` to `292` and Y `10` to `88`:

```html
<div class="chart-head"><h3>推理耗时趋势</h3>
  <strong id="latencyCurrent">—</strong></div>
<svg viewBox="0 0 300 118" aria-label="Inference duration chart">
  <line class="chart-grid" x1="44" y1="10" x2="292" y2="10"></line>
  <line class="chart-grid" x1="44" y1="49" x2="292" y2="49"></line>
  <line class="chart-grid" x1="44" y1="88" x2="292" y2="88"></line>
  <text id="latencyTickTop" class="chart-tick" x="38" y="13">—</text>
  <text id="latencyTickMiddle" class="chart-tick" x="38" y="52">—</text>
  <text id="latencyTickBottom" class="chart-tick" x="38" y="91">—</text>
  <text id="latencyAge" class="chart-time" x="44" y="108">—</text>
  <text class="chart-time chart-time-now" x="292" y="108">现在</text>
  <path id="latencyPath" class="chart-latency"></path>
</svg>
```

Use this corresponding memory frame and expose the latest values inside the
existing color legend:

```html
<div class="chart-head"><h3>内部内存趋势</h3></div>
<svg viewBox="0 0 300 118" aria-label="Internal memory chart">
  <line class="chart-grid" x1="44" y1="10" x2="292" y2="10"></line>
  <line class="chart-grid" x1="44" y1="49" x2="292" y2="49"></line>
  <line class="chart-grid" x1="44" y1="88" x2="292" y2="88"></line>
  <text id="memoryTickTop" class="chart-tick" x="38" y="13">—</text>
  <text id="memoryTickMiddle" class="chart-tick" x="38" y="52">—</text>
  <text id="memoryTickBottom" class="chart-tick" x="38" y="91">—</text>
  <text id="memoryAge" class="chart-time" x="44" y="108">—</text>
  <text class="chart-time chart-time-now" x="292" y="108">现在</text>
  <path id="memoryFreePath" class="chart-free"></path>
  <path id="memoryLargestPath" class="chart-largest"></path>
</svg>
<div class="chart-legend">
  <span><i class="legend-key legend-free"></i>空闲内存
    <strong id="memoryFreeValue">—</strong></span>
  <span><i class="legend-key legend-largest"></i>最大连续块
    <strong id="memoryLargestValue">—</strong></span>
</div>
```

Add compact CSS that keeps labels legible and allows the memory legend to wrap:

```css
.chart-head{display:flex;align-items:baseline;justify-content:space-between;gap:.5rem}
.chart-head strong{font-size:.7rem;color:#475467}
.chart-grid{stroke:#eaecf0;stroke-width:1}
.chart-tick,.chart-time{fill:#667085;font-size:8px}
.chart-tick{text-anchor:end}
.chart-time-now{text-anchor:end}
.chart-legend{flex-wrap:wrap}
```

- [x] **Step 4: Implement the shared rounded scale and rendering logic**

Register the new elements with `document.getElementById`. Replace the current implicit min/max path generator with these behaviors:

```javascript
const CHART_LEFT=44,CHART_RIGHT=292,CHART_TOP=10,CHART_BOTTOM=88;
function niceStep(rawStep){
  const power=10**Math.floor(Math.log10(rawStep));
  const normalized=rawStep/power;
  const factor=normalized<=1?1:normalized<=2?2:normalized<=5?5:10;
  return factor*power;
}
function niceChartScale(values){
  const finite=values.filter(Number.isFinite);
  if(finite.length===0)return null;
  let low=Math.min(...finite),high=Math.max(...finite);
  let span=high-low;
  if(span===0)span=Math.max(Math.abs(high)*.1,1);
  const padding=span*.1;
  const step=niceStep((span+2*padding)/4);
  const minimum=Math.max(0,Math.floor((low-padding)/step)*step);
  let maximum=Math.ceil((high+padding)/step)*step;
  if(maximum<=minimum)maximum=minimum+step;
  return{minimum,midpoint:(minimum+maximum)/2,maximum};
}
function chartPath(values,scale){
  if(!scale)return'';
  const points=values.map((value,index)=>({value:Number(value),index}))
    .filter(point=>Number.isFinite(point.value));
  if(points.length===0)return'';
  return points.map((point,pathIndex)=>{
    const x=values.length===1?(CHART_LEFT+CHART_RIGHT)/2:
      CHART_LEFT+point.index*(CHART_RIGHT-CHART_LEFT)/(values.length-1);
    const y=CHART_BOTTOM-(point.value-scale.minimum)*(CHART_BOTTOM-CHART_TOP)/
      (scale.maximum-scale.minimum);
    return`${pathIndex===0?'M':'L'}${x.toFixed(1)},${y.toFixed(1)}`;
  }).join(' ');
}
```

Add these render helpers so scale labels, oldest age, and clearing behavior use
one explicit path:

```javascript
function setScaleLabels(top,middle,bottom,scale,formatter){
  if(!scale){top.textContent=middle.textContent=bottom.textContent='—';return;}
  top.textContent=formatter(scale.maximum);
  middle.textContent=formatter(scale.midpoint);
  bottom.textContent=formatter(scale.minimum);
}
function chartAgeLabel(){
  if(healthHistory.length<2)return'0 秒前';
  const first=Number(healthHistory[0].monitor_uptime_ms);
  const last=Number(healthHistory[healthHistory.length-1].monitor_uptime_ms);
  if(!Number.isFinite(first)||!Number.isFinite(last)||last<first)return'—';
  return`${Math.round((last-first)/1000)} 秒前`;
}
function renderHealthCharts(){
  if(healthHistory.length===0){clearHealthHistory();return;}
  const durations=healthHistory.map(item=>Number(item.inference.last_duration_ms));
  const durationScale=niceChartScale(durations);
  latencyPath.setAttribute('d',chartPath(durations,durationScale));
  setScaleLabels(latencyTickTop,latencyTickMiddle,latencyTickBottom,
    durationScale,value=>String(Math.round(value)));
  const latestDuration=durations[durations.length-1];
  latencyCurrent.textContent=Number.isFinite(latestDuration)
    ?`当前 ${Math.round(latestDuration)} ms`:'—';
  latencyAge.textContent=chartAgeLabel();

  const free=healthHistory.map(item=>Number(item.memory.internal.free_bytes)/1024);
  const largest=healthHistory.map(item=>
    Number(item.memory.internal.largest_free_block_bytes)/1024);
  const memoryScale=niceChartScale(free.concat(largest));
  memoryFreePath.setAttribute('d',chartPath(free,memoryScale));
  memoryLargestPath.setAttribute('d',chartPath(largest,memoryScale));
  setScaleLabels(memoryTickTop,memoryTickMiddle,memoryTickBottom,
    memoryScale,value=>Number(value.toFixed(1)).toString());
  memoryFreeValue.textContent=Number.isFinite(free.at(-1))
    ?`${free.at(-1).toFixed(1)} KiB`:'—';
  memoryLargestValue.textContent=Number.isFinite(largest.at(-1))
    ?`${largest.at(-1).toFixed(1)} KiB`:'—';
  memoryAge.textContent=chartAgeLabel();
}
```

Register every referenced DOM element with `document.getElementById` before
these helpers run. Extend `clearHealthHistory()` so every new dynamic label
becomes `—` while the static right label remains `现在`.

- [x] **Step 5: Run the focused behavior test and verify GREEN**

Run:

```bash
python3 -m unittest tests.test_http_capture_component -v
```

Expected: all four HTTP dashboard behavior tests PASS, with no `NaN` or `Infinity` in chart paths.

- [x] **Step 6: Commit the chart implementation**

```bash
git add components/HTTP_CAPTURE/dashboard_page.c tests/test_http_capture_component.py
git commit -m "feat: add readable health chart scales"
```

### Task 2: Verify firmware and phone presentation

**Files:**
- Modify: `docs/acceptance/v0.3.0-runtime-health-dashboard.md`

**Interfaces:**
- Consumes: dashboard behavior from Task 1 and the existing ESP32-S3 build configuration.
- Produces: a reproducible software and phone acceptance record for the exact Git commit and firmware version.

- [x] **Step 1: Run the complete host suite**

Run:

```bash
python3 -m unittest discover -s tests -v
python3 -m py_compile tests/*.py
```

Expected: all tests PASS and Python compilation exits with status 0.

- [x] **Step 2: Build the ESP32-S3 firmware**

Run after activating ESP-IDF 5.5.4:

```bash
idf.py -B build-esp32s3 -D SDKCONFIG=sdkconfig.esp32s3 build
```

Expected: build succeeds, the application fits the 4 MiB partition, and the output reports the exact Git-derived application version.

- [x] **Step 3: Flash and inspect boot behavior**

Run against the discovered `/dev/cu.usbmodem*` port:

```bash
idf.py -B build-esp32s3 -D SDKCONFIG=sdkconfig.esp32s3 -p /dev/cu.usbmodem5B900039351 flash
```

Expected: flash hashes verify, the board hard-resets, camera/inference/HTTP services start, and health monitoring remains off by default.

- [x] **Step 4: Perform focused phone acceptance**

On a phone connected to `ESP32S3-CAPTURE`, open `http://192.168.4.1/`, enable health monitoring, and confirm:

1. the latency chart shows current milliseconds, three numeric Y labels, an oldest-sample age, and `现在`;
2. the memory chart shows both current KiB values and three shared-scale Y labels;
3. labels remain inside the chart cards without horizontal overflow;
4. paths update once per distinct sequence;
5. disabling monitoring restores the exact disabled copy and removes the health task;
6. inference continues without watchdog, panic, or reboot output.

- [x] **Step 5: Update and commit the acceptance record**

Record the exact commit, firmware version, host-test count, build size, phone observations, and final status in `docs/acceptance/v0.3.0-runtime-health-dashboard.md`.

```bash
git add docs/acceptance/v0.3.0-runtime-health-dashboard.md
git commit -m "docs: record health dashboard acceptance"
```

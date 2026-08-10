# Runtime Health Chart Scale Design

Date: 2026-08-10
Status: Approved approach A

## 1. Context

The runtime-health dashboard currently draws two SVG trend lines using the
minimum and maximum values in the browser's 60-sample history. The plot shows
the direction of change, but it has no numeric scale. On a phone this creates
two problems:

1. the user cannot estimate the value represented by a point on the line;
2. automatic scaling can make a small change look visually large without
   revealing the actual range.

The change must keep the page lightweight and self-contained in firmware. It
must not add a chart library or board-side history.

## 2. Chosen Design

Both charts use the same compact mobile-friendly frame:

- three horizontal grid lines;
- top, middle, and bottom numeric Y-axis labels;
- a left X-axis label showing the age of the oldest retained sample;
- a right X-axis label reading `现在`;
- a current-value summary in the chart header or legend.

The inference chart displays its latest value as `当前 N ms`. The internal
memory legend displays the latest free-memory and largest-contiguous-block
values in KiB beside their existing color keys.

The oldest-sample label is derived from the first and last retained monitor
uptime values when available and is capped by the 60-sample browser window.
During startup it therefore reads the actual available span instead of
claiming that a full minute has already been collected.

## 3. Scale Calculation

Each render derives one scale from the finite values visible in that chart.
The two internal-memory series share one scale so their vertical positions
remain directly comparable.

The scale calculation is:

1. discard non-finite values;
2. find the visible minimum and maximum;
3. add 10 percent vertical padding;
4. if all values are equal, introduce a small unit-appropriate span;
5. divide the padded span by four, then round that raw step upward to the next
   `1`, `2`, or `5` multiplied by a power of ten;
6. round the lower bound down and the upper bound up to that step;
7. clamp the lower bound to zero because duration and byte counts cannot be
   negative.

The three displayed labels are the rounded upper bound, the midpoint, and the
rounded lower bound. Duration labels use whole milliseconds. Memory labels use
whole KiB when possible and one decimal place only when required.

This preserves useful variation without hiding the chart's real magnitude.
A fixed zero-based scale was rejected because it would flatten the normal
internal-memory changes. Per-point touch tooltips were rejected for this
iteration because the visible scale and current values satisfy the need while
keeping the embedded page small and reliable.

## 4. SVG and Page Structure

The existing plain SVG implementation remains in
`components/HTTP_CAPTURE/dashboard_page.c`.

Each chart gains:

- a compact header row containing the title and latest value;
- three reusable grid-line elements;
- three Y-axis text elements;
- two X-axis text elements;
- a plot area inset far enough to prevent labels from overlapping the line.

The path generator receives an explicit plot rectangle and computed scale.
The inference path and both memory paths continue to be generated from the
same bounded `healthHistory` array. No additional request, task, timer, or
board memory is introduced.

On narrow screens each chart remains a single-column card. Axis text uses a
small but readable size and the SVG view box reserves label space, so values
do not overflow the card shown in the phone acceptance screenshot.

## 5. State and Error Handling

- With no samples, paths are empty, current values and tick labels display an
  em dash, and no `NaN` or `Infinity` is written into the DOM.
- With one sample or equal values, the synthetic span keeps the line and scale
  valid.
- Non-finite values are ignored for scale calculation. A series with no finite
  values is not drawn.
- Disabling health monitoring clears the paths, tick labels, latest-value
  summaries, and oldest-sample labels together with the existing history.
- Network interruption keeps the most recently rendered chart visible while
  the health header reports that the connection state is unknown.

## 6. Verification

Host-side dashboard behavior tests will verify that:

1. a multi-sample latency history produces finite path coordinates, readable
   upper/middle/lower labels, and the latest duration summary;
2. both memory lines use finite coordinates under their shared scale and the
   legend exposes both latest values;
3. a single-value history still produces finite scale labels and a valid path;
4. disabling monitoring clears histories, paths, labels, and summaries;
5. the existing 60-sample bound and duplicate-sequence filtering remain
   unchanged.

ESP-IDF build verification ensures the larger embedded HTML remains valid for
the target. Final phone acceptance checks label legibility, lack of horizontal
overflow, and an understandable approximate magnitude for both charts.

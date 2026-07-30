# UI Design System Rigor Pass — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make Matrix Player's GUI code produce the design system its documentation already describes — a real typographic hierarchy, one honest scale factor for all geometry, and a correctly ordered text-color ladder — while fixing two pre-existing Essential-mode defects found along the way.

**Architecture:** A new pure-arithmetic module `gui/src/ui_metrics.{hh,cc}` owns the type scale and the geometry scale, derived from one factor anchored at the app's real render height (1080). It is introduced *alongside* the existing `uiScale_`, call sites migrate file by file, and the legacy factor is deleted last — so every intermediate commit builds and renders correctly.

**Tech Stack:** C++17, CMake + Ninja, vk_canvas (`Canvas`, `UiScale` from `core/layout.hh`, `textCenteredStyled` from `core/text_util.hh`), assert-based tests (no framework).

## Global Constraints

- **Spec:** `docs/superpowers/specs/2026-07-28-ui-design-system-rigor-pass-design.md` is authoritative. Where this plan and the spec disagree, stop and ask.
- **`kMinReadableTextSizePx` = 18.2857f** — from `ui_min_text_size.gen.h` (build-generated). Never hardcode the literal; always include the header.
- **Reference height = 1080.0f**, ratio **1.18f**, `floorScale` **1.0f**.
- **Legacy factor = 1.63389** (`max(13.0f/661.0f*1080, 18.2857) / 13.0f`). Used only to derive re-authored values; never appears in shipped code.
- **Commits:** use `./git_wrapper commit "<msg>"` — **never** plain `git commit`. It runs `git add -A` itself, so do not stage manually. Do **not** push; the user pushes.
- **Build:** `scripts/linux/build.sh --debug` → `build/linux_debug/`. Release → `build/linux/`.
- **`core/` rule:** unchanged — this plan touches only `gui/`.
- **No behavior changes beyond the spec.** Geometry must be pixel-identical at H=1080 except where the spec names an intended change.

---

## File Structure

| file | responsibility |
|---|---|
| `gui/src/ui_metrics.hh` | **new** — `UiTextSizes`, `UiMetrics`, `computeUiMetrics()`. Pure; includes only `<algorithm>`, `<cmath>`, `layout.hh`, `ui_min_text_size.gen.h`. |
| `gui/src/ui_metrics.cc` | **new** — implementation. |
| `gui/src/ui_metrics_test.cc` | **new** — assert-based test, `main()` returns 0 on success. |
| `gui/src/theme.hh` | color ladder values + re-authored `SP_*`. |
| `gui/src/player_view.hh` | drop the 7 `kTextSize*Pct` constants and local `UiTextSizes`; hold `UiMetrics metrics_`. |
| `gui/src/player_view.cc` | `recalcLayout()` + `drawFrame()` + panel draws migrated; Essential fixes; empty-state centering. |
| `gui/src/panels/settings_panels.cc` | `drawHeader`/`drawScrollbar` re-authored values. |
| `gui/CMakeLists.txt` | compile `ui_metrics.cc`; add `ui_metrics_test` target. |
| `docs/UI_DESIGN_SYSTEM.md` | rewritten §2–§5, §7, §8, §10; `file:line` re-anchored. |

---

## Task 1: `ui_metrics` module and its test

**Files:**
- Create: `gui/src/ui_metrics.hh`
- Create: `gui/src/ui_metrics.cc`
- Create: `gui/src/ui_metrics_test.cc`
- Modify: `gui/CMakeLists.txt`

**Interfaces:**
- Consumes: `UiScale` (`framework/vk_canvas/core/layout.hh`), `kMinReadableTextSizePx` (`ui_min_text_size.gen.h`, generated into `${CMAKE_BINARY_DIR}/generated`).
- Produces: `struct UiTextSizes { float caption, secondary, body, title, header; }`; `struct UiMetrics { float scale; UiTextSizes text; float space(float) const; float stroke(float) const; }`; `UiMetrics computeUiMetrics(float contentHeight)`; `constexpr float kUiTypeRatio = 1.18f;`, `constexpr float kUiReferenceHeight = 1080.0f;`

- [ ] **Step 1: Write the failing test**

Create `gui/src/ui_metrics_test.cc`:

```cpp
// Asserts must stay live even though the app builds Release (NDEBUG).
#undef NDEBUG
#include <cassert>
#include <cmath>
#include <cstdio>

#include "ui_metrics.hh"

static bool nearlyEqual(float a, float b) { return std::fabs(a - b) < 0.001f; }

int main() {
    // ── The factor floors at 1.0 and grows only above the reference height ──
    assert(nearlyEqual(computeUiMetrics(700.0f).scale,  1.0f));
    assert(nearlyEqual(computeUiMetrics(900.0f).scale,  1.0f));
    assert(nearlyEqual(computeUiMetrics(1080.0f).scale, 1.0f));
    assert(nearlyEqual(computeUiMetrics(1440.0f).scale, 1440.0f / 1080.0f));
    assert(nearlyEqual(computeUiMetrics(2160.0f).scale, 2.0f));

    // ── Roles at the reference height: floor * ratio^n ──
    UiMetrics m = computeUiMetrics(1080.0f);
    assert(nearlyEqual(m.text.caption,   18.2857f));
    assert(nearlyEqual(m.text.secondary, 18.2857f));
    assert(nearlyEqual(m.text.body,      18.2857f * 1.18f));
    assert(nearlyEqual(m.text.title,     18.2857f * 1.18f * 1.18f));
    assert(nearlyEqual(m.text.header,    18.2857f * 1.18f * 1.18f * 1.18f));

    // ── The invariant the OLD system violated: adjacent roles keep an exact
    //    ratio at EVERY height, so the hierarchy never distorts. ──
    for (float h : { 400.0f, 700.0f, 900.0f, 1080.0f, 1440.0f, 2160.0f, 4320.0f }) {
        UiMetrics k = computeUiMetrics(h);
        assert(nearlyEqual(k.text.body   / k.text.caption, 1.18f));
        assert(nearlyEqual(k.text.title  / k.text.body,    1.18f));
        assert(nearlyEqual(k.text.header / k.text.title,   1.18f));
        // caption is the smallest role and never drops under the legibility floor
        assert(k.text.caption >= kMinReadableTextSizePx - 0.001f);
        // hairlines never vanish
        assert(k.stroke(1.0f) >= 1.0f);
    }

    // ── space() is linear in the factor; stroke() snaps to whole pixels ──
    assert(nearlyEqual(m.space(65.0f), 65.0f));
    assert(nearlyEqual(computeUiMetrics(2160.0f).space(65.0f), 130.0f));
    assert(nearlyEqual(m.stroke(1.0f), 1.0f));
    assert(nearlyEqual(m.stroke(3.0f), 3.0f));
    assert(nearlyEqual(computeUiMetrics(1440.0f).stroke(1.0f), 1.0f));  // round(1.333)
    assert(nearlyEqual(computeUiMetrics(1440.0f).stroke(2.0f), 3.0f));  // round(2.667)
    assert(nearlyEqual(computeUiMetrics(1440.0f).stroke(3.0f), 4.0f));  // round(4.0)
    assert(nearlyEqual(computeUiMetrics(2160.0f).stroke(1.0f), 2.0f));
    assert(nearlyEqual(computeUiMetrics(2160.0f).stroke(3.0f), 6.0f));

    std::printf("ui_metrics_test: all assertions passed\n");
    return 0;
}
```

- [ ] **Step 2: Run it to make sure it fails**

```bash
scripts/linux/build.sh --debug
```

Expected: FAIL at configure or compile — `ui_metrics.hh: No such file or directory` (the target does not exist yet).

- [ ] **Step 3: Write the header**

Create `gui/src/ui_metrics.hh`:

```cpp
#pragma once
#include "layout.hh"                 // vk_canvas: UiScale
#include "ui_min_text_size.gen.h"    // build-generated: kMinReadableTextSizePx

// ── The one scale ────────────────────────────────────────────────────────────
//
// Text sizes and geometry derive from a SINGLE factor anchored at the app's
// real render height. Complete mode force-fullscreens (os/linux_host.cc), so
// 1080 is what this app actually draws at, not a historical reference size.
//
// The smallest role is PINNED to kMinReadableTextSizePx (the font's own
// geometric legibility floor, generated at build time). Every other role is
// that floor times kUiTypeRatio^n. This is what keeps the hierarchy honest:
// because all roles derive from one clamped value, the scale clamps
// UNIFORMLY below the reference height instead of the smallest role clamping
// alone and squashing the scale flat — the defect this module replaces.
//
// See docs/superpowers/specs/2026-07-28-ui-design-system-rigor-pass-design.md.

static constexpr float kUiReferenceHeight = 1080.0f;
static constexpr float kUiTypeRatio       = 1.18f;

struct UiTextSizes {
    float caption;    // badges, hints, placeholders, section captions
    float secondary;  // artist, time, duration, search text
    float body;       // grid titles, track titles, nav, settings rows
    float title;      // now-playing title, album-view title
    float header;     // page + panel headers
};

// caption and secondary share a size deliberately — they are separated by the
// text-color ladder (theme.hh) and font style, not by size.

struct UiMetrics {
    float       scale = 1.0f;   // 1.0 at H <= 1080, H/1080 above
    UiTextSizes text{};

    // Spacing/size: pads, gaps, row heights, widths, icon boxes.
    float space(float authored) const { return authored * scale; }

    // Stroke weight: hairlines, borders, bars. Snapped to whole device pixels
    // so a 1px rule never lands blurred across two pixels on a taller display.
    float stroke(float authored) const;
};

UiMetrics computeUiMetrics(float contentHeight);
```

- [ ] **Step 4: Write the implementation**

Create `gui/src/ui_metrics.cc`:

```cpp
#include "ui_metrics.hh"

#include <algorithm>
#include <cmath>

float UiMetrics::stroke(float authored) const {
    return std::max(1.0f, std::round(authored * scale));
}

UiMetrics computeUiMetrics(float contentHeight) {
    UiMetrics m;

    // floorScale = 1.0 (not UiScale's 0.5 default): "never smaller than the
    // reference" — the app has a minimum window height and force-fullscreens
    // in Complete mode, so shrinking below the reference is not a case worth
    // serving, and allowing it is what let the type scale collapse.
    UiScale s{ kUiReferenceHeight, /*floorScale=*/1.0f };
    m.scale = s.factor(contentHeight);

    const float caption = kMinReadableTextSizePx * m.scale;
    m.text.caption   = caption;
    m.text.secondary = caption;
    m.text.body      = caption * kUiTypeRatio;
    m.text.title     = m.text.body  * kUiTypeRatio;
    m.text.header    = m.text.title * kUiTypeRatio;
    return m;
}
```

- [ ] **Step 5: Wire up the build**

In `gui/CMakeLists.txt`, add `src/ui_metrics.cc` to the shared source list (line 7-11):

```cmake
set(MATRIX_PLAYER_PORTABLE_SOURCES
    src/gui_main.cc
    src/player_view.cc
    src/art_view.cc
    src/ui_metrics.cc
    src/panels/settings_panels.cc)
```

Then append the test target at the end of the file (after the font-copy
`add_custom_command`). It is Debug-only, matching how this repo gates its other
dev tooling (`MATRIX_BUILD_AB_TEST`, see root `CMakeLists.txt:230`):

```cmake
# ---------------------------------------------------------------------------
# ui_metrics_test — the app's only automated test. Pure arithmetic (no Vulkan,
# no window), so it runs anywhere. Debug-only; asserts survive via #undef NDEBUG
# in the test source itself. Convention matches framework/vk_canvas/core/tests/.
# ---------------------------------------------------------------------------
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_executable(ui_metrics_test src/ui_metrics_test.cc src/ui_metrics.cc)
    target_include_directories(ui_metrics_test PRIVATE
        src
        ${CMAKE_BINARY_DIR}/generated
        ${CMAKE_SOURCE_DIR}/framework/vk_canvas/core)
    add_dependencies(ui_metrics_test generate_ui_min_text_size)
    if(NOT WIN32)
        target_compile_options(ui_metrics_test PRIVATE -Wall)
    endif()
endif()
```

`layout.hh` lives in `framework/vk_canvas/core/`, which the app target already
reaches transitively via `vk_canvas_core`; the test does not link that library,
so it needs the include directory explicitly.

- [ ] **Step 6: Run the test and make sure it passes**

```bash
scripts/linux/build.sh --debug && ./build/linux_debug/gui/ui_metrics_test
```

Expected: `ui_metrics_test: all assertions passed`, exit code 0.

If `stroke(2.0f)` at 1440 fails: `std::round(2.6667) == 3.0`. If it returned 2, the implementation used truncation rather than `std::round`.

- [ ] **Step 7: Commit**

```bash
./git_wrapper commit "Add ui_metrics: single-factor type + geometry scale, with tests"
```

---

## Task 2: Adopt `UiMetrics` for text sizes

Text sizes migrate first and alone, because this is the one intentionally
visible change — isolating it means the next tasks can claim "no visual change"
and be checked against it.

**Files:**
- Modify: `gui/src/player_view.hh:46-102` (constants + `UiTextSizes`), `:639` (`textSizes_` member)
- Modify: `gui/src/player_view.cc:1453-1472` (`recalcLayout()` head), plus every `textSizes_.*` read

**Interfaces:**
- Consumes: `computeUiMetrics()`, `UiMetrics`, `UiTextSizes` from Task 1.
- Produces: `PlayerWindow::metrics_` (a `UiMetrics`), replacing `textSizes_`. **`uiScale_` keeps its name and its current value** so `drawFrame()` and the panel draws go on compiling untouched; Tasks 3-5 migrate its uses away and Task 5 deletes it.

- [ ] **Step 1: Replace the constants block in `player_view.hh`**

Delete `player_view.hh:61-102` (from `static constexpr float kReferenceWindowHeight` through `struct UiTextSizes { ... };`, inclusive — the seven `kTextSize*Pct` constants, `kMinWindowContentH`, the `static_assert`, and the local `UiTextSizes`). Replace the whole block with:

```cpp
// UI text sizing and geometry scale now live in ui_metrics.hh — one factor
// anchored at the app's real render height, with the smallest role pinned to
// the font's legibility floor. See that header's comment for why.

// The smallest role IS the floor, so the minimum content height at which the
// scale is not clamped is exactly the reference height. Complete mode's window
// sizing must be at least this tall (see UiMode in host.hh).
static constexpr float kMinWindowContentH = kUiReferenceHeight;
```

**This changes a startup behavior, deliberately.** `player_view.cc:279` picks the
initial UI mode with `monitorH >= ceil(kMinWindowContentH) ? Complete : Essential`.
The threshold moves **1051 → 1080**, so a monitor 1051-1079px tall now opens in
Essential mode where it previously opened in Complete. No common display height
falls in that band (768, 800, 864, 900, 1024, 1050, then 1080), so this is a
boundary tidy-up rather than a real regression — but it *is* a behavior change,
and the old justification for 1051 ("the height at which the smallest role clears
the legibility floor") no longer exists, because under the new model the smallest
role is *always* at the floor. The threshold's meaning becomes "is this monitor
tall enough for the full browsing UI", which is a layout question, and 1080 is
the honest answer to it.

Also delete the `static_assert(kMinWindowContentH <= 2000.0f, ...)` at
`player_view.hh:95-98`: it guarded against a bad font recalibration blowing up a
*derived* value, and `kMinWindowContentH` is now a plain constant.

Add `#include "ui_metrics.hh"` to the include block (after `#include "theme.hh"`, line 43).

Remove `#include "responsive_text.hh"` and `#include "ui_min_text_size.gen.h"` from `player_view.hh` **only if** no other declaration in that header uses them — `ui_metrics.hh` includes the latter itself. Grep first: `grep -n "ResponsiveTextScale\|kMinReadableTextSizePx" gui/src/player_view.hh gui/src/player_view.cc`.

- [ ] **Step 2: Replace the member**

In `player_view.hh`, replace the trailing member (line ~639):

```cpp
    // Computed once per recalcLayout() call from the window's current
    // content height and the *Pct constants above.
    UiTextSizes textSizes_{};
```

with:

```cpp
    // Recomputed once per recalcLayout() from the window's content height.
    // metrics_.text.* are the type roles; metrics_.space()/stroke() are the
    // geometry helpers. See ui_metrics.hh.
    UiMetrics metrics_{};
```

Leave the existing `uiScale_` member (`player_view.hh:395`) **in place and
unchanged**. It is the pre-rigor-pass geometry factor, and every remaining use of
it is a call site still to migrate — Task 5 deletes it once there are none. Only
its declaring comment changes:

```cpp
    // LEGACY, being retired: the pre-rigor-pass geometry factor. Every
    // remaining `* uiScale_` is a call site not yet migrated to
    // metrics_.space()/stroke(). Deleted when the last one is gone.
    float uiScale_ = 1.0f;
```

- [ ] **Step 3: Update `recalcLayout()`'s head**

Replace `player_view.cc:1456-1473` (from the `ResponsiveTextScale scale{...}` line through `const float us = uiScale_;`) with:

```cpp
    metrics_ = computeUiMetrics((float)H);

    // LEGACY, being retired: reproduce the old factor exactly so that call
    // sites not yet migrated keep rendering at their current size. 13.0f was
    // the old "nav" role's calibration size and 661.0f its reference height —
    // both are gone from the type scale now, so the formula is spelled out
    // here rather than derived. Deleted once nothing multiplies by uiScale_.
    uiScale_ = std::max(13.0f / 661.0f * (float)H, kMinReadableTextSizePx) / 13.0f;
    const float us = uiScale_;
```

`uiScale_` keeps both its name and its value, so `drawFrame()` and all four
panel draws compile and render **exactly** as before this task. Keep
`const float us` too — every existing `* us` inside `recalcLayout()` is
unaffected.

`kMinReadableTextSizePx` comes in via `ui_metrics.hh`. Add `#include <algorithm>` to `player_view.cc` if `std::max` is not already available there (it is — grep to confirm).

- [ ] **Step 4: Migrate every text-size read**

Mechanically replace across `gui/src/player_view.cc` (57 occurrences):

| old | new |
|---|---|
| `textSizes_.badge` | `metrics_.text.caption` |
| `textSizes_.secondary` | `metrics_.text.secondary` |
| `textSizes_.body` | `metrics_.text.body` |
| `textSizes_.nav` | `metrics_.text.body` |
| `textSizes_.transportTitle` | `metrics_.text.title` |
| `textSizes_.trackPanelTitle` | `metrics_.text.title` |
| `textSizes_.header` | `metrics_.text.header` |

Find them all with:

```bash
grep -n "textSizes_\." gui/src/player_view.cc
```

Verify zero remain afterwards:

```bash
grep -rn "textSizes_" gui/src/ && echo "STILL PRESENT — fix before continuing"
```

- [ ] **Step 5: Build and run**

```bash
scripts/linux/build.sh --debug && ./build/linux_debug/gui/matrix_player
```

Expected — the intended visible change from spec §3, and nothing else:
- page/panel headers noticeably larger (27.77 → 30.04)
- now-playing title in the transport bar larger (22.87 → 25.46)
- grid album titles and track titles slightly larger (20.42/21.24 → 21.58)
- artist, time, duration, badges slightly smaller (18.79 → 18.29)
- **all spacing, row heights, and box sizes unchanged** — `uiScale_` still drives them

If chrome moved, the rewritten `uiScale_` formula does not reproduce the old
derived value. It must evaluate to **1.63389** at H=1080; check by temporarily
printing it, or by hand: `max(13/661*1080, 18.2857) / 13 = 21.2406 / 13`.

- [ ] **Step 6: Commit**

```bash
./git_wrapper commit "Adopt ui_metrics type roles: 5 roles from one ratio, hierarchy restored"
```

---

## Task 3: Migrate `recalcLayout()` geometry

**Files:**
- Modify: `gui/src/player_view.cc:1453-1618` (`recalcLayout()`)
- Modify: `gui/src/player_view.hh` (`gridPadX_`, `gridPadY_` stay ints; no signature change)

**Interfaces:**
- Consumes: `metrics_.space()`, `metrics_.stroke()` from Task 1; `uiScale_` from Task 2.
- Produces: nothing new. `recalcLayout()` no longer references `us`/`uiScale_` when this task is done — but `drawFrame()` and the panels still do, so the member stays until Task 5.

- [ ] **Step 1: Re-author the scaled literals**

In `recalcLayout()`, replace each `X * us` with `metrics_.space(Y)` where **Y = round(X × 1.63389)**:

| line | now | becomes |
|---|---|---|
| 1502 | `(int)(80 * us)` | `(int)metrics_.space(131.0f)` |
| 1508 | `std::max(170, (int)(170.0f * us))` | `(int)metrics_.space(278.0f)` |
| 1544-45 | `+ 18.0f * us` | `+ metrics_.space(29.0f)` |
| 1548 | `(int)(40 * us)` | `(int)metrics_.space(65.0f)` |
| 1556 | `(int)(50 * us)` | `(int)metrics_.space(82.0f)` |
| 1557 | `(int)(58 * us)`, `(int)(90 * us)` | `(int)metrics_.space(95.0f)`, `(int)metrics_.space(147.0f)` |
| 1558 | `40.0f * us`, `102.0f * us` | `metrics_.space(65.0f)`, `metrics_.space(167.0f)` |
| 1563-64 | `8.0f * us` | `metrics_.space(13.0f)` |
| 1568 | `(int)(12 * us)` | `(int)metrics_.space(SP_MD)` |
| 1575 | `(int)(28 * us)` | `(int)metrics_.space(46.0f)` |
| 1581 | `(int)(44 * us)` | `(int)metrics_.space(72.0f)` |
| 1582 | `(int)(12 * us)` | `(int)metrics_.space(SP_MD)` |
| 1595 | `(int)(16 * us)` | `(int)metrics_.space(26.0f)` |
| 1605 | `(int)(90.0f * us)` | `(int)metrics_.space(147.0f)` |
| 1606 | `(int)(220.0f * us)` | `(int)metrics_.space(360.0f)` |
| 1607 | `(int)(52.0f * us)` | `(int)metrics_.space(85.0f)` |
| 1608 | `(int)(14.0f * us)` | `(int)metrics_.space(23.0f)` |

**Write plain float literals in this task, never `SP_*`.** The `SP_*` tokens
still hold their old values (4/8/12/20/40) until Task 6 re-authors them, so
using a token here would render the wrong size for three commits. Task 6 swaps
literals for tokens once the values coincide.

- [ ] **Step 2: Wrap the bare literals**

These render at face value today and must keep their number, gaining only `space()`:

| line | now | becomes |
|---|---|---|
| 1524 | `kTargetTilePitch = 250` | keep the constant; at use site (1529) `gridW / (int)metrics_.space(kTargetTilePitch)` |
| 1525 | `kMinGridArtSize = 80` | at uses (1530, 1533) `(int)metrics_.space(kMinGridArtSize)` |
| 1526 | `kGridArtMargin = 30` | at uses (1530, 1533, 1540) `(int)metrics_.space(kGridArtMargin)` |
| 1528, 1551 | `gridPadX_`, `gridPadY_` | `(int)metrics_.space((float)gridPadX_)`, same for Y |
| 1557 | `12`, `sidebarW - 12` | `(int)metrics_.space(12.0f)`, `sidebarW - (int)metrics_.space(12.0f)` |
| 1596 | `rcBtnPrev_.left - 76` | `rcBtnPrev_.left - (int)metrics_.space(76.0f)` |

For the grid constants, compute each once into a local before the column math so
the expression stays readable:

```cpp
    const int tilePitch  = (int)metrics_.space((float)kTargetTilePitch);
    const int minArtSize = (int)metrics_.space((float)kMinGridArtSize);
    const int artMargin  = (int)metrics_.space((float)kGridArtMargin);

    int gridW = rcGrid_.right - rcGrid_.left - (int)metrics_.space((float)gridPadX_) * 2;
    int desiredCols = std::clamp(gridW / tilePitch, 2, 8);
    while (desiredCols > 1 && (gridW / desiredCols) - artMargin < minArtSize) desiredCols--;
    gridCols_ = std::max(1, desiredCols);

    int newGridArtSize = std::max(minArtSize, gridW / gridCols_ - artMargin);
```

- [ ] **Step 3: Remove `us` from `recalcLayout()`**

Delete `const float us = uiScale_;` once no `* us` remains in the function:

```bash
sed -n '1453,1620p' gui/src/player_view.cc | grep -n "\bus\b"
```

Expected: no output before deleting the declaration. Keep the `uiScale_`
**member** and its assignment — `drawFrame()` and the panels still use it until
Task 5.

- [ ] **Step 4: Build and compare**

```bash
scripts/linux/build.sh --debug && ./build/linux_debug/gui/matrix_player
```

Expected: **identical to the end of Task 2.** Sidebar width, transport bar height, nav row spacing, search box, settings rows, button sizes — all unchanged.

If the sidebar or transport bar visibly changed size, a re-authored value is wrong. Check it against `round(old × 1.63389)`.

- [ ] **Step 5: Commit**

```bash
./git_wrapper commit "Migrate recalcLayout geometry to metrics_.space(), re-anchored at 1080"
```

---

## Task 4: Migrate `drawFrame()` geometry

**Files:**
- Modify: `gui/src/player_view.cc:793-1449` (`drawFrame()`)

**Interfaces:**
- Consumes: `metrics_.space()`, `metrics_.stroke()`.
- Produces: nothing new.

- [ ] **Step 1: Re-author the scaled literals**

| line | now | becomes |
|---|---|---|
| 896 | `4.0f * uiScale_` | `metrics_.space(7.0f)` |
| 907 | `9.0f * uiScale_` | `metrics_.space(15.0f)` |
| 908-09 | `16.0f * uiScale_` (×2) | `metrics_.space(26.0f)` |
| 985 | `2.0f * uiScale_` | `metrics_.stroke(3.0f)` |
| 1023 | `10.0f * uiScale_` | `metrics_.space(16.0f)` |
| 1114 | `SP_XL * uiScale_` | `metrics_.space(65.0f)` |
| 1126 | `40.0f * uiScale_` | `metrics_.space(65.0f)` |
| 1224 | `1.5f * uiScale_` | `metrics_.stroke(2.0f)` |
| 1238 | `30.0f * uiScale_`, `46.0f * uiScale_` | `metrics_.space(49.0f)`, `metrics_.space(75.0f)` |
| 1245 | `16.0f * uiScale_` | `metrics_.space(26.0f)` |
| 1266 | `2.0f * uiScale_` | `metrics_.stroke(3.0f)` |
| 1310 | `120.0f * uiScale_` | `metrics_.space(196.0f)` |

- [ ] **Step 2: Wrap the bare spacing literals**

Keep the number, add `space()`:

| line | now | becomes |
|---|---|---|
| 859-62 | `sb.w - 32`, x `16` | `sb.w - metrics_.space(32.0f)`, `metrics_.space(16.0f)` |
| 882-86, 901-05 | `r.x + 4`, `r.w - 8` | `r.x + metrics_.space(4.0f)`, `r.w - metrics_.space(8.0f)` |
| 888 | `r.x + 20` | `r.x + metrics_.space(20.0f)` |
| 968 | `x - 6, y - 6, a + 12, a + 12` | each via `metrics_.space(6.0f)` / `space(12.0f)` |
| 973-75 | `-9/+18 r12`, `-6/+12 r10`, `-3/+6 r8` | `space()` on every inset and radius |
| 977 | `-3/+6 r8` | `space()` on each |
| 1005 | `y + a + 2`, height `2` | `y + a + metrics_.space(2.0f)`, `metrics_.stroke(2.0f)` |
| 1118 | `pad + 16` | `pad + metrics_.space(16.0f)` |
| 1128 | `artY + 4` | `artY + metrics_.space(4.0f)` |
| 1172, 1174 | `y += 6`, `y += 12` | `metrics_.space(6.0f)`, `metrics_.space(12.0f)` |
| 1173 | separator height `1` | `metrics_.stroke(1.0f)` |
| 1209 | `colX - 12`, `colW + 24` | `metrics_.space(12.0f)`, `metrics_.space(24.0f)` |
| 1215 | bar width `3.0f` | `metrics_.stroke(3.0f)` |
| 1275 | `+ 36.0f` | `+ metrics_.space(36.0f)` |
| 1302 | `yy += 28.0f` | `metrics_.space(28.0f)` |
| 1313 | `imgSize + 16.0f` | `imgSize + metrics_.space(16.0f)` |
| 1330, 1435-36 | hairlines `1` | `metrics_.stroke(1.0f)` |
| 1378 | `t.x + t.w - 16` | `- metrics_.space(16.0f)` |
| 1385-86 | `- tagW - 8`, `+ 8` | `metrics_.space(8.0f)` |
| 1422 | `- tagW - 24 - timeW` | `metrics_.space(24.0f)` |
| 1438-43 | `w.h - 8.0f`, `w.x + 8`, `w.y + 4`, `+ 8.0f` | `space()` on each |
| 854 | sidebar rule `1` wide | `metrics_.stroke(1.0f)` |

**Line 1215's `3.0f` is the accent selection bar** — a stroke, not a gap. Line 884/903's `3.0f` (sidebar/gear selection bar) likewise: `metrics_.stroke(3.0f)`.

- [ ] **Step 3: Handle the mixed-argument draws carefully**

Line 1005 carries both classes in one call:

```cpp
// before
canvas.rect(x, y + a + 2, a, 2, toColor(CLR_ACCENT, 0.4f));
// after — the +2 is a GAP below the art, the trailing 2 is the bar's THICKNESS
canvas.rect(x, y + a + metrics_.space(2.0f), a, metrics_.stroke(2.0f),
            toColor(CLR_ACCENT, 0.4f));
```

Line 1091-94 (settings row outline) is all stroke:

```cpp
float bt = isActiveModeRow ? metrics_.stroke(2.0f) : metrics_.stroke(1.0f);
```

- [ ] **Step 4: Verify no `uiScale_` remains in `drawFrame()`**

```bash
sed -n '793,1449p' gui/src/player_view.cc | grep -n "uiScale_" && echo "STILL PRESENT"
```

Expected: no output.

- [ ] **Step 5: Build and compare**

```bash
scripts/linux/build.sh --debug && ./build/linux_debug/gui/matrix_player
```

Expected: **identical to the end of Task 3.** Grid tile spacing, hover halo, now-playing glow, quality borders, selection bars, transport layout, album-view columns — all unchanged.

Check the now-playing glow specifically: it is the most layered geometry in the app (three rects at radii 12/10/8) and the easiest to get subtly wrong.

- [ ] **Step 6: Commit**

```bash
./git_wrapper commit "Migrate drawFrame geometry to metrics_.space()/stroke()"
```

---

## Task 5: Migrate the settings panels

**Files:**
- Modify: `gui/src/panels/settings_panels.cc:47-83` (`drawHeader`, `drawScrollbar`)
- Modify: `gui/src/panels/settings_panels.hh:36-49` (parameter docs only)
- Modify: `gui/src/player_view.cc:2592-2620, 2695-2835, 2931-2990, 3039-3072` (the four panel draws)

**Interfaces:**
- Consumes: `metrics_.space()`, `metrics_.stroke()`, `metrics_.scale`.
- Produces: `panels::drawHeader` and `panels::drawScrollbar` keep their `float uiScale` parameter — callers now pass `metrics_.scale` instead of `uiScale_`. The parameter is renamed `scale` for clarity; **signature and argument order are unchanged.**

- [ ] **Step 1: Re-author `settings_panels.cc`**

```cpp
LayoutRect drawHeader(Canvas& canvas, const LayoutRect& area, const std::string& title,
                      float scale, float headerTextSize, LayoutRect& closeRc) {
    Rect a = toRect(area);
    canvas.rect(a.x, a.y, a.w, a.h, toColor(CLR_BG_MAIN));

    float headerH = 91.0f * scale;
    canvas.textStyled(title, a.x + 39.0f * scale, a.y + headerH * 0.5f - headerTextSize * 0.5f,
                      headerTextSize, toColor(CLR_TEXT_PRIMARY), FontStyle::Bold);
    canvas.rect(a.x, a.y + headerH, a.w, std::max(1.0f, std::round(scale)), toColor(CLR_SEPARATOR));

    float closeW = 147.0f * scale, closeH = 52.0f * scale;
    closeRc = { (int)(area.right - closeW - 33.0f * scale), (int)(area.top + (headerH - closeH) * 0.5f),
                (int)(area.right - 33.0f * scale), (int)(area.top + (headerH + closeH) * 0.5f) };

    return { area.left, (int)(area.top + headerH), area.right, area.bottom };
}
```

`drawScrollbar`'s three values are all **scaled** today
(`settings_panels.cc:69-76`: `6.0f * uiScale`, `SP_XS * uiScale`,
`24.0f * uiScale`), so all three get re-authored:

| now | becomes |
|---|---|
| `barW = 6.0f * uiScale` | `barW = 10.0f * scale` |
| `SP_XS * uiScale` (inset) | `6.0f * scale` (becomes `SP_XS` in Task 6) |
| `thumbH` floor `24.0f * uiScale` | `39.0f * scale` |

Rename the parameter `uiScale` → `scale` throughout both functions.

- [ ] **Step 2: Re-author the four panel draws in `player_view.cc`**

| now | becomes |
|---|---|
| `SP_LG * uiScale_` (pad, ×4 sites) | `metrics_.space(33.0f)` |
| `36.0f * uiScale_` (btnH, ×4 sites) | `metrics_.space(59.0f)` |
| `14.0f * uiScale_` (empty-state inset, ×5 sites) | `metrics_.space(23.0f)` |
| `170.0f * uiScale_` (btnW, ×2 sites) | `metrics_.space(278.0f)` |
| `120.0f * uiScale_` (Apply btnW) | `metrics_.space(196.0f)` |
| `200.0f * uiScale_` (Select btnW) | `metrics_.space(327.0f)` |
| `34.0f * uiScale_` (rowH, eq search h) | `metrics_.space(56.0f)` |
| `12.0f * uiScale_` (×8 sites) | `metrics_.space(20.0f)` |
| `10.0f * uiScale_` (eq search gap) | `metrics_.space(16.0f)` |
| `60.0f * uiScale_` (JACK empty state y) | `metrics_.space(98.0f)` |
| `uiScale_` passed to `drawHeader`/`drawScrollbar` | `metrics_.scale` |

**`kPanelRowH` stays 44.** It is passed bare (`(float)kPanelRowH` to
`drawScrollList`, `rowCount * kPanelRowH` to the scrollbar) and renders at 44px
today. Wrap its uses in `metrics_.space((float)kPanelRowH)` — do **not**
re-author the constant. Sites: `player_view.cc:2600, 2603, 2745, 2766, 2798,
2816, 2969, 2973, 3057, 3061`, plus the scroll-clamp arithmetic at `:2532-2568`.

- [ ] **Step 3: Verify no `uiScale_` remains anywhere**

```bash
grep -rn "uiScale_" gui/src/ && echo "STILL PRESENT"
```

Expected: no output. If any remain, migrate them before continuing.

- [ ] **Step 4: Delete the legacy factor**

Now that nothing references it, remove the `uiScale_` member from
`player_view.hh` and its assignment in `recalcLayout()`. Build must still
succeed. If it does not, a call site was missed — Step 3's grep should have
caught it.

- [ ] **Step 5: Build and check every panel**

```bash
scripts/linux/build.sh --debug && ./build/linux_debug/gui/matrix_player
```

Open all four panels (gear → each row) and confirm they look identical to
before: header height, Close button, list rows, scrollbar, action buttons.
Scroll the Audio Settings device list to confirm the scrollbar still tracks.

- [ ] **Step 6: Commit**

```bash
./git_wrapper commit "Migrate settings panels to metrics_; retire the legacy uiScale_ factor"
```

---

## Task 6: `theme.hh` — color ladder and `SP_*` adoption

**Files:**
- Modify: `gui/src/theme.hh:12-22` (colors), `:44-48` (`SP_*`)
- Modify: `gui/src/player_view.cc`, `gui/src/panels/settings_panels.cc` (adopt tokens)

**Interfaces:**
- Consumes: nothing new.
- Produces: re-authored `SP_XS/SM/MD/LG/XL` = `6/13/20/33/65`; `CLR_TEXT_SECONDARY` = 170, `CLR_TEXT_DIM` = 128.

- [ ] **Step 1: Fix the color ladder**

Replace `theme.hh:12-17`:

```cpp
static constexpr ColorRef CLR_TEXT_PRIMARY    = RGB(242, 242, 242);
// The ladder is ORDERED: PRIMARY 242 (17.7:1 on CLR_BG_MAIN) > SECONDARY 170
// (8.5:1) > DIM 128 (5.0:1). It was previously inverted — DIM sat at 140 and
// SECONDARY at 128, so badges and hints read LOUDER than artist names and
// durations. Raising DIM for WCAG without re-spacing SECONDARY is what caused
// it; keep the ordering in mind before changing either value.
static constexpr ColorRef CLR_TEXT_SECONDARY  = RGB(170, 170, 170);
// TIGHTEST CONTRAST IN THE APP: the DSP badge draws DIM on CLR_BG_TRANSPORT
// (22), which is 4.58:1 — above WCAG AA (4.5:1) but with almost no margin.
// Do not lower this value without re-checking that pair.
static constexpr ColorRef CLR_TEXT_DIM        = RGB(128, 128, 128);
```

- [ ] **Step 2: Re-author the spacing scale**

Replace `theme.hh:44-48`:

```cpp
// Base pixel rhythm, authored at the 1080 reference height (see ui_metrics.hh).
// Callers pass these through UiMetrics::space(), never multiply by hand.
static constexpr float SP_XS =  6.0f;
static constexpr float SP_SM = 13.0f;
static constexpr float SP_MD = 20.0f;
static constexpr float SP_LG = 33.0f;
static constexpr float SP_XL = 65.0f;
```

Update the block comment above them (`theme.hh:41-43`) to drop the stale
"callers multiply by uiScale_ (the one proportion factor, = textSizes_.nav / 13)"
sentence.

- [ ] **Step 3: Adopt the tokens where values now coincide**

Replace the bare numbers written in Tasks 3-5 with the matching token —
`metrics_.space(6.0f)` → `metrics_.space(SP_XS)`, `13.0f` → `SP_SM`, `20.0f` →
`SP_MD`, `33.0f` → `SP_LG`, `65.0f` → `SP_XL`. Only where the value matches
exactly; leave 16, 23, 26, 29 etc. as literals.

Sites: `player_view.cc` transport pad and button gap (was `20.0f`), album-view
`pad` (was `65.0f`), album art gap (was `65.0f`), gear rule gap (was `13.0f`),
all four panel `pad`s (was `33.0f`), and `settings_panels.cc`'s scrollbar inset
(was `6.0f`).

- [ ] **Step 4: Build and check**

```bash
scripts/linux/build.sh --debug && ./build/linux_debug/gui/matrix_player
```

Expected — the intended change from spec §5, and no geometry movement:
- artist lines under grid tiles, the `m:ss / m:ss` clock, and track durations **brighter**
- `BITPERFECT`/`REF EQ` tag, `Search` placeholder, quality badge, `ABOUT THIS ALBUM` caption, empty-state hints **dimmer**

- [ ] **Step 5: Commit**

```bash
./git_wrapper commit "Un-invert the text-color ladder; re-author SP_* at the 1080 reference"
```

---

## Task 7: Essential-mode fixes

**Files:**
- Modify: `gui/src/player_view.cc:1475-1500` (Essential branch of `recalcLayout()`), `:1534-1539` (grid cache guard)

**Interfaces:**
- Consumes: `metrics_`, `uiMode_`.
- Produces: `rcEssentialArt_` is populated in **both** UI modes, which
  `loadTransportArtTexture()` (`:1893-1898`) already reads.

- [ ] **Step 1: Restructure `recalcLayout()` so both modes compute**

Remove the `return` at `player_view.cc:1499`. Convert the function so the
Essential geometry is computed unconditionally, then the Complete geometry, with
only the *cache side effect* gated. Replace the Essential branch head:

```cpp
    // Essential-mode geometry is computed in BOTH modes, not just when
    // Essential is active: loadTransportArtTexture() sizes the now-playing
    // texture for max(transport, essential) so a mode switch never stretches
    // it, and rcEssentialArt_ being {} in Complete mode made that max() pick
    // the ~92px transport thumb — which then got scaled 5.4x on Alt+L.
    {
        int margin = std::max(12, W / 20);
        int bottomReserve = std::max(120, H / 5);

        // btnSize is derived from bottomReserve, NOT from W. Deriving it from
        // W let it exceed the reserve at the mode's own default size
        // (1200x700 -> btnSize 150 vs reserve 140), so the controls overlapped
        // the bottom of the album art and sat on top of the title band.
        // The reserve holds a title line plus the button row plus their gaps.
        int titleH = (int)(metrics_.text.title * 1.6f);
        int btnSize = std::max(40, bottomReserve - titleH - margin);
        int btnGap  = std::max(16, btnSize / 2);

        int artSize = std::min({ W - margin * 2, H - bottomReserve - margin, H });
        int artX = (W - artSize) / 2;
        int artY = margin;
        rcEssentialArt_ = { artX, artY, artX + artSize, artY + artSize };

        int titleY = rcEssentialArt_.bottom + 12;
        rcEssentialTitle_ = { margin, titleY, W - margin, titleY + titleH };

        int totalBtnW = btnSize * 3 + btnGap * 2;
        int btnX = (W - totalBtnW) / 2;
        int btnY = H - margin - btnSize;
        rcEssentialPrev_     = { btnX, btnY, btnX + btnSize, btnY + btnSize };
        btnX += btnSize + btnGap;
        rcEssentialPlayStop_ = { btnX, btnY, btnX + btnSize, btnY + btnSize };
        btnX += btnSize + btnGap;
        rcEssentialNext_     = { btnX, btnY, btnX + btnSize, btnY + btnSize };
    }

    if (uiMode_ == UiMode::Essential) return;
```

The early `return` now sits *after* the Essential block, so Complete-mode
geometry is skipped in Essential mode as before — but `rcEssentialArt_` is
always fresh. `rcTransportArt_` is still stale while in Essential mode; that is
harmless, because `loadTransportArtTexture()` takes the `max()` of the two and
the Essential rect is the larger one.

At 1200x700 this now yields: `titleH ≈ 41`, `btnSize = max(40, 140-41-60) = 40`,
`btnY = 600`, art `60..560`, title `572..613` — no overlap.

- [ ] **Step 2: Guard the grid art cache**

At `player_view.cc:1534-1539`, `clearGridArtTexCache()` fires whenever
`gridArtSize_` changes. That block is inside the Complete-only section, so it is
already unreachable in Essential mode after Step 1 — **verify this** rather than
adding a redundant guard:

```bash
grep -n "clearGridArtTexCache" gui/src/player_view.cc
```

Confirm the call at ~1538 sits below the `if (uiMode_ == UiMode::Essential) return;`
line. If the restructure placed it above, add `if (uiMode_ == UiMode::Complete)`
around it.

- [ ] **Step 3: Test the overlap fix**

```bash
scripts/linux/build.sh --debug && ./build/linux_debug/gui/matrix_player
```

Press **Alt+L**. Expected: the three transport buttons sit below the title, with
no part of them over the album art or the title text.

- [ ] **Step 4: Test the blur fix**

With the app still running in Complete mode: play a track, **then** press Alt+L.

Expected: the album art is **sharp immediately** — not blurry-until-next-track.
This is the exact signature from spec §6.2; if it is still blurry on the first
switch and sharp after skipping a track, `rcEssentialArt_` is still not being
populated in Complete mode.

- [ ] **Step 5: Commit**

```bash
./git_wrapper commit "Fix Essential mode: control overlap at 1200x700, and blurry art on mode switch"
```

---

## Task 8: Empty states — measured centering, and the art window's fallback size

**Files:**
- Modify: `gui/src/player_view.cc:926-940`
- Modify: `gui/src/art_view.cc:365-373`

**Interfaces:**
- Consumes: `canvas.textWidthStyled()`; `computeUiMetrics()` from Task 1.
- Produces: nothing new.

- [ ] **Step 1: Replace the guessed offsets**

```cpp
        if (albums_.empty()) {
            // Measured centering — the old "g.w * 0.5f - 160" guessed this
            // string's half-width at one text size and drifted the moment
            // either changed.
            const char* msg = "No albums yet. Use the gear icon below to add a music folder.";
            float w = canvas.textWidthStyled(msg, metrics_.text.body, FontStyle::Roman);
            canvas.textStyled(msg, g.x + (g.w - w) * 0.5f, g.y + metrics_.space(100.0f),
                              metrics_.text.body, toColor(CLR_TEXT_DIM), FontStyle::Roman);
        } else if (gridIndices_.empty()) {
            std::string msg;
            if (!searchQuery_.empty()) {
                msg = "No matches for \"" + searchQuery_ + "\"";
            } else {
                const char* filterLabel =
                    albumTypeFilter_ == AlbumTypeFilter::Ep     ? "EPs" :
                    albumTypeFilter_ == AlbumTypeFilter::Single ? "Singles" :
                    albumTypeFilter_ == AlbumTypeFilter::Remix  ? "Remixes" : "Albums";
                msg = std::string("No ") + filterLabel + " yet";
            }
            float w = canvas.textWidthStyled(msg, metrics_.text.body, FontStyle::Roman);
            canvas.textStyled(msg, g.x + (g.w - w) * 0.5f, g.y + metrics_.space(100.0f),
                              metrics_.text.body, toColor(CLR_TEXT_DIM), FontStyle::Roman);
        }
```

Using `textWidthStyled` + explicit placement rather than `textCenteredStyled`
keeps this consistent with the `centered`/`centeredIn` lambdas already used
elsewhere in `drawFrame()`, and avoids a second centering convention.

- [ ] **Step 2: Make the art window's fallback text scale**

`art_view.cc:372` draws the "No artwork" placeholder at
`std::max(18.0f, kMinReadableTextSizePx)` — which is always 18.2857, since the
floor wins. On a 2160px-tall monitor (the exact case a fullscreen art window is
for) it stays 18px and is nearly invisible. `ArtWindow` has its own renderer and
no `PlayerWindow` to borrow from, but `computeUiMetrics()` is a free function, so
it can just call it:

```cpp
        canvas.clear(toColor(CLR_BG_MAIN));
        // One rarely-shown placeholder string, so this window doesn't need the
        // full per-role scale — but it does need to scale: the old
        // max(18.0f, kMinReadableTextSizePx) was always 18.29px, invisible on
        // the 4K display a fullscreen art window is actually for.
        canvas.textCentered("No artwork", canvas.w() * 0.5f, canvas.h() * 0.5f,
                            computeUiMetrics(canvas.h()).text.body,
                            toColor(CLR_TEXT_DIM));
```

Add `#include "ui_metrics.hh"` to `art_view.cc`. The `kMinReadableTextSizePx`
include may become unused — check with
`grep -n "kMinReadableTextSizePx" gui/src/art_view.cc` and remove the
now-redundant `ui_min_text_size.gen.h` include only if nothing else uses it
(`ui_metrics.hh` provides it transitively either way).

- [ ] **Step 3: Test both empty states**

```bash
scripts/linux/build.sh --debug && ./build/linux_debug/gui/matrix_player
```

- Type nonsense into the sidebar search → `No matches for "..."` should be centered in the content area.
- Click **EPs**/**Singles**/**Remixes** with no such releases → `No EPs yet` centered.

The art window's fallback is Windows-only (`ArtWindow` has no Wayland
implementation yet — see `CLAUDE.md`), so on Linux verify it **compiles** only;
it cannot be exercised here.

- [ ] **Step 4: Commit**

```bash
./git_wrapper commit "Center grid empty states by measurement; scale the art window's fallback text"
```

---

## Task 9: Update `docs/UI_DESIGN_SYSTEM.md`

**Files:**
- Modify: `docs/UI_DESIGN_SYSTEM.md`

**Interfaces:** none — documentation only.

- [ ] **Step 1: Re-anchor every `file:line` citation**

```bash
grep -n "drawFrame\|drawUiIcon\|recalcLayout\|drawWarningIcon\|drawSearchField" gui/src/player_view.cc | grep "^[0-9]*:void\|^[0-9]*:static"
```

Update every `player_view.cc:NNN` in the doc to the real line.

- [ ] **Step 2: Rewrite §2 (color tokens)**

- `CLR_TEXT_SECONDARY` 170, `CLR_TEXT_DIM` 128, with contrast figures 8.5:1 and 5.0:1.
- Add a `CLR_WARNING` row: `224,180,40` — non-blocking warnings (bitperfect mismatch).
- Correct the sentence claiming `CLR_ERROR` is the only reserved semantic color.
- Add: "the ladder is ordered 242 > 170 > 128; the tightest pair in the app is DIM on `CLR_BG_TRANSPORT` at 4.58:1."

- [ ] **Step 3: Rewrite §3 (typography)**

Replace the seven-percentage table with the five roles and the single model:
`caption = kMinReadableTextSizePx × uiScale`, `role(n) = caption × 1.18ⁿ`,
`uiScale = UiScale{1080, floorScale 1.0}.factor(H)`. State the uniform-clamp
property explicitly, and that `caption`/`secondary` share a size by design.

- [ ] **Step 4: Rewrite §4 (spacing)**

New `SP_*` values `6 · 13 · 20 · 33 · 65`, authored at the 1080 reference.
Replace the "adoption is in progress" caveat with the space/stroke rule:
spacing through `space()`, stroke weights through `stroke()` (whole device
pixels), never a bare literal.

- [ ] **Step 5: Add the missing components**

- §7: add `drawWarningIcon` (triangle + `!` bar + dot, same 36-unit grid).
- §8: add the bitperfect warning banner (full-width strip above the transport bar, `CLR_WARNING` tint + hairlines + icon) and `panels::drawScrollbar` (track + proportional thumb, `CLR_TEXT_SECONDARY`, affordance only — not draggable).

- [ ] **Step 6: Extend the §10 checklist**

Add:
8. **Spacing through `space()`, strokes through `stroke()`.** Never a bare pixel literal, never a hand-written `* scale`.
9. **The text-color ladder is ordered** 242 > 170 > 128. Check contrast before changing any of the three.

- [ ] **Step 7: Commit**

```bash
./git_wrapper commit "Update UI_DESIGN_SYSTEM.md to the rigor-pass model"
```

---

## Task 10: Full verification pass

**Files:** none modified (unless a defect is found).

- [ ] **Step 1: Run the test**

```bash
scripts/linux/build.sh --debug && ./build/linux_debug/gui/ui_metrics_test
```

Expected: `ui_metrics_test: all assertions passed`.

- [ ] **Step 2: Confirm nothing references the retired symbols**

```bash
grep -rn "uiScale_\|textSizes_\|kTextSize.*Pct\|kReferenceWindowHeight\|ResponsiveTextScale" gui/src/
```

Expected: no output.

- [ ] **Step 3: Release build**

```bash
scripts/linux/build.sh --release && ./build/linux/gui/matrix_player
```

Walk the full UI against spec §9's expected-delta list:

| area | expected |
|---|---|
| grid | tile size/spacing/hover halo/glow **unchanged**; titles slightly larger; artist lines brighter |
| sidebar | width, row heights, selection bar **unchanged**; brand unchanged |
| transport | bar height, button size/spacing **unchanged**; now-playing title larger; clock brighter; DSP tag dimmer |
| album view | column layout, art size, row heights **unchanged**; title larger; durations brighter |
| settings panels | header/rows/buttons/scrollbar **unchanged**; header text larger |
| empty states | centered |
| Essential (Alt+L) | no overlap; art sharp on first switch |

- [ ] **Step 4: Confirm the tight contrast pair by eye**

The `REF EQ` tag on the transport bar is the 4.58:1 pair. It should read as
clearly de-emphasized but still legible. If it disappears against the bar,
stop — `CLR_TEXT_DIM` needs revisiting and the spec's §5 assumption was wrong.

- [ ] **Step 5: Commit any fixes, then report**

Report to the user: the six text-size deltas, the color change, both Essential
fixes, and anything that moved which should not have.

---

## Notes for the implementer

- **`git_wrapper` runs `git add -A`.** Check `git status` before each commit; if unrelated files are dirty, ask the user rather than sweeping them in.
- **The "no visual change" claim is per-task.** Tasks 3, 4 and 5 must each be run and eyeballed *individually*. Batching them defeats the only verification available for the re-authoring arithmetic.
- **When a re-authored value looks wrong**, the check is `round(old_literal × 1.63389)`. If that does not match what the plan says, trust the arithmetic and flag the discrepancy.
- **There is no Windows machine here.** `windows_host.cc` and `wasapi_output.cc` are untouched by this plan, but `player_view.cc`'s `#ifdef _WIN32` blocks in `drawAudioSettings` do contain `uiScale_` uses — migrate them by inspection; they cannot be compile-checked on this machine.

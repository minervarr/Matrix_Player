# Bar A Typographic Rail + EQ Switcher Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
> On leaving plan mode, copy this file to `docs/superpowers/plans/2026-09-11-bar-a-typographic-rail.md` and commit it with the first task.

**Goal:** Separate bar A's cells by typeface and weight (Settings in Terminus, Find as a regular Computer Modern `F`, filters in bold), move Find beside Settings, auto-close an empty search, and replace the along-the-bar AutoEQ list with a full-page `EqSwitcher` scene.

**Architecture:** `rail_layout` (pure geometry) gets the new order; `bar_a` (pure drawing from a model) gets the new faces and loses the unfurled list; a new pure `terminus_glyph` module draws one baked 1-bit glyph as run-length rects; `player_view` gains the `EqSwitcher` scene and the search auto-close. Terminus is baked by a committed Python tool into a committed header, so the C++ build never needs Python.

**Tech Stack:** C++17, vk_canvas `Canvas`, CMake/Ninja, Python 3 + Pillow 12 (bake tool only), plain-`assert` Debug tests.

**Spec:** `docs/superpowers/specs/2026-09-11-bar-a-typographic-rail-design.md` (commits `49275f2`, `9c84dae`).

## Global Constraints

- Commit ONLY with `./git_wrapper commit "<msg>"`; bare message, no `Co-Authored-By`/`Claude-Session` trailers (workspace CLAUDE.md overrides).
- No change to any submodule (`framework/*`). Everything lands in `gui/`, `tools/`, `docs/`, `CLAUDE.md`.
- Scripted edits that insert non-ASCII into C++: use real characters or raw strings — never `\xE2\x80\x94` inside a normal Python string (memory: heredoc-utf8-double-encoding). Grep for `\xc3\xa2\xc2\x80\xc2\x94` before every commit.
- Terminus: integer scale only, nearest sampling; strikes `ter-u16b.otb`, `ter-u32b.otb`; LICENSE (SIL OFL 1.1) copied alongside.
- Colour ladder unchanged: filters PRIMARY 242, Find SECONDARY 170, Settings DIM 128.
- Test convention: Debug-only executables in `gui/CMakeLists.txt`, `#undef NDEBUG`, plain `assert`, no engine linked.
- Verification loop per task: `scripts/linux/build.sh --debug` → all test binaries → captures as stated in the task.
- Device verification is the user's; never claim on-device behaviour.

## File map

| File | Responsibility | Task |
|---|---|---|
| `tools/terminus/bake_glyphs.py` (new) | Bake chosen Terminus glyphs from `.otb` to 1-bit rows | 1 |
| `tools/terminus/{ter-u16b.otb,ter-u32b.otb,LICENSE,README.md}` (new) | Bake inputs + licence + how to regenerate | 1 |
| `gui/src/terminus_glyphs.gen.h` (new, generated, committed) | Baked bits | 1 |
| `gui/src/terminus_glyph.{hh,cc}` (new) | Pure: strike/scale choice + bit rows → run-length rects; thin Canvas draw | 1 |
| `gui/src/terminus_glyph_test.cc` (new) | Pure test of the above | 1 |
| `gui/src/rail_layout.{hh,cc}` | New order; remove eqNone/eqList/eqListOpen/railListRow | 2 |
| `gui/src/rail_layout_test.cc` | Anchors for the new order; rotation invariant kept | 2 |
| `gui/src/bar_a.{hh,cc}` | New faces per cell; no × half; no unfurled list | 3 |
| `gui/src/player_view.{hh,cc}` | `EqSwitcher` scene; search auto-close; bar A wiring | 3, 4 |
| `tools/ui_capture/main.cc` | retire `43-autoeq-unfurled`, add `45-eq-switcher` | 4 |
| `gui/CMakeLists.txt`, `android/CMakeLists.txt` | new sources + test | 1 |
| `docs/UI_DESIGN_SYSTEM.md`, `CLAUDE.md` | rail section, three-S note, rule 6 | 5 |

---

### Task 1: Terminus glyph — bake, pure run-length geometry, Canvas draw

Ported from Economycs (`tools/bake_terminus.py`, `gui/src/term.cc:72-100`), with its five known defects fixed: inputs read from this repo not `/usr/share/fonts`; paths built from `HERE`; `getbbox` called once; a missing glyph is a hard error not a blank cell; pixels verified to be exactly 0/255.

**Files:**
- Create: `tools/terminus/bake_glyphs.py`, `tools/terminus/README.md`
- Copy in: `tools/terminus/ter-u16b.otb`, `tools/terminus/ter-u32b.otb`, `tools/terminus/LICENSE` (from `~/Files/code/active/Economycs/assets/fonts/terminus/`)
- Generate + commit: `gui/src/terminus_glyphs.gen.h`
- Create: `gui/src/terminus_glyph.hh`, `gui/src/terminus_glyph.cc` (pure), `gui/src/terminus_glyph_draw.cc` (Canvas), `gui/src/terminus_glyph_test.cc`
- Modify: `gui/CMakeLists.txt` (`MATRIX_PLAYER_PORTABLE_SOURCES` 38-55, `matrix_ui_capture` 385-402, new test after `grid_layout_test` ~331), `android/CMakeLists.txt` (259-280)

**Interfaces:**
- Produces:
  - `struct terminus::Glyph { char32_t cp; int cellW; int cellH; const std::uint32_t* rows; };` (gen header; bit 0 = leftmost pixel)
  - `struct terminus::Pick { const terminus::Glyph* glyph = nullptr; int scale = 1; };`
  - `terminus::Pick terminus::pickStrike(char32_t cp, float targetPx);` — `targetPx < 24` → 16 px strike ×1; else 32 px strike × `max(1, lround(targetPx/32))`; unknown cp → `glyph == nullptr`.
  - `struct terminus::Run { int x, y, w, h; };` and `void terminus::runs(const Glyph& g, int scale, std::vector<Run>& out);` — device-pixel rects relative to the glyph's top-left.
  - `bool drawTerminusGlyph(Canvas& c, const LayoutRect& rc, char32_t cp, float targetPx, const Color& col);` — centred in `rc`, origin FLOORED so every edge is a whole pixel; returns false if no glyph.

- [ ] **Step 1: Copy the font inputs**

```bash
mkdir -p tools/terminus
cp ~/Files/code/active/Economycs/assets/fonts/terminus/{ter-u16b.otb,ter-u32b.otb,LICENSE} tools/terminus/
```

- [ ] **Step 2: Write the bake tool** — `tools/terminus/bake_glyphs.py`

```python
#!/usr/bin/env python3
"""Bake the Terminus glyphs bar A draws into gui/src/terminus_glyphs.gen.h.

Terminus is a BITMAP font, so it cannot go through the app's text engine
(RasterFont bakes from outlines, and FontStyle's four slots are all in use).
Instead the few glyphs the UI needs are baked here, one bit per pixel, and drawn
as run-length rectangles -- the method Economycs uses for its whole interface.

Usage:  python3 tools/terminus/bake_glyphs.py      (needs Pillow)
The output is committed, so a normal C++ build never needs Python.
"""
import os
import sys

try:
    from PIL import Image, ImageFont
except ImportError as e:
    sys.exit(f"missing dependency: {e}\n  pip install pillow")

HERE = os.path.dirname(os.path.abspath(__file__))
HDR_OUT = os.path.join(HERE, "..", "..", "gui", "src", "terminus_glyphs.gen.h")
STRIKES = [16, 32]      # the only strikes that are integer multiples of 8x16
GLYPHS = "S"            # Settings. Add characters here, rerun, commit both.


def bake(size, ch):
    path = os.path.join(HERE, f"ter-u{size}b.otb")
    # BASIC layout: Raqm (the default) fails on some slots of a bitmap strike.
    font = ImageFont.truetype(path, size, layout_engine=ImageFont.Layout.BASIC)
    cw = int(round(font.getlength("M")))
    if cw <= 0 or cw > 32:
        sys.exit(f"{path}: cell width {cw} -- not a monospace strike <= 32 px?")
    mask = font.getmask(ch, mode="1")
    mw, mh = mask.size
    if not (mw and mh):
        sys.exit(f"{path}: no glyph for {ch!r}")
    glyph = Image.frombytes("L", (mw, mh), bytes(mask))
    img = Image.new("L", (cw, size), 0)
    img.paste(glyph, (0, font.getbbox(ch)[1]))   # getmask already includes x bearing
    rows = []
    for y in range(size):
        bits = 0
        for x in range(cw):
            v = img.getpixel((x, y))
            if v not in (0, 255):
                sys.exit(f"{path}: {ch!r} has a grey pixel ({v}) -- not a bitmap strike")
            if v:
                bits |= 1 << x            # bit 0 is the LEFTMOST pixel
        rows.append(bits)
    if not any(rows):
        sys.exit(f"{path}: {ch!r} baked empty")
    return cw, size, rows


def main():
    out = [
        "// GENERATED by tools/terminus/bake_glyphs.py -- do not edit.",
        "//",
        "// Terminus (https://terminus-font.sourceforge.net/), SIL Open Font",
        "// License 1.1 -- see tools/terminus/LICENSE. Bold strikes only.",
        "//",
        "// One uint32_t per pixel row, bit 0 = leftmost pixel. Drawn by",
        "// gui/src/terminus_glyph*.cc as run-length rectangles at an INTEGER",
        "// scale; see that header for why nothing here is ever resampled.",
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        "namespace terminus {",
        "",
        "struct Glyph {",
        "    char32_t cp;",
        "    int cellW;",
        "    int cellH;",
        "    const std::uint32_t* rows;   // cellH entries",
        "};",
        "",
    ]
    for size in STRIKES:
        entries = []
        for ch in GLYPHS:
            cw, chh, rows = bake(size, ch)
            name = f"kBold{size}_U{ord(ch):04X}"
            out.append(f"// {ch!r}: Terminus Bold {size}px, {cw}x{chh}")
            out.append(f"inline constexpr std::uint32_t {name}[] = {{")
            for i in range(0, len(rows), 8):
                out.append("    " + ", ".join(f"0x{v:08X}" for v in rows[i:i + 8]) + ",")
            out.append("};")
            out.append("")
            entries.append(f"    {{U'\\u{ord(ch):04X}', {cw}, {chh}, {name}}},")
        out.append(f"inline constexpr Glyph kBold{size}[] = {{")
        out.extend(entries)
        out.append("};")
        out.append("")
    out.append("}  // namespace terminus")
    with open(HDR_OUT, "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")
    print(f"wrote {os.path.relpath(HDR_OUT, HERE)}  ({len(GLYPHS)} glyph(s) x {len(STRIKES)} strikes)")


if __name__ == "__main__":
    main()
```

- [ ] **Step 3: Run the bake**

Run: `python3 tools/terminus/bake_glyphs.py`
Expected: `wrote ../../gui/src/terminus_glyphs.gen.h  (1 glyph(s) x 2 strikes)`; the header holds `kBold16_U0053[16]`, `kBold32_U0053[32]`, `kBold16[]`, `kBold32[]`.

- [ ] **Step 4: Write the failing test** — `gui/src/terminus_glyph_test.cc`

```cpp
// terminus_glyph_test -- the baked Terminus glyph and its run-length rects.
// Links src/terminus_glyph.cc and nothing else. Convention matches
// framework/vk_canvas/core/tests/*.cc: plain assert(), NDEBUG undefined.
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <vector>

#include "terminus_glyph.hh"

// Paint runs into a boolean grid and compare with the glyph's bits expanded by
// `scale`: every set bit covered, no unset bit covered, no pixel twice.
static void checkExact(const terminus::Glyph& g, int scale) {
    std::vector<terminus::Run> rs;
    terminus::runs(g, scale, rs);
    const int W = g.cellW * scale, H = g.cellH * scale;
    std::vector<int> hit((size_t)W * H, 0);
    for (const terminus::Run& r : rs) {
        assert(r.w > 0 && r.h > 0);
        assert(r.x >= 0 && r.y >= 0 && r.x + r.w <= W && r.y + r.h <= H);
        for (int y = r.y; y < r.y + r.h; ++y)
            for (int x = r.x; x < r.x + r.w; ++x) hit[(size_t)y * W + x]++;
    }
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const bool on = (g.rows[y / scale] >> (x / scale)) & 1u;
            assert(hit[(size_t)y * W + x] == (on ? 1 : 0));
        }
}

int main() {
    // The one glyph bar A draws exists in both strikes and is not blank.
    const terminus::Pick small = terminus::pickStrike(U'S', 20.0f);
    const terminus::Pick big   = terminus::pickStrike(U'S', 30.0f);
    assert(small.glyph && small.glyph->cellW == 8  && small.glyph->cellH == 16);
    assert(big.glyph   && big.glyph->cellW   == 16 && big.glyph->cellH   == 32);

    // Integer scale only, chosen from the target height.
    assert(small.scale == 1);                                   // < 24 -> 16 px x1
    assert(big.scale == 1);                                     // 30 -> 32 px x1
    assert(terminus::pickStrike(U'S', 48.0f).scale == 2);       // round(1.5)
    assert(terminus::pickStrike(U'S', 100.0f).scale == 3);      // round(3.125)
    assert(terminus::pickStrike(U'S', 1.0f).scale >= 1);        // never 0

    // A glyph that was not baked is reported, not drawn as garbage.
    assert(terminus::pickStrike(U'Q', 30.0f).glyph == nullptr);

    // The runs reproduce the bitmap exactly, at every scale we might use.
    for (int s = 1; s <= 3; ++s) { checkExact(*small.glyph, s); checkExact(*big.glyph, s); }

    printf("terminus_glyph_test: all assertions passed\n");
    return 0;
}
```

- [ ] **Step 5: Register the test and sources, run it to see it fail**

In `gui/CMakeLists.txt`, add `src/terminus_glyph.cc` and `src/terminus_glyph_draw.cc` after `src/grid_layout.cc` in BOTH `MATRIX_PLAYER_PORTABLE_SOURCES` and the `matrix_ui_capture` list (one line each — edit by hand, do NOT string-replace the indented `rail_layout.cc` line: the 12-space entry contains the 4-space one). After the `grid_layout_test` block add:

```cmake
    # terminus_glyph_test -- the baked Terminus glyph and its run-length rects.
    # Links the REAL src/terminus_glyph.cc; the Canvas half is a separate TU
    # (terminus_glyph_draw.cc), same split as ui_icons.cc / ui_icons_draw.cc.
    add_executable(terminus_glyph_test src/terminus_glyph_test.cc src/terminus_glyph.cc)
    target_include_directories(terminus_glyph_test PRIVATE src ${CMAKE_SOURCE_DIR}/framework/app_shell)
    if(NOT MSVC)
        target_compile_options(terminus_glyph_test PRIVATE -Wall)
    endif()
```

In `android/CMakeLists.txt` add `${MATRIX_GUI_SRC}/terminus_glyph.cc` and `${MATRIX_GUI_SRC}/terminus_glyph_draw.cc` after `grid_layout.cc`.

Run: `scripts/linux/build.sh --debug`
Expected: FAIL — `terminus_glyph.hh: No such file`.

- [ ] **Step 6: Implement** — `gui/src/terminus_glyph.hh`

```cpp
#pragma once

// ── One Terminus glyph, drawn as pixels ─────────────────────────────────────
//
// Terminus is a BITMAP font: strikes at fixed sizes, every pixel exactly on or
// off. It cannot enter vk_canvas's text engine, which bakes from outlines and
// whose four FontStyle slots are all taken, so the glyphs the UI needs are
// baked by tools/terminus/bake_glyphs.py into terminus_glyphs.gen.h and drawn
// here as run-length rectangles -- one rect per horizontal run of set pixels.
//
// INTEGER SCALE ONLY, nearest sampling. A fractional scale resamples pixels
// into grey, and a 1-bit face with grey in it is a blurred face. Economycs's
// docs/type-hierarchy.md states the rule; this follows it.
//
// The geometry half is pure (terminus_glyph.cc, tested by terminus_glyph_test);
// the Canvas half is terminus_glyph_draw.cc -- the ui_icons.cc split.
#include <cstdint>
#include <vector>

#include "layout_rect.hh"          // framework/app_shell, same as ui_icons.hh
#include "terminus_glyphs.gen.h"

// Forward-declared exactly as ui_icons.hh does, so the pure half and its test
// never include canvas.hh.
class Canvas;
struct Color;

namespace terminus {

struct Pick {
    const Glyph* glyph = nullptr;   // null: this character was not baked
    int          scale = 1;
};

// The strike and integer scale for `cp` drawn about `targetPx` tall: the 16 px
// strike at x1 below 24 px, otherwise the 32 px strike at round(target / 32).
Pick pickStrike(char32_t cp, float targetPx);

// Device-pixel rectangles relative to the glyph's top-left, one per run of set
// pixels in a row, each `scale` tall.
struct Run { int x, y, w, h; };
void runs(const Glyph& g, int scale, std::vector<Run>& out);

} // namespace terminus

// Draw `cp` centred in `rc`. The origin is FLOORED so every edge lands on a
// whole pixel -- the shape path antialiases a fractional edge, and a grey
// pixel is precisely what a bitmap face must not have. False if not baked.
bool drawTerminusGlyph(Canvas& c, const LayoutRect& rc, char32_t cp, float targetPx,
                       const Color& col);
```

`gui/src/terminus_glyph.cc`

```cpp
#include "terminus_glyph.hh"

#include <algorithm>
#include <cmath>

namespace terminus {

namespace {
const Glyph* find(const Glyph* table, int n, char32_t cp) {
    for (int i = 0; i < n; ++i) if (table[i].cp == cp) return &table[i];
    return nullptr;
}
template <int N> const Glyph* find(const Glyph (&table)[N], char32_t cp) {
    return find(table, N, cp);
}
} // namespace

Pick pickStrike(char32_t cp, float targetPx) {
    Pick p;
    if (targetPx < 24.0f) {
        p.glyph = find(kBold16, cp);
        p.scale = 1;
    } else {
        p.glyph = find(kBold32, cp);
        p.scale = std::max(1, (int)std::lround(targetPx / 32.0f));
    }
    return p;
}

void runs(const Glyph& g, int scale, std::vector<Run>& out) {
    out.clear();
    const int s = std::max(1, scale);
    for (int row = 0; row < g.cellH; ++row) {
        const std::uint32_t bits = g.rows[row];
        if (bits == 0) continue;
        int x0 = -1;
        // `bit == cellW` is a sentinel that is always off: it flushes a run
        // that reaches the right edge.
        for (int bit = 0; bit <= g.cellW; ++bit) {
            const bool on = bit < g.cellW && ((bits >> bit) & 1u);
            if (on && x0 < 0) {
                x0 = bit;
            } else if (!on && x0 >= 0) {
                out.push_back({x0 * s, row * s, (bit - x0) * s, s});
                x0 = -1;
            }
        }
    }
}

} // namespace terminus
```

`gui/src/terminus_glyph_draw.cc`

```cpp
#include "terminus_glyph.hh"

#include <cmath>

#include "canvas.hh"
#include "layout_rect.hh"

bool drawTerminusGlyph(Canvas& c, const LayoutRect& rc, char32_t cp, float targetPx,
                       const Color& col) {
    const terminus::Pick p = terminus::pickStrike(cp, targetPx);
    if (!p.glyph) return false;
    const float w = (float)(p.glyph->cellW * p.scale);
    const float h = (float)(p.glyph->cellH * p.scale);
    const float ox = std::floor(rc.left + ((rc.right - rc.left) - w) * 0.5f);
    const float oy = std::floor(rc.top  + ((rc.bottom - rc.top) - h) * 0.5f);
    std::vector<terminus::Run> rs;
    terminus::runs(*p.glyph, p.scale, rs);
    for (const terminus::Run& r : rs)
        c.rect(ox + r.x, oy + r.y, (float)r.w, (float)r.h, col);
    return true;
}
```

(`layout_rect.hh` lives in `framework/app_shell`; `LayoutRect` is a `struct`, `Canvas` a `class`, `Color` a `struct` — the tags `ui_icons.hh` already uses.)

- [ ] **Step 7: Build and run all tests**

Run: `scripts/linux/build.sh --debug && ./build/linux_debug/gui/terminus_glyph_test` then the full test list (CLAUDE.md, now 13 binaries with this one).
Expected: `terminus_glyph_test: all assertions passed`; every other test passes; ui_capture PNGs byte-identical to HEAD (nothing draws the glyph yet).

- [ ] **Step 8: Docs + commit**

`tools/terminus/README.md`: what the folder is, the OFL licence, `python3 tools/terminus/bake_glyphs.py` to regenerate, "add a character to `GLYPHS`, rerun, commit the header". Add `./build/linux_debug/gui/terminus_glyph_test` to CLAUDE.md's test list ("twelve" → "thirteen").

```bash
grep -rlP '\xc3\xa2\xc2\x80\xc2\x94' gui/src tools/terminus || echo clean
./git_wrapper commit "Terminus glyphs for the UI: baked 1-bit, drawn as pixel runs at an integer scale"
```

---

### Task 2: New rail order and typography; the unfurled list and the × are gone

Lands as ONE commit with the `player_view` rewiring, because `player_view.cc` reads `rail_.eqNone`, `rail_.eqList` and `railListRow` (`:2624`, `:2644`, `:2654-2666`) and would not compile against the new rail alone. Until Task 3, touching the EQ box opens the existing EQ Settings panel, so every intermediate commit is a working app.

**Files:**
- Modify (full rewrite): `gui/src/rail_layout.hh`, `gui/src/rail_layout.cc`, `gui/src/rail_layout_test.cc`, `gui/src/bar_a.hh`, `gui/src/bar_a.cc`
- Modify: `gui/src/player_view.hh`, `gui/src/player_view.cc` (bar A wiring — Step 6), `tools/ui_capture/main.cc` (retire `43-autoeq-unfurled`)

**Interfaces:**
- Consumes (Task 1): `bool drawTerminusGlyph(Canvas&, const LayoutRect&, char32_t, float targetPx, const Color&)`.
- Produces:
  - `struct RailInput { LayoutRect bar; UiOrientation orient; bool bitPerfect; bool searchOpen; int cell; int eqBoxExtent; int pad; int gap; };` — `eqListOpen` removed.
  - `struct RailLayout { LayoutRect letters[kRailLetterCount]; LayoutRect search; LayoutRect settings; LayoutRect close; LayoutRect eqBox; };` — `eqNone`, `eqName`, `eqList` removed; `search` is the Find cell (or the open field).
  - `railListCapacity` / `railListRow` removed.
  - `enum class BarAItem { None, Filter, Playlists, Search, SearchClose, Settings, EqBox };` — `EqNone`, `EqName`, `EqRow`, `EqMore` removed; `EqBox` = the whole box.
  - `BarAModel` loses `eqListOpen`, `eqRows`, `eqMore`; `BarAEqRow` removed.

- [ ] **Step 1: Rewrite the test first** — `gui/src/rail_layout_test.cc` (whole file)

Two defects in the current file are fixed rather than carried over: its rotation check ran FIVE states while the comment claims eight (orientation is the pair inside each call, so four bit-perfect × search-open states cover it), and its centring assertion `slackA == slackB || slackA == slackB + 1` has an impossible second branch — with `floor` centring the far slack is the one that can be larger.

```cpp
// Asserts must stay live even though the app builds Release (NDEBUG).
#undef NDEBUG
#include <cassert>
#include <cstdio>

#include "rail_layout.hh"

namespace {

// A 1920x1080 screen at the reference scale: space(130) is 130 px, and a cell
// is square at that thickness. These are the app's real numbers, not round
// ones -- a layout test on invented sizes proves nothing about the layout the
// app actually draws.
constexpr int kW    = 1920;   // the bar's long extent
constexpr int kT    = 130;    // its thickness  (space(130), scale 1.0)
constexpr int kCell = 130;    // one cell: square at this thickness
constexpr int kEq   = 300;
constexpr int kPad  = 16;
constexpr int kGap  = 24;

bool empty(const LayoutRect& r) {
    return r.left == 0 && r.top == 0 && r.right == 0 && r.bottom == 0;
}
int  wide(const LayoutRect& r) { return r.right - r.left; }
int  tall(const LayoutRect& r) { return r.bottom - r.top; }
bool same(const LayoutRect& a, const LayoutRect& b) {
    return a.left == b.left && a.top == b.top &&
           a.right == b.right && a.bottom == b.bottom;
}

RailInput vertical(bool bitPerfect, bool searchOpen) {
    RailInput in;
    in.bar = { 0, 0, kW, kT };          // top bar: full width, thickness tall
    in.orient = UiOrientation::Vertical;
    in.bitPerfect = bitPerfect;
    in.searchOpen = searchOpen;
    in.cell = kCell; in.eqBoxExtent = kEq; in.pad = kPad; in.gap = kGap;
    return in;
}

RailInput horizontal(bool bitPerfect, bool searchOpen) {
    RailInput in = vertical(bitPerfect, searchOpen);
    in.bar = { 0, 0, kT, kW };          // left bar: thickness wide, full height
    in.orient = UiOrientation::Horizontal;
    return in;
}

// The 90-degree COUNTER-CLOCKWISE rotation, stated geometrically and NOT
// copied from rail_layout.cc's place(). In screen coordinates (y down),
// rotating a W-by-T bar counter-clockwise sends (x, y) to (y, W - x).
LayoutRect rotateCcw(const LayoutRect& r) {
    return { r.top, kW - r.right, r.bottom, kW - r.left };
}

constexpr int kRects = kRailLetterCount + 4;   // letters + search, settings, close, eqBox
void collect(const RailLayout& l, const LayoutRect* (&out)[kRects]) {
    int n = 0;
    for (int i = 0; i < kRailLetterCount; i++) out[n++] = &l.letters[i];
    out[n++] = &l.search;
    out[n++] = &l.settings;
    out[n++] = &l.close;
    out[n++] = &l.eqBox;
}

// ── The two orientations are ONE layout and a rotation ─────────────────────
void assertRotationHolds(bool bitPerfect, bool searchOpen) {
    const RailLayout v = computeRailLayout(vertical(bitPerfect, searchOpen));
    const RailLayout h = computeRailLayout(horizontal(bitPerfect, searchOpen));
    const LayoutRect* vr[kRects];
    const LayoutRect* hr[kRects];
    collect(v, vr);
    collect(h, hr);
    for (int i = 0; i < kRects; i++) {
        assert(empty(*vr[i]) == empty(*hr[i]));
        if (empty(*vr[i])) continue;
        assert(same(rotateCcw(*vr[i]), *hr[i]));
    }
}

}  // namespace

int main() {
    // ── The rare cells sit together at the near end: Settings, then Find ───
    {
        const RailLayout v = computeRailLayout(vertical(false, false));
        assert(v.settings.left == kPad);
        assert(v.search.left == v.settings.right);          // Find beside Settings
        assert(wide(v.search) == kCell && tall(v.search) == kT);
        assert(v.eqBox.left == v.search.right);             // then the EQ box
        assert(wide(v.eqBox) == kEq);                        // the box IS the name now
        for (int i = 1; i < kRailLetterCount; i++)
            assert(v.letters[i].left > v.letters[i - 1].left);
        assert(tall(v.settings) == kT && tall(v.letters[kRailAlbums]) == kT);
        assert(wide(v.letters[kRailAlbums]) == kCell);
    }

    // ── The whole point: Find is never next to a filter letter ─────────────
    // A slightly-off tap on Albums used to open search, because search was the
    // cell immediately beside it. Now the gap always stands between them.
    for (bool bp : { false, true }) {
        const RailLayout v = computeRailLayout(vertical(bp, false));
        assert(v.letters[kRailAlbums].left - v.search.right >= kGap);
        if (!bp) assert(v.letters[kRailAlbums].left - v.eqBox.right >= kGap);
    }

    // ── Bit-perfect CENTRES the letter group, Reference EQ PEGS it ─────────
    {
        const RailLayout ref = computeRailLayout(vertical(false, false));
        const RailLayout bp  = computeRailLayout(vertical(true,  false));
        assert(ref.letters[kRailPlaylists].right == kW - kPad);
        assert(bp.letters[kRailPlaylists].right < kW - kPad);
        assert(empty(bp.eqBox));

        // Centred in the space after the near cluster (Settings, Find, gap).
        const int freeA  = kPad + 2 * kCell + kGap;
        const int freeB  = kW - kPad;
        const int slackA = bp.letters[kRailAlbums].left - freeA;
        const int slackB = freeB - bp.letters[kRailPlaylists].right;
        assert(slackB - slackA == 0 || slackB - slackA == 1);   // floor puts any odd px far

        // Centring moves the group, never resizes it.
        assert(ref.letters[kRailPlaylists].right - ref.letters[kRailAlbums].left ==
               bp.letters[kRailPlaylists].right  - bp.letters[kRailAlbums].left);
    }

    // ── Horizontal, asserted directly: near end is the BOTTOM ──────────────
    {
        const RailLayout h = computeRailLayout(horizontal(false, false));
        assert(h.settings.bottom == kW - kPad);
        assert(h.search.bottom == h.settings.top);
        assert(h.eqBox.bottom == h.search.top);
        assert(h.eqBox.top - h.letters[kRailAlbums].bottom >= kGap);
        for (int i = 1; i < kRailLetterCount; i++)
            assert(h.letters[i].top < h.letters[i - 1].top);
        assert(h.letters[kRailPlaylists].top == kPad);
        assert(wide(h.settings) == kT && tall(h.letters[kRailAlbums]) == kCell);
    }

    // ── Open search: letters and box hide, Settings stays, field in between ─
    {
        const RailLayout v      = computeRailLayout(vertical(false, true));
        const RailLayout closed = computeRailLayout(vertical(false, false));
        for (int i = 0; i < kRailLetterCount; i++) assert(empty(v.letters[i]));
        assert(empty(v.eqBox));
        assert(same(v.settings, closed.settings));
        assert(v.close.right == kW - kPad && wide(v.close) == kCell);
        assert(v.search.left  == v.settings.right + kGap);
        assert(v.search.right == v.close.left - kGap);
        assert(wide(v.search) > kCell * 4);                  // a field, not a letter

        const RailLayout bpOpen = computeRailLayout(vertical(true, true));
        assert(same(bpOpen.search, v.search) && same(bpOpen.close, v.close));
    }

    // ── A narrow vertical bar shrinks every cell uniformly ─────────────────
    // A phone held upright: nine cells plus the box do not fit at 130.
    {
        RailInput in = vertical(false, false);
        in.bar = { 0, 0, 1080, kT };
        const RailLayout n = computeRailLayout(in);
        const int cell = wide(n.settings);
        assert(cell > 0 && cell < kCell);
        assert(wide(n.search) == cell);
        for (int i = 0; i < kRailLetterCount; i++) assert(wide(n.letters[i]) == cell);
        assert(n.search.left == n.settings.right && n.eqBox.left == n.search.right);
        assert(n.letters[kRailAlbums].left - n.eqBox.right >= kGap);
        assert(n.letters[kRailPlaylists].right == 1080 - kPad);
        for (int i = 1; i < kRailLetterCount; i++)
            assert(n.letters[i].left == n.letters[i - 1].right);
    }

    // ── Every state, both orientations: one layout and a rotation ──────────
    for (bool bp : { false, true })
        for (bool open : { false, true })
            assertRotationHolds(bp, open);

    // ── Degenerate bars produce nothing, and never a negative rect ─────────
    {
        RailInput z = vertical(false, false);
        z.bar = {};
        const RailLayout l = computeRailLayout(z);
        assert(empty(l.settings) && empty(l.search) && empty(l.eqBox));

        RailInput narrow = vertical(false, false);
        narrow.bar = { 0, 0, 300, kT };
        const RailLayout n = computeRailLayout(narrow);
        assert(n.search.left >= n.settings.right);
        for (int i = 0; i < kRailLetterCount; i++)
            assert(n.letters[i].right >= n.letters[i].left);
    }

    printf("rail_layout_test: all assertions passed\n");
    return 0;
}
```

- [ ] **Step 2: Build only the test, see it fail**

The new test uses only fields that exist today (`letters`, `search`, `settings`, `close`, `eqBox`), so it compiles against the OLD rail and fails at runtime. Build the one target — the app itself is about to be broken until Step 6:

Run: `cmake --build build/linux_debug --target rail_layout_test && ./build/linux_debug/gui/rail_layout_test`
Expected: abort at `v.search.left == v.settings.right` (today search sits beside Albums).

- [ ] **Step 3: Rewrite `gui/src/rail_layout.hh`**

Keep lines 1-30 of the current header (the PURE note and "The one axis"). Replace everything from `// ── The order along it` to the end with:

```cpp
// ── The order along it ───────────────────────────────────────────────────────
//
//   near [ Settings ][ Find ][ AutoEQ box ] · · · gap · · · [ letters ] far
//
// RARE cells together at the near end, FREQUENT cells alone at the far end,
// with the gap between them. Find used to be the cell right beside Albums, the
// same size as a filter letter, so a slightly-off tap on Albums opened search
// -- and opening search collapses every letter. It is now two cells and a gap
// away from the nearest filter.
//
// Settings is pinned at the very near end and NEVER moves: opening search must
// not cost what the listener already typed, so Settings stays reachable and in
// place while the letters collapse around it.
//
// The AutoEQ box follows Find. It shows the active profile's NAME and nothing
// else; touching it opens the EqSwitcher scene (player_view). In bit-perfect
// there is nothing to pick a profile FOR, so the box does not exist and the
// letter group CENTRES in the space that is left; in Reference EQ the group
// pegs to the far end. The jump between those two is instant -- no animation.
//
// With search open the letters and the box hide, a close button takes the far
// end, and the field spans the middle, starting after Settings. Open search
// looks identical in bit-perfect and in Reference EQ -- one state, not two.

// The seven filter letters, in the order they are laid out from the near end
// outward. Deliberately the READING order, not Album::ReleaseType's order,
// which is frozen by the albums table and means nothing on screen.
enum RailLetter {
    kRailAlbums = 0,
    kRailEps,
    kRailSingles,
    kRailCompilations,
    kRailLive,
    kRailRemixes,
    kRailPlaylists,
    kRailLetterCount
};

struct RailInput {
    LayoutRect    bar{};                                // bar A, in window coords
    UiOrientation orient    = UiOrientation::Horizontal;
    bool          bitPerfect = false;                   // no AutoEQ box
    bool          searchOpen = false;

    // The DESIRED extent of one cell along the long axis; the cross-axis
    // extent is always the bar's full thickness. Every cell SHRINKS uniformly
    // when nine of them plus the AutoEQ box cannot fit -- the ordinary
    // vertical bar, whose long extent is the window's width.
    int cell = 0;
    // The AutoEQ box's extent along the long axis. Ignored when it is hidden.
    int eqBoxExtent = 0;
    // Outer inset at both ends, and the gap between the near cluster and the
    // letter group.
    int pad = 0;
    int gap = 0;
};

// Every rect is in window coordinates. A hidden element is returned as {} --
// an empty rect, which every hit-test in this codebase already misses.
struct RailLayout {
    LayoutRect letters[kRailLetterCount]{};  // empty while search is open
    LayoutRect search{};    // the Find cell, or the text field while open
    LayoutRect settings{};
    LayoutRect close{};     // only while search is open
    LayoutRect eqBox{};     // the active profile's name; empty when bit-perfect or searching
};

RailLayout computeRailLayout(const RailInput& in);
```

- [ ] **Step 4: Rewrite `computeRailLayout` in `gui/src/rail_layout.cc`**

Keep lines 1-30 (`Span`, `place()`, `longExtent()` unchanged). Replace `computeRailLayout` and delete `railListCapacity` / `railListRow` (lines 147-168):

```cpp
RailLayout computeRailLayout(const RailInput& in) {
    RailLayout out;

    const int L = longExtent(in);
    if (L <= 0 || in.cell <= 0) return out;   // no bar yet, or no metrics yet

    const bool showEqBox = !in.bitPerfect && !in.searchOpen;
    const int  eqLen     = (showEqBox && in.eqBoxExtent > 0) ? in.eqBoxExtent : 0;

    // ── The cell shrinks to fit, and this is load-bearing ───────────────────
    // Nine cells (Settings + Find + seven letters) plus the AutoEQ box plus the
    // insets can exceed the bar's long extent -- the ordinary VERTICAL bar,
    // whose long extent is the window's WIDTH. Shrinking uniformly keeps every
    // filter reachable; dropping one would hide it, and overflowing would put
    // letters underneath the box, where they would still hit-test.
    int cell = in.cell;
    {
        const int cells = kRailLetterCount + 2;         // + Find + Settings
        const int fixed = in.pad * 2 + eqLen + in.gap;
        if ((long long)cells * cell + fixed > L) {
            cell = (L - fixed) / cells;
            if (cell < 1) return out;                   // nothing legible fits
        }
    }

    // Settings: pinned at the near end, in every state.
    const Span settings{ in.pad, in.pad + cell };
    out.settings = place(in, settings);

    if (in.searchOpen) {
        // Close at the far end, the field spanning everything between it and
        // Settings. The letters and the box are gone -- collapsing them is what
        // makes room for a field wide enough to hold chips.
        const Span close{ L - in.pad - cell, L - in.pad };
        out.close  = place(in, close);
        out.search = place(in, Span{ settings.b + in.gap, close.a - in.gap });
        return out;
    }

    // Find sits beside Settings: the two RARE cells together, where a tap
    // meant for a filter cannot reach them. See rail_layout.hh.
    const Span find{ settings.b, settings.b + cell };
    out.search = place(in, find);
    int nearEnd = find.b;

    if (eqLen > 0) {
        const Span box{ nearEnd, nearEnd + eqLen };
        out.eqBox = place(in, box);
        nearEnd = box.b;
    }

    const int groupLen = kRailLetterCount * cell;
    int groupStart;
    if (showEqBox) {
        // Reference EQ: pegged to the far end.
        groupStart = L - in.pad - groupLen;
    } else {
        // Bit-perfect: centred in the space that is actually free.
        const int freeA = nearEnd + in.gap;
        const int freeB = L - in.pad;
        groupStart = freeA + ((freeB - freeA) - groupLen) / 2;
    }
    // Never overlap the near cluster, however narrow the window gets.
    if (groupStart < nearEnd + in.gap) groupStart = nearEnd + in.gap;

    for (int i = 0; i < kRailLetterCount; i++) {
        const int a = groupStart + i * cell;
        out.letters[i] = place(in, Span{ a, a + cell });
    }
    return out;
}
```

- [ ] **Step 5: Rewrite `gui/src/bar_a.hh` and `gui/src/bar_a.cc`**

`bar_a.hh` — keep lines 1-37 (includes and the shared-code rule); replace from `enum class BarAItem` to the end:

```cpp
// What a point in bar A means. Deliberately NOT the desktop's integer hit
// vocabulary (kSidebar*Hit): the caller translates at its own edge.
enum class BarAItem {
    None = 0,
    Filter,       // index = a RailLetter (kRailAlbums .. kRailRemixes)
    Playlists,
    Search,       // the Find cell that OPENS search -- never the open field
    SearchClose,
    Settings,
    EqBox,        // the active profile's name; touching it opens the EqSwitcher
};

struct BarAPick {
    BarAItem item  = BarAItem::None;
    int      index = -1;
};

inline bool operator==(const BarAPick& a, const BarAPick& b) {
    return a.item == b.item && a.index == b.index;
}
inline bool operator!=(const BarAPick& a, const BarAPick& b) { return !(a == b); }

// Everything bar A needs to draw itself, as values.
struct BarAModel {
    LayoutRect    bar{};
    UiOrientation orient = UiOrientation::Horizontal;
    RailLayout    rail{};
    UiMetrics     metrics{};

    bool searchOpen     = false;
    bool searchFocused  = false;
    bool settingsActive = false;
    bool eqSwitcherOpen = false;   // the box reads as selected while its scene is up

    std::string searchQuery;

    int  activeLetter    = -1;     // a RailLetter, or -1
    bool playlistsActive = false;

    // The AutoEQ box. Its presence is decided by the geometry (rail.eqBox is
    // empty in bit-perfect and while searching), so there is no second flag.
    bool        eqNone      = true;   // no profile is selected
    bool        eqTentative = false;  // selected, not yet credited
    std::string eqName;

    BarAPick hovered;   // Android leaves this None
};

void     drawBarA(Canvas& canvas, const BarAModel& m);
BarAPick barAHitTest(const BarAModel& m, int x, int y);

// One text-input field, shared by bar A's search and the EQ panel's profile
// search, so the two boxes cannot drift apart.
void drawSearchField(Canvas& canvas, const LayoutRect& rc, const std::string& text,
                     bool focused, const char* placeholder, float textSize);
```

`bar_a.cc` — keep lines 1-33 (includes, `toRect`/`toColor`/`ptIn`, `drawSearchField`) and add `#include "terminus_glyph.hh"` after `text_util.hh`. Replace from line 35 to the end:

```cpp
// ── Drawing ──────────────────────────────────────────────────────────────────
//
// THE LETTERS ARE NOT ROTATED in the horizontal layout: a single capital reads
// the same upright at any orientation. Rotation is only for text whose LENGTH
// runs along the bar -- the AutoEQ profile name.
//
// Three cells used to be told apart only by theme.hh's grey ladder, because
// three of them said "S". They are now told apart by TYPEFACE AND WEIGHT first,
// and the ladder only confirms:
//
//   Settings  S   Terminus (a 1-bit pixel face)   DIM 128
//   Find      F   Computer Modern, regular        SECONDARY 170
//   filters   A E S C L R P   Computer Modern BOLD   PRIMARY 242
//
// The filters are the most used cells, so they get the heaviest weight, which
// also holds up best in the shrunken cells of a phone held upright. Settings is
// machine chrome, not music, so it gets the terminal face. The one remaining
// pair of equal letters -- Settings and Singles -- is a pixel S beside a bold
// serif S, which do not resemble each other at any size.

namespace {

void drawEqBox(Canvas& canvas, const BarAModel& m) {
    if (m.rail.eqBox.right <= m.rail.eqBox.left) return;

    const bool vertical = (m.orient == UiOrientation::Vertical);
    const Rect box = toRect(m.rail.eqBox);

    // A frame, so the region reads as its own thing rather than as more cells.
    const float hair = m.metrics.stroke(1.0f);
    canvas.rect(box.x, box.y, box.w, box.h, toColor(CLR_BG_TRANSPORT));
    if (vertical) {
        canvas.rect(box.x, box.y, hair, box.h, toColor(CLR_SEPARATOR));
        canvas.rect(box.x + box.w - hair, box.y, hair, box.h, toColor(CLR_SEPARATOR));
    } else {
        canvas.rect(box.x, box.y, box.w, hair, toColor(CLR_SEPARATOR));
        canvas.rect(box.x, box.y + box.h - hair, box.w, hair, toColor(CLR_SEPARATOR));
    }
    if (m.eqSwitcherOpen)
        canvas.rect(box.x, box.y, box.w, box.h,
                    toColor(CLR_ACCENT, UI_SELECT_TINT_ALPHA), UI_CORNER_RADIUS);
    else if (m.hovered.item == BarAItem::EqBox)
        canvas.rect(box.x, box.y, box.w, box.h, toColor(CLR_HOVER), UI_CORNER_RADIUS);

    // The name is the one text in bar A whose LENGTH runs along the bar, so it
    // is rotated in the horizontal layout. Canvas honours setRotation() for
    // text; it does not for image().
    const std::string text = m.eqNone ? "No AutoEQ" : m.eqName;
    const float sz  = m.metrics.text.secondary;
    const float pad = m.metrics.space(SP_SM);
    const ColorRef clr = m.eqNone ? CLR_TEXT_DIM
                       : (m.eqTentative ? CLR_TEXT_SECONDARY : CLR_TEXT_PRIMARY);
    if (vertical) {
        canvas.text(truncateToWidth(canvas, text, box.w - pad * 2, sz, FontStyle::Roman),
                    box.x + pad, box.y + box.h * 0.5f - sz * 0.5f, sz, toColor(clr));
        return;
    }
    const float cx = box.x + box.w * 0.5f, cy = box.y + box.h * 0.5f;
    canvas.setRotation(-1.57079633f, cx, cy);
    const std::string shown = truncateToWidth(canvas, text, box.h - pad * 2, sz, FontStyle::Roman);
    const float tw = canvas.textWidth(shown, sz);
    canvas.text(shown, cx - tw * 0.5f, cy - sz * 0.5f, sz, toColor(clr));
    canvas.clearRotation();
}

}  // namespace

void drawBarA(Canvas& canvas, const BarAModel& m) {
    const Rect bar = toRect(m.bar);
    if (bar.w <= 0 || bar.h <= 0) return;

    canvas.rect(bar.x, bar.y, bar.w, bar.h, toColor(CLR_BG_SIDEBAR));

    // The hairline sits on the bar's INNER edge -- the one facing the content.
    const float hair = m.metrics.stroke(1.0f);
    if (m.orient == UiOrientation::Vertical)
        canvas.rect(bar.x, bar.y + bar.h - hair, bar.w, hair, toColor(CLR_SEPARATOR));
    else
        canvas.rect(bar.x + bar.w - hair, bar.y, hair, bar.h, toColor(CLR_SEPARATOR));

    // A cell's background: selected (accent tint + a 3px accent bar on the
    // inner edge, pointing at the content it filters) or hovered (neutral
    // grey). False when the cell is hidden, so the caller draws nothing.
    auto cellBg = [&](const LayoutRect& lr, bool active, bool hovered) -> bool {
        if (lr.right <= lr.left) return false;
        const Rect r = toRect(lr);
        if (active) {
            canvas.rect(r.x, r.y, r.w, r.h,
                        toColor(CLR_ACCENT, UI_SELECT_TINT_ALPHA), UI_CORNER_RADIUS);
            const float t = m.metrics.stroke(3.0f);
            if (m.orient == UiOrientation::Vertical)
                canvas.rect(r.x, r.y + r.h - t, r.w, t, toColor(CLR_ACCENT));
            else
                canvas.rect(r.x + r.w - t, r.y, t, r.h, toColor(CLR_ACCENT));
        } else if (hovered) {
            canvas.rect(r.x, r.y, r.w, r.h, toColor(CLR_HOVER), UI_CORNER_RADIUS);
        }
        return true;
    };
    // One Computer Modern glyph, centred, in the given weight.
    auto glyph = [&](const LayoutRect& lr, const char* g, FontStyle style, ColorRef clr) {
        const Rect r  = toRect(lr);
        const float sz = m.metrics.text.title;
        const float w  = canvas.textWidthStyled(g, sz, style);
        canvas.textStyled(g, r.x + (r.w - w) * 0.5f, r.y + r.h * 0.5f - sz * 0.5f,
                          sz, toColor(clr), style);
    };

    if (!m.searchOpen) {
        // Initials in the reading order rail_layout.hh fixes. BOLD: these are
        // the cells the listener uses most.
        static const char* kGlyphs[] = { "A", "E", "S", "C", "L", "R" };
        for (int i = kRailAlbums; i <= kRailRemixes; i++) {
            const bool active = (m.activeLetter == i);
            if (cellBg(m.rail.letters[i], active,
                       m.hovered.item == BarAItem::Filter && m.hovered.index == i && !active))
                glyph(m.rail.letters[i], kGlyphs[i], FontStyle::Bold,
                      active ? CLR_ACCENT : CLR_TEXT_PRIMARY);
        }
        if (cellBg(m.rail.letters[kRailPlaylists], m.playlistsActive,
                   m.hovered.item == BarAItem::Playlists && !m.playlistsActive))
            glyph(m.rail.letters[kRailPlaylists], "P", FontStyle::Bold,
                  m.playlistsActive ? CLR_ACCENT : CLR_TEXT_PRIMARY);

        // Find: an F no filter uses, in the REGULAR weight, one step down the
        // ladder -- it is about the music, but it is not a section.
        if (cellBg(m.rail.search, false, m.hovered.item == BarAItem::Search))
            glyph(m.rail.search, "F", FontStyle::Roman, CLR_TEXT_SECONDARY);
    } else {
        // Open search: the field spans the middle, a close cell at the far end.
        drawSearchField(canvas, m.rail.search, m.searchQuery, m.searchFocused,
                        "Search your library", m.metrics.text.secondary);
        if (cellBg(m.rail.close, false, m.hovered.item == BarAItem::SearchClose))
            glyph(m.rail.close, "×", FontStyle::Roman, CLR_TEXT_SECONDARY);
    }

    // Settings: pinned at the near end in every state, in TERMINUS. Falls back
    // to a Computer Modern S only if the baked glyph is missing, which
    // terminus_glyph_test makes a build-time failure rather than a runtime one.
    if (cellBg(m.rail.settings, m.settingsActive,
               m.hovered.item == BarAItem::Settings && !m.settingsActive)) {
        const ColorRef c = m.settingsActive ? CLR_ACCENT : CLR_TEXT_DIM;
        if (!drawTerminusGlyph(canvas, m.rail.settings, U'S', m.metrics.text.title, toColor(c)))
            glyph(m.rail.settings, "S", FontStyle::Roman, c);
    }

    drawEqBox(canvas, m);
}

// ── Hit-testing ──────────────────────────────────────────────────────────────
// The same rects the drawing used, so the two cannot disagree.
BarAPick barAHitTest(const BarAModel& m, int x, int y) {
    for (int i = kRailAlbums; i <= kRailRemixes; i++)
        if (ptIn(m.rail.letters[i], x, y)) return { BarAItem::Filter, i };
    if (ptIn(m.rail.letters[kRailPlaylists], x, y)) return { BarAItem::Playlists, -1 };
    if (ptIn(m.rail.settings, x, y))               return { BarAItem::Settings, -1 };
    if (ptIn(m.rail.eqBox, x, y))                  return { BarAItem::EqBox, -1 };
    if (ptIn(m.rail.close, x, y))                  return { BarAItem::SearchClose, -1 };
    // The search rect is the Find cell when closed and the field when open;
    // only the former is a click target -- the field handles its own focus.
    if (ptIn(m.rail.search, x, y) && !m.searchOpen) return { BarAItem::Search, -1 };
    return {};
}
```

(`"×"` is the multiplication sign as a real UTF-8 character literal, the same glyph the old code wrote as `"\xC3\x97"`; either compiles to the same bytes. If editing via a script, paste the character — never a Python `\x` escape.)

- [ ] **Step 5b: Run the rail test green**

Run: `cmake --build build/linux_debug --target rail_layout_test && ./build/linux_debug/gui/rail_layout_test`
Expected: `rail_layout_test: all assertions passed`.

- [ ] **Step 6: Rewire `player_view` to the new rail**

Everything that fed the along-the-bar list goes. Line numbers are as of `66c9643`; match on the quoted text, not the number.

`gui/src/player_view.hh`:
- In the sentinel block (`:1114-1133`) delete `kSidebarHpMoreHit = 7`, `kSidebarHpNoneHit = 9`, the unused `kSidebarEqNoneHit = 13`, and `kSidebarHpRowBase = 100`. Change `kSidebarEqBoxHit`'s comment to `// the AutoEQ box (opens the EqSwitcher)`.
- Delete `struct HpRow`, `hpRows_`, `hpNoneRc_`, `hpMoreRc_`, `eqNameRc_`, the unfurl comment and `bool eqListOpen_` (`:1748-1762`).

`gui/src/player_view.cc`:
- `barAModel()` (`:6995`): delete `m.eqListOpen = eqListOpen_;` and the whole block from `// Which profile a row IS stays app data.` through `m.eqMore = hpMoreRc_;`.
- `sidebarHitToPick()` (`:7052`): delete `if (nav >= kSidebarHpRowBase) ...`, `case kSidebarHpNoneHit`, `case kSidebarHpMoreHit`; change `case kSidebarEqBoxHit: return { BarAItem::EqName, -1 };` to `return { BarAItem::EqBox, -1 };`.
- `pickToSidebarHit()` (`:7073`): delete the `EqRow`, `EqNone`, `EqMore` cases; change `case BarAItem::EqName:` to `case BarAItem::EqBox:`.
- `recalcLayout()` (`:2618`): delete `ri.eqListOpen  = eqListOpen_;`, and delete everything from `// ── The AutoEQ box's hit model` through the closing `}` of `} else if (eqListOpen_) { ... }` (`:2631-2667`).
- `openSearch()` (`:6958`): delete `eqListOpen_    = false;      // two unfurled things at once is a mess`.
- `handleClick()` sidebar branch: replace from `// The headphone sentinels MUST be tested before the` through the end of the `else if (nav == kSidebarHpMoreHit) { ... }` block with:

```cpp
        // The sentinels MUST be tested before the `nav >= 0` branch below,
        // which casts whatever it gets into an AlbumTypeFilter.
        if (nav == kSidebarEqBoxHit) {
            // Until the EqSwitcher scene lands (next task), the box opens the
            // EQ Settings panel -- the route "Search more..." used to take.
            onEqSettings();            // clears panelFromSidebar_, as openers do
            settingsOpen_     = true;
            panelFromSidebar_ = true;
            return;
        } else if (nav == kSidebarSearchHit) {
            openSearch();
            return;
        } else if (nav == kSidebarSearchCloseHit) {
            closeSearch();
            return;
        } else if (nav == kSidebarPlaylistsHit) {
```

(the old `kSidebarSearchHit` / `kSidebarSearchCloseHit` branches are the same two lines each — they are now simply inside this chain; the `kSidebarPlaylistsHit` branch and everything after it are unchanged.)
- `captureGoTo()`: delete the `"43-autoeq-unfurled"` block (`:10222-10235`) and the orphaned comment above `"44-transport-ordinal"` (`:10192-10194`, "The AutoEQ switcher unfurled…").

`tools/ui_capture/main.cc`: delete `"43-autoeq-unfurled",` from `kStates[]`.

Then prove nothing is left:

Run: `grep -n "eqListOpen\|hpRows_\|hpNoneRc_\|hpMoreRc_\|eqNameRc_\|kSidebarHp\|railList\|EqNone\|EqRow\|EqMore\|EqName\|eqNone{\|rail_.eqNone\|rail_.eqList" gui/src/*.cc gui/src/*.hh tools/ui_capture/main.cc`
Expected: only `BarAModel::eqNone` uses remain (the bool meaning "no profile" — `m.eqNone = eqCurrent_.name.empty();` and its reads in `bar_a.cc`), nothing else.

- [ ] **Step 7: Build, test, capture**

Run: `scripts/linux/build.sh --debug`, then every test binary (13).
Expected: all pass, `rail_layout_test: all assertions passed`.
Then `matrix_ui_capture --out <dir>` (default, empty library) and `--fixture 60 --frame 720x1640` / `--frame 1640x720` with `--only 10-grid`. Every bar-A-bearing capture changes; LOOK at `10-grid-albums` at both phone sizes: `[pixel S][F] | No AutoEQ | A E S C L R P` in bold, Find two cells and a gap from Albums.

- [ ] **Step 8: Commit**

```bash
grep -rlP '\xc3\xa2\xc2\x80\xc2\x94' gui/src || echo clean
./git_wrapper commit "Bar A: Find beside Settings, cells told apart by typeface and weight"
```

---

### Task 3: The EqSwitcher scene

A third `ContentOverlay`, built on the signal chain's pattern: `panels::drawHeader` + Close, rows clipped to the content rect, a draw-published viewport for the wheel, and the scrollbar. Also fixes a pre-existing bug the scene would otherwise inherit: **bar A navigation never closes a scene** — a filter tap while the signal chain is open changes the grid underneath while the scene stays drawn over it.

**Files:**
- Modify: `gui/src/player_view.hh`, `gui/src/player_view.cc`, `tools/ui_capture/main.cc`

**Interfaces:**
- Consumes: `BarAItem::EqBox` / `kSidebarEqBoxHit` (Task 2), `selectEqProfile(const EqAssignment&)`, `clearEqProfile()`, `onEqSettings()`, `eqHeadphones_` (`std::vector<EqHeadphone>`, fields `name`, `source`, `form`), `eqCurrent_` (`EqAssignment`), `eqCurrentTentative_`, `panels::drawHeader(Canvas&, const LayoutRect&, const std::string&, float scale, float headerTextSize, LayoutRect& closeRc)`, `panels::drawScrollbar`, `panels::drawButton`, `scrollTo(int, int, int, int)`, `clampScroll`, `SP_XL` (65, the album view's track row).
- Produces: `ContentOverlay::EqSwitcher`; `void drawEqSwitcher(Canvas&, const LayoutRect& area);`; `BarAModel::eqSwitcherOpen` set true while the scene is up.

- [ ] **Step 1: Declare the scene** — `gui/src/player_view.hh`

Change `enum class ContentOverlay { None, AlbumArt, SignalChain };` to `enum class ContentOverlay { None, AlbumArt, SignalChain, EqSwitcher };` and append to the comment above it:

```cpp
    // EqSwitcher joined them for the same reason: choosing which headphone
    // profile is on is something done WHILE the music plays, with the
    // transport under the listener's hands -- a panel would swallow the very
    // Space bar that stops it.
```

After `bool hoverScClose_ = false;` add:

```cpp
    // ── The EqSwitcher scene ────────────────────────────────────────────────
    // The saved AutoEQ profiles as full-width rows of FULL names. It replaced a
    // list that unfurled along bar A, where each profile got a segment the size
    // of a filter letter and a name drew as "S..." -- sideways, in the
    // horizontal layout. Rows are rebuilt by the draw and read by the hit-test,
    // the contract rcScClose_ already has.
    void drawEqSwitcher(Canvas& canvas, const LayoutRect& area);
    struct EsEntry {
        enum Kind { NoEq, Trial, Saved, All } kind;
        int idx;                      // into eqHeadphones_ for Saved, else -1
    };
    std::vector<EsEntry>    esEntries_;
    std::vector<LayoutRect> esRows_;  // parallel to esEntries_
    LayoutRect rcEsContent_{};        // rows hit-test only inside this
    LayoutRect rcEsClose_{};
    bool hoverEsClose_ = false;
    int  esHoverRow_   = -1;
    int  esScrollY_    = 0;
    int  esContentH_   = 0;           // measured by the draw
    int  esViewH_      = 0;           // published by the draw, like scViewH_
```

- [ ] **Step 2: Draw it** — `gui/src/player_view.cc`, next to `drawSignalChain`

```cpp
// ── The EqSwitcher scene ─────────────────────────────────────────────────────
// No EQ first, then the profile on trial (picked, not yet credited its sixty
// seconds -- see creditEqHeadphone), then the saved rows in their stored order
// (pinned, most used, most recent), then the route to the whole catalogue.
// The active row wears the accent tint: accent is state, never hover.
void PlayerWindow::drawEqSwitcher(Canvas& canvas, const LayoutRect& area) {
    const LayoutRect content = panels::drawHeader(canvas, area, "AutoEQ", metrics_.scale,
                                                  metrics_.text.header, rcEsClose_);
    rcEsContent_ = content;
    const Rect c = toRect(content);
    canvas.setClip(c.x, c.y, c.w, c.h);

    const float pad   = metrics_.space(SP_LG);
    const float inset = metrics_.space(SP_MD);
    const int   rowH  = (int)metrics_.space(SP_XL);
    const float sz    = metrics_.text.body;

    esEntries_.clear();
    esRows_.clear();
    const bool trialLeads = eqCurrentTentative_ && !eqCurrent_.name.empty();
    esEntries_.push_back({ EsEntry::NoEq, -1 });
    if (trialLeads) esEntries_.push_back({ EsEntry::Trial, -1 });
    for (int i = 0; i < (int)eqHeadphones_.size(); i++)
        esEntries_.push_back({ EsEntry::Saved, i });
    esEntries_.push_back({ EsEntry::All, -1 });

    int y = (int)(c.y + pad) - esScrollY_;
    for (size_t i = 0; i < esEntries_.size(); i++) {
        const EsEntry& e = esEntries_[i];
        const LayoutRect rc = { (int)(c.x + pad), y, (int)(c.x + c.w - pad), y + rowH };
        esRows_.push_back(rc);
        y += rowH;

        std::string label;
        bool active = false;
        ColorRef clr = CLR_TEXT_PRIMARY;
        FontStyle style = FontStyle::Roman;
        switch (e.kind) {
        case EsEntry::NoEq:
            label  = "No EQ";
            active = eqCurrent_.name.empty();
            break;
        case EsEntry::Trial:
            label  = eqCurrent_.name;
            active = true;
            break;
        case EsEntry::Saved: {
            const EqHeadphone& h = eqHeadphones_[e.idx];
            label = h.name;
            // name + source, not the full triple: the form is the one field a
            // regenerated catalogue can change (see findByKey's fallback).
            active = !trialLeads && h.name == eqCurrent_.name && h.source == eqCurrent_.source;
            break;
        }
        case EsEntry::All:
            label = "All profiles\xE2\x80\xA6";
            clr   = CLR_TEXT_DIM;
            style = FontStyle::Italic;
            break;
        }

        const Rect r = toRect(rc);
        if (active)
            canvas.rect(r.x, r.y, r.w, r.h, toColor(CLR_ACCENT, UI_SELECT_TINT_ALPHA),
                        UI_CORNER_RADIUS);
        else if ((int)i == esHoverRow_)
            canvas.rect(r.x, r.y, r.w, r.h, toColor(CLR_HOVER), UI_CORNER_RADIUS);
        canvas.textStyled(truncateToWidth(canvas, label, r.w - inset * 2, sz, style),
                          r.x + inset, r.y + r.h * 0.5f - sz * 0.5f, sz,
                          toColor(active ? CLR_ACCENT : clr), style);
    }
    canvas.clearClip();

    esContentH_ = (int)(y + esScrollY_ - c.y + pad);
    esViewH_    = content.bottom - content.top;
    const int capped = (int)clampScroll((float)esScrollY_, (float)esContentH_, (float)esViewH_);
    if (capped != esScrollY_) { esScrollY_ = capped; markDirty(); }
    panels::drawScrollbar(canvas, content, esContentH_, esScrollY_, metrics_.scale);
    panels::drawButton(canvas, rcEsClose_, "Close", hoverEsClose_, metrics_.text.body);
}
```

(`truncateToWidth` takes a `FontStyle` — `truncateToWidth(Canvas&, const std::string&, float maxW, float size, FontStyle)`, `framework/vk_canvas/core/text_util.hh:17`.)

- [ ] **Step 3: Wire draw, click, hover, wheel, close, and the box**

In `drawFrame()`, the scene dispatch (`if (overlay_ == ContentOverlay::SignalChain) drawSignalChain(canvas, rcGrid_);`) becomes:

```cpp
        if (overlay_ == ContentOverlay::SignalChain)
            drawSignalChain(canvas, rcGrid_);
        else if (overlay_ == ContentOverlay::EqSwitcher)
            drawEqSwitcher(canvas, rcGrid_);
```

In `handleClick()`, directly after the signal chain's scene branch (`if (!settingsOpen_ && overlay_ == ContentOverlay::SignalChain && ptInRect(rcGrid_, x, y)) { ... }`), add:

```cpp
    if (!settingsOpen_ && overlay_ == ContentOverlay::EqSwitcher && ptInRect(rcGrid_, x, y)) {
        if (ptInRect(rcEsClose_, x, y)) { closeOverlay(); return; }
        if (!ptInRect(rcEsContent_, x, y)) return;      // a row scrolled under the header
        for (size_t i = 0; i < esRows_.size() && i < esEntries_.size(); i++) {
            if (!ptInRect(esRows_[i], x, y)) continue;
            const EsEntry e = esEntries_[i];
            closeOverlay();                             // a pick closes it, like any menu
            switch (e.kind) {
            case EsEntry::NoEq:  clearEqProfile(); break;
            case EsEntry::Trial: break;                 // already applied; re-assigning
                                                        // would restart its minute
            case EsEntry::Saved:
                if (e.idx < (int)eqHeadphones_.size()) {
                    const EqHeadphone& h = eqHeadphones_[e.idx];
                    selectEqProfile({ h.name, h.source, h.form });
                }
                break;
            case EsEntry::All:
                // The catalogue lives in the EQ Settings panel. It only draws
                // under settingsOpen_, and closeActivePanel() hands the view back
                // because of panelFromSidebar_ -- the old "Search more..." route.
                onEqSettings();
                settingsOpen_     = true;
                panelFromSidebar_ = true;
                break;
            }
            return;
        }
        return;
    }
```

In `onMouseMove()`, after the signal chain's hover block, add:

```cpp
    if (!settingsOpen_ && overlay_ == ContentOverlay::EqSwitcher) {
        const bool hc = ptInRect(rcEsClose_, x, y) != 0;
        int hr = -1;
        if (ptInRect(rcEsContent_, x, y))
            for (size_t i = 0; i < esRows_.size(); i++)
                if (ptInRect(esRows_[i], x, y)) { hr = (int)i; break; }
        if (hc != hoverEsClose_ || hr != esHoverRow_) {
            hoverEsClose_ = hc;
            esHoverRow_   = hr;
            invalidate();
        }
    }
```

In `onMouseWheel()`'s scene branch, after the `SignalChain` arm:

```cpp
        else if (overlay_ == ContentOverlay::EqSwitcher) {
            esScrollY_ = scrollTo(esScrollY_, delta, esContentH_, esViewH_);
            invalidate();
        }
```

(i.e. turn the existing `if (overlay_ == ContentOverlay::SignalChain) { ... }` into an `if … else if …` pair inside the same block.)

In `closeOverlay()`, after `hoverScClose_ = false;` add `hoverEsClose_ = false; esHoverRow_ = -1;`.

In `barAModel()`, after `m.settingsActive = settingsOpen_;` add `m.eqSwitcherOpen = (!settingsOpen_ && overlay_ == ContentOverlay::EqSwitcher);`.

In `handleClick()`'s sidebar branch, replace Task 2's temporary EQ box branch with the toggle:

```cpp
        if (nav == kSidebarEqBoxHit) {
            // The box is the handle: touching it opens the switcher, touching it
            // again puts it away without changing anything.
            if (overlay_ == ContentOverlay::EqSwitcher) {
                closeOverlay();
            } else {
                settingsOpen_ = false;          // a scene only draws outside Settings
                overlay_      = ContentOverlay::EqSwitcher;
                esScrollY_    = 0;
                esHoverRow_   = -1;
                applyCursor();
                invalidate();
            }
            return;
        } else if (nav == kSidebarSearchHit) {
```

- [ ] **Step 4: Bar A navigation closes whatever scene is open**

In the same sidebar branch add `closeOverlay();` as the first statement inside each of: the `kSidebarPlaylistsHit` branch, the `kSidebarSettingsHit` branch, and the `nav >= 0` (filter letter) branch — before their own logic. `closeOverlay()` is a no-op when nothing is open. Comment once, above the Playlists branch:

```cpp
        // Moving anywhere on bar A leaves any open scene. Without this a filter
        // tap under the signal chain changed the grid while the scene stayed
        // drawn over it -- the change was real and invisible.
```

- [ ] **Step 5: Capture state** — `captureGoTo()` and `tools/ui_capture/main.cc`

In `captureGoTo()`'s `reset` lambda add `overlay_ = ContentOverlay::None;`. Before the final `return false;` add:

```cpp
    if (state == "45-eq-switcher") {
        // Reached the way a listener reaches it: by touching the box. Search
        // comes earlier in the state list and hides the box, so put it back.
        closeSearch();
        drawFrame();
        if (rail_.eqBox.right <= rail_.eqBox.left) return false;   // bit-perfect
        click(rail_.eqBox);
        return overlay_ == ContentOverlay::EqSwitcher;
    }
```

In `kStates[]` add `"45-eq-switcher",` after `"44-transport-ordinal",`.

- [ ] **Step 6: Build, test, capture, look**

Run: `scripts/linux/build.sh --debug`, all 13 tests, then
`matrix_ui_capture --fixture 60 --frame 720x1640 --only 45-eq --out <dir>` and the same at `1640x720`.
Expected: `45-eq-switcher` written (not SKIPPED); the image shows the "AutoEQ" header, a full-width `No EQ` row in the accent tint (fresh DB has no profile), and `All profiles…` in dim italic, bar A's box tinted as selected. Look at both images.

- [ ] **Step 7: Commit**

```bash
grep -rlP '\xc3\xa2\xc2\x80\xc2\x94' gui/src tools/ui_capture || echo clean
./git_wrapper commit "EqSwitcher: saved AutoEQ profiles as a readable full-page list"
```

---

### Task 4: An empty search closes when you tap elsewhere

**Trap (found in exploration):** the focus check in `handleClick()` (`:4167-4185`) runs BEFORE bar A is hit-tested (`:4266`). If `closeSearch()` runs there, `recalcLayout()` has already brought the letters back and the chip strip has gone, so the SAME tap would land on whatever now sits under the finger. The tap is therefore spent on closing — except in the two places that do not move: the transport (bar B) and Settings (pinned at the near end in every state).

**Files:**
- Modify: `gui/src/player_view.cc`, `tools/ui_capture/main.cc`

**Interfaces:**
- Consumes: `closeSearch()`, `searchOpen_`, `searchFocused_`, `searchQuery_` (`std::string`), `searchChips_` (`std::vector<facets::Chip>`), `rcSearch_`, `rcBarB_`, `rcNavSettings_`.

- [ ] **Step 1: A capture state that fails today** — `captureGoTo()`, before the final `return false;`

```cpp
    if (state == "46-search-empty-closes") {
        // A self-check through the REAL input path: open search, type nothing,
        // tap the grid. Reported FAILED while an empty search survives the tap.
        closeSearch();
        click(rcSearch_);                               // opens it
        if (!searchOpen_) return false;
        click(rcGrid_);                                 // a tap anywhere else
        return !searchOpen_;
    }
```

and `"46-search-empty-closes",` after `"45-eq-switcher",` in `kStates[]`.

Run: `scripts/linux/build.sh --debug && ./build/linux_debug/gui/matrix_ui_capture --only 46 --out <dir>`
Expected: `46-search-empty-closes SKIPPED (state unreachable…)` — the search stayed open.

- [ ] **Step 2: Implement** — replace the focus block in `handleClick()`

From `// Search box focus: clicking it starts typing; clicking anywhere else` through `if (searchFocused_) return;` and its closing `}`:

```cpp
    // Search box focus: clicking it starts typing; clicking anywhere else
    // releases focus (the query itself stays, still filtering).
    {
        const bool wasFocused = searchFocused_;
        // Only while the field EXISTS. When search is closed that same rect is
        // the Find cell that opens it.
        searchFocused_ = searchOpen_ && ptInRect(rcSearch_, x, y) != 0;

        // An EMPTY search -- no text, no chips -- closes when the tap lands
        // anywhere else. Opening search by mistake used to cost a trip to the
        // close cell at the far end; now it costs nothing. A search holding
        // anything stays open: a stray tap never discards what was typed.
        if (searchOpen_ && !searchFocused_ && searchQuery_.empty() && searchChips_.empty()) {
            closeSearch();
            // closeSearch() re-laid out bar A (the letters are back) and the
            // grid (the chip strip is gone) under the finger, so this tap is
            // spent on closing -- except where nothing moved: the transport,
            // and Settings, pinned at the near end in every state.
            if (!ptInRect(rcBarB_, x, y) && !ptInRect(rcNavSettings_, x, y)) return;
        } else if (searchFocused_ != wasFocused) {
            syncKeyboard();   // a phone has no keyboard until this box asks
            // Focus decides whether the suggestion row exists at all, so the
            // list is rebuilt on both edges; recalcLayout() follows because
            // that row occupies real space.
            refreshSuggestions();
            recalcLayout();
            invalidate();
        }
        if (searchFocused_) return;
    }
```

- [ ] **Step 3: Verify**

Run: `scripts/linux/build.sh --debug && ./build/linux_debug/gui/matrix_ui_capture --only 46 --out <dir>`
Expected: `46-search-empty-closes` written. Then the full capture set: `40-search`, `41-search-suggest`, `42-search-chips` still written (they type before tapping elsewhere, so their searches are not empty).

- [ ] **Step 4: Commit**

```bash
./git_wrapper commit "An empty search closes when you tap anywhere else"
```

---

### Task 5: Documentation, and the whole thing verified at phone size

**Files:**
- Modify: `CLAUDE.md`, `docs/UI_DESIGN_SYSTEM.md`, `gui/src/player_view.hh` (stale comments), copy this plan to `docs/superpowers/plans/2026-09-11-bar-a-typographic-rail.md`

- [ ] **Step 1: CLAUDE.md**
  - "The frame" rule 3 ("**Three cells read `S`** … told apart by the text ladder"): rewrite — Settings (Terminus), Find (`F`, regular), filters (bold); the ladder now confirms rather than separates; Find sits beside Settings, a gap from Albums, because search is the least-used cell and used to be one tap away from the most-used one.
  - "The frame" rule 6 and "Driver AutoEQ profiles" rules 5-6 (the box that unfurls, the list that hides the letters): replace with the EqSwitcher scene — full-width rows, No EQ / trial / saved / All profiles…, tap applies and closes, the box toggles it, Back closes it, bar A navigation closes any scene.
  - "Full-page scenes": the table and "Four things here will bite…" gain `EqSwitcher`; note that bar A navigation now closes scenes.
  - Test list: `terminus_glyph_test` (Task 1 already added it) — confirm "thirteen".
  - A short paragraph on Terminus: bitmap, baked by `tools/terminus/bake_glyphs.py`, committed header, integer scale only, OFL.
- [ ] **Step 2: `docs/UI_DESIGN_SYSTEM.md`** — the bar A / rail section and §8.2's neighbour: the typography table (face, weight, colour per cell), Find beside Settings, the EQ box as a name that opens a scene, the EqSwitcher as a new §8.x (header, rows at `SP_XL`, accent tint for the active row, dim italic `All profiles…`, scrollbar).
- [ ] **Step 3: Stale comments** — `player_view.hh` near the removed EQ members and the capture code that said the box's rects are "computed during draw"; grep `"unfurl"` in `gui/src` and fix every hit.
- [ ] **Step 4: Full verification**
  - `scripts/linux/build.sh --debug` and `--release`; all 13 test binaries.
  - `matrix_ui_capture --out <dir>` (default): every state written except the known pre-existing six; bar A changed in all of them, so compare BY EYE, not by bytes — check `10-grid-albums`, `30-settings`, `40-search`, `45-eq-switcher`, `46-search-empty-closes`.
  - `--fixture 60` at `--frame 720x1640` and `--frame 1640x720` for `10-grid`, `40-search`, `45-eq`: Settings is a crisp pixel `S` (no grey edge pixels — zoom in), `F` regular, letters bold, box shows the name, switcher rows full names.
  - `cd android && VULKAN_SDK=/opt/shader-slang sh gradlew assembleRelease`; `nm -C` the arm64 `.so` for `drawTerminusGlyph` and `PlayerWindow::drawEqSwitcher`.
- [ ] **Step 5: Commit**

```bash
grep -rlP '\xc3\xa2\xc2\x80\xc2\x94' gui/src docs CLAUDE.md || echo clean
./git_wrapper commit "Docs: bar A by typeface, the EqSwitcher scene, Terminus"
```

---

## Self-review (against the spec)

| Spec requirement | Task |
|---|---|
| Rare cells (Settings, Find) together at the near end; gap before letters | 2 (layout + test "Find is never next to a filter letter") |
| Find is the letter `F` | 2 (`bar_a.cc`) |
| Cell count unchanged (2 + 7) | 2 (clamp `kRailLetterCount + 2`; narrow-bar test) |
| Empty search closes on tap elsewhere; non-empty stays | 4 (+ capture self-check 46) |
| EQ box shows the name only; × removed; bit-perfect hides it | 2 |
| Settings Terminus / Find regular / filters bold; ladder unchanged | 1 + 2 |
| Terminus baked, committed header, integer scale, OFL copied | 1 |
| EqSwitcher: full-width full-name rows, order, active tint, trial, apply-and-close, All profiles…, Back/box closes, bars keep working | 3 |
| Removals (eqListOpen, eqList, railListRow, EqRow/EqNone, ×, eqNone) | 2 (grep step) |
| rail_layout_test updated, rotation kept; terminus_glyph_test; capture 45; 43 retired | 1, 2, 3 |
| Docs | 5 |

Types used across tasks: `terminus::Glyph/Pick/Run`, `drawTerminusGlyph` (1→2); `RailLayout{letters,search,settings,close,eqBox}`, `BarAItem::EqBox`, `kSidebarEqBoxHit` (2→3); `ContentOverlay::EqSwitcher`, `EsEntry`, `esRows_` (3). Names are consistent between tasks.

## Execution

Inline, in this session (the listener is present and wants the work finished), task by task with the full verification loop between tasks. Each task is one commit through `./git_wrapper commit`. Not pushed — `./git_wrapper push` is the listener's call.

# Settings-panel chrome in Terminus Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Draw Settings' fixed UI vocabulary (headers, button labels, section captions, toggle words, short status lines) in Terminus, ALL CAPS, across all five Settings panels — while runtime data (folder paths, device names, EQ profile names, literal shell commands) stays in the existing serif face, unconditionally.

**Architecture:** Extend the existing single-glyph Terminus module (`terminus_glyph.hh/.cc`) with a running-text layer (`resolveText`/`terminusTextWidth`/`drawTerminusText`) that falls back to serif on the first unrepresentable character. Bake the full printable ASCII range once (not one letter at a time). Give the two shared panel widgets (`panels::drawHeader`, `panels::drawButton`) an opt-in flag so only Settings converts. Convert every direct `canvas.textStyled` call inside the five panel-drawing functions that carries a **compile-time literal string** — never one built by concatenating a literal with runtime data, even when that data happens to be ASCII.

**Tech Stack:** C++17, vk_canvas `Canvas`, CMake/Ninja, Python 3 + Pillow (bake tool only), plain-`assert` Debug tests.

**Spec:** `docs/superpowers/specs/2026-09-11-settings-terminus-chrome-design.md`.

## A refinement the spec's rule needed, found during planning

The spec states one mechanical rule: a string draws in Terminus if every
character is representable, and falls back to serif automatically otherwise.
That rule is sufficient to protect **non-ASCII** data (an accented folder
name, a Cyrillic device string) automatically. It is **not** sufficient to
protect **ASCII** data — a USB device name like `Hiby FC4`, a literal `adb`
command embedding a MAC address, or a sentence built as `"Now: " + codecName`
would happily pass the all-ASCII test and get shouted in blocky caps, which is
wrong: a device's actual name must never be re-cased, and a command the
listener is meant to copy-paste must never be reformatted.

So the actual rule this plan applies has two parts:

1. A call site whose string argument is **entirely a compile-time literal**
   (a header title, a button label, a status sentence, one of a small closed
   set of literals selected by an index) is wrapped — Terminus if every
   character resolves, serif automatically otherwise (the spec's rule, doing
   real work here: several of these literals use an em dash and correctly
   fall back).
2. A call site whose string is built by **concatenating a literal with a
   runtime value** (a device name, a database key, a profile name, an
   assembled notice) — or is **entirely** runtime data — is left as a plain,
   unwrapped `canvas.textStyled` call, unconditionally serif, regardless of
   whether that particular value happens to be ASCII today.

Every task below states which rule applies at each call site it touches.

## Global Constraints

- Commit ONLY with `./git_wrapper commit "<msg>"`; bare message, no `Co-Authored-By`/`Claude-Session` trailers (workspace CLAUDE.md overrides).
- No change to any submodule (`framework/*`). `widgets::drawFitButton`, `widgets::drawRadioRow`, `widgets::drawToggle`, and `widgets::drawScrollList` all live in `framework/vk_canvas` and are **read but never edited** — see Task 2 and Task 7's "explicitly not done" list for what stays serif because of this.
- Scripted edits that insert non-ASCII into C++: use real characters or raw strings — never `\xE2\x80\x94` inside a normal Python string (memory: heredoc-utf8-double-encoding). Grep for `\xc3\xa2\xc2\x80\xc2\x94` before every commit.
- Terminus: integer scale only, nearest sampling; the existing two strikes (`ter-u16b.otb`, `ter-u32b.otb`) are reused — no new strike in this plan.
- Test convention: Debug-only executables, `#undef NDEBUG`, plain `assert`, no engine linked for the pure half.
- Verification loop per task: `scripts/linux/build.sh --debug` → all test binaries → captures as stated in the task.
- The bake script (`tools/terminus/bake_glyphs.py`) is verified empirically in this plan (Task 1, Step 1) rather than assumed — the full candidate character set was test-baked during planning and produced zero errors at both strikes.

## File map

| File | Responsibility | Task |
|---|---|---|
| `tools/terminus/bake_glyphs.py` | `GLYPHS` becomes the full printable-ASCII-minus-lowercase set; the "empty glyph is an error" check gets a space exception | 1 |
| `gui/src/terminus_glyphs.gen.h` | Regenerated: 69 glyphs × 2 strikes | 1 |
| `gui/src/terminus_glyph.hh` | Declares `terminus::resolveText`, `terminusTextWidth`, `drawTerminusText` | 1 |
| `gui/src/terminus_glyph.cc` | Implements `resolveText` (pure) and `terminusTextWidth` (pure) | 1 |
| `gui/src/terminus_glyph_draw.cc` | Implements `drawTerminusText` (Canvas-dependent) | 1 |
| `gui/src/terminus_glyph_test.cc` | Asserts every real chrome string round-trips with zero fallback, plus fallback examples | 1 |
| `gui/src/panels/settings_panels.hh/.cc` | `drawHeader`/`drawButton` gain `bool terminusChrome = false` | 2 |
| `gui/src/player_view.cc` — `drawActivePanel` | Shared Close button passes `terminusChrome = true` | 2 |
| `gui/src/player_view.cc` — `drawManageFolders`, `drawFolderPicker` | Convert (Task 3) | 3 |
| `gui/src/player_view.cc` — `drawInterfaceSettings` | Convert (Task 4) | 4 |
| `gui/src/player_view.cc` — `drawEqSettings` | Convert (Task 5) | 5 |
| `gui/src/player_view.cc` — `drawAudioSettings`, `drawBluetoothCodecSection` | Convert (Task 6) | 6 |
| `docs/UI_DESIGN_SYSTEM.md`, `CLAUDE.md` | Document the running-text Terminus layer and the two-part rule | 7 |

---

### Task 1: Terminus running text — bake the full set, resolve/measure/draw a string

**Files:**
- Modify: `tools/terminus/bake_glyphs.py`, `gui/src/terminus_glyphs.gen.h` (generated), `gui/src/terminus_glyph.hh`, `gui/src/terminus_glyph.cc`, `gui/src/terminus_glyph_draw.cc`, `gui/src/terminus_glyph_test.cc`

**Interfaces:**
- Consumes: existing `terminus::Glyph`, `terminus::Pick`, `terminus::pickStrike`, `terminus::Run`, `terminus::runs` — unchanged.
- Produces:
  - `bool terminus::resolveText(const std::string&, float targetPx, std::vector<const terminus::Glyph*>& out, int& outScale);` — pure.
  - `float terminusTextWidth(const std::string&, float targetPx);` — pure; returns `-1.0f` (never a real width) when unrepresentable, distinct from an empty string's `0.0f`.
  - `bool drawTerminusText(Canvas&, const std::string&, float x, float y, float targetPx, const Color&);` — same `(x, y)` = top-left convention as `Canvas::text`/`textStyled`; draws nothing and returns `false` on the first unresolvable character.

- [ ] **Step 1: Update the bake script**

In `tools/terminus/bake_glyphs.py`, change:

```python
GLYPHS = "S"            # Settings. Add characters here, rerun, commit both.
```

to:

```python
# The full printable-ASCII set MINUS lowercase (0x20 space through 0x7E '~',
# skipping 'a'-'z'): drawTerminusText folds ASCII lowercase to its capital's
# glyph at lookup time, so baking lowercase too would double the table for
# entries nothing ever looks up. Baking the WHOLE set up front, rather than
# one letter at a time the way `S`/`F` were, means a future Settings copy
# change never needs a re-bake -- only a genuinely new (non-ASCII) character
# would. Verified empirically during planning: all 69 chars bake cleanly at
# both strikes, no grey pixels, no missing glyphs (see the design spec).
GLYPHS = "".join(chr(c) for c in range(0x20, 0x7F) if not (0x61 <= c <= 0x7A))
```

And in `bake()`, change:

```python
    if not any(rows):
        sys.exit(f"{path}: {ch!r} baked empty")
```

to:

```python
    if not any(rows) and ch != " ":
        sys.exit(f"{path}: {ch!r} baked empty")
```

(Space is the one legitimately all-zero-row glyph — its only job is to occupy `cellW` pixels of advance. Verified during planning: Pillow returns a proper non-degenerate `(cellW, size)` mask for `" "` in both Terminus strikes, so it never hits the earlier "no glyph" check.)

- [ ] **Step 2: Run the bake, verify the count**

Run: `python3 tools/terminus/bake_glyphs.py`
Expected: `wrote ../../gui/src/terminus_glyphs.gen.h  (69 glyph(s) x 2 strikes)`

- [ ] **Step 3: Extend the header** — `gui/src/terminus_glyph.hh`

Add, inside `namespace terminus { ... }`, right after the existing `void runs(...)` declaration:

```cpp
// Resolves every character of `text` against the strike `targetPx` picks,
// after folding ASCII lowercase to its capital (drawTerminusText's whole
// reason for existing is that callers keep writing normal mixed-case string
// literals). Returns false on the FIRST unresolvable character -- any byte
// >= 0x80 (a UTF-8 lead or continuation byte) is never baked, which is what
// makes any non-ASCII string fall back automatically; `out`/`outScale` are
// left in an unspecified partial state on failure and must not be used.
bool resolveText(const std::string& text, float targetPx,
                 std::vector<const Glyph*>& out, int& outScale);
```

Then, after the closing `} // namespace terminus` and before `bool drawTerminusGlyph(...)`, add:

```cpp
// The advance width `text` would occupy at drawTerminusText's chosen strike
// and scale -- for centering/right-alignment, mirroring Canvas::textWidth().
// Returns -1.0f, never a real width, when the string cannot be represented
// (distinct from a legitimately empty string's width of 0). Pure -- no
// Canvas -- so terminus_glyph_test can assert on real chrome strings without
// linking the renderer.
float terminusTextWidth(const std::string& text, float targetPx);
```

And after `bool drawTerminusGlyph(...)`'s declaration, add:

```cpp
// Draws `text` left-aligned from (x, y) -- the same (x, y) = top-left
// convention Canvas::text()/textStyled() already use. The chosen strike's
// actual cell height is rarely exactly targetPx (16 or 32 times an integer
// scale), so the vertical placement is offset internally to keep a caller's
// EXISTING centering math (built around targetPx) correct without change --
// passing the same (x, y, targetPx) as an equivalent canvas.text() call lines
// the two up. Draws nothing and returns false if any character is
// unresolvable -- never half a string.
bool drawTerminusText(Canvas& c, const std::string& text, float x, float y,
                      float targetPx, const Color& col);
```

- [ ] **Step 4: Write the failing test** — append to `gui/src/terminus_glyph_test.cc`, before `printf(...)`:

```cpp
    // ── Running text: resolve, measure, and the two-part fallback rule ─────
    //
    // ASCII (any case) round-trips; a byte outside 0x20-0x7E (here: the "é" in
    // a synthetic folder name, and a real em-dash sentence lifted verbatim
    // from drawAudioSettings) does not, and terminusTextWidth reports that as
    // -1.0f rather than 0 -- 0 is a legitimately empty string's answer.
    assert(terminusTextWidth("", 20.0f) == 0.0f);
    assert(terminusTextWidth("Done", 20.0f) > 0.0f);
    assert(terminusTextWidth("done", 20.0f) == terminusTextWidth("DONE", 20.0f));  // case-folded
    assert(terminusTextWidth("café", 20.0f) < 0.0f);
    assert(terminusTextWidth("SBC is a lossy encode \xE2\x80\x94 this route can never be bit-perfect.",
                             20.0f) < 0.0f);   // a real em-dash sentence from drawAudioSettings

    // Every literal chrome string actually wrapped in Settings (Tasks 3-6)
    // must round-trip with ZERO fallback -- the same "a missing glyph is a
    // build-time failure" discipline the Settings S already has, extended to
    // whole strings. If a future edit adds a character outside the baked
    // ASCII set to one of these, this line is what catches it.
    static const char* kChromeStrings[] = {
        "Music Folders", "No music folders added yet.", "Remove Selected", "Done",
        "Close",
        "Audio Output Settings", "Output backend:", "USB DAC:",
        "No USB audio devices found.", "Device:", "Mode:", "Starting port:",
        "No running JACK server found (or no physical playback ports).",
        "Headphones:", "No paired A2DP device. Pair and connect a pair of headphones first.",
        "Android chooses the output route itself, and follows it when you",
        "16-bit output. Not a bit-perfect path for deeper sources.",
        "Release the device in your sound server first; only one app can stream to it.",
        "Playback goes through the AOAS service, which owns the USB",
        "and silent across app switches. AOAS must be installed and",
        "signed with the same key as this app. There is no device to pick here.",
        "Apply",
        "Bluetooth codec", "No Bluetooth headphones connected.",
        "This phone has not granted codec control. Two ways to get it:",
        "Pair as companion device", "...or, from a computer, once:", "Forget",
        "Apply saves this against these headphones and re-applies it whenever they reconnect.",
        "EQ / AutoEQ Profiles", "My Drivers", "All Profiles", "Best only", "Every source",
        "No saved drivers match.", "No profiles match.",
        "Select", "Pin", "Unpin", "Remove", "Assign to Device", "Clear",
        "Select Music Folder", "No subfolders here.", "Cancel", "Select This Folder",
        "Interface", "Scrolling", "Flicking a list throws it, and it slows to a stop on its own.",
    };
    for (const char* s : kChromeStrings)
        assert(terminusTextWidth(s, 20.0f) >= 0.0f);
```

- [ ] **Step 5: Build, see it fail**

Run: `scripts/linux/build.sh --debug`
Expected: FAIL — `terminusTextWidth`/`resolveText` undeclared.

- [ ] **Step 6: Implement** — `gui/src/terminus_glyph.cc`, inside `namespace terminus { ... }`, after `runs(...)`:

```cpp
bool resolveText(const std::string& text, float targetPx,
                 std::vector<const Glyph*>& out, int& outScale) {
    out.clear();
    out.reserve(text.size());
    outScale = 1;
    for (unsigned char ch : text) {
        const char32_t up = (ch >= 'a' && ch <= 'z') ? (char32_t)(ch - 'a' + 'A')
                                                     : (char32_t)ch;
        const Pick p = pickStrike(up, targetPx);
        if (!p.glyph) return false;
        out.push_back(p.glyph);
        outScale = p.scale;   // identical for every glyph at this targetPx
    }
    return true;
}
```

Then, at file scope, after the closing `} // namespace terminus`:

```cpp
float terminusTextWidth(const std::string& text, float targetPx) {
    if (text.empty()) return 0.0f;
    std::vector<const terminus::Glyph*> glyphs;
    int scale = 1;
    if (!terminus::resolveText(text, targetPx, glyphs, scale)) return -1.0f;
    float total = 0.0f;
    for (const terminus::Glyph* g : glyphs) total += (float)(g->cellW * scale);
    return total;
}
```

- [ ] **Step 7: Implement the draw half** — `gui/src/terminus_glyph_draw.cc`, append:

```cpp
bool drawTerminusText(Canvas& c, const std::string& text, float x, float y,
                      float targetPx, const Color& col) {
    std::vector<const terminus::Glyph*> glyphs;
    int scale = 1;
    if (!terminus::resolveText(text, targetPx, glyphs, scale)) return false;
    if (glyphs.empty()) return true;   // nothing to draw; not a failure
    const float cellH = (float)(glyphs[0]->cellH * scale);
    const float oy = std::floor(y + (targetPx - cellH) * 0.5f);
    float pen = std::floor(x);
    std::vector<terminus::Run> rs;
    for (const terminus::Glyph* g : glyphs) {
        terminus::runs(*g, scale, rs);
        for (const terminus::Run& r : rs)
            c.rect(pen + (float)r.x, oy + (float)r.y, (float)r.w, (float)r.h, col);
        pen += (float)(g->cellW * scale);
    }
    return true;
}
```

- [ ] **Step 8: Build and run**

Run: `scripts/linux/build.sh --debug && ./build/linux_debug/gui/terminus_glyph_test`
Expected: `terminus_glyph_test: all assertions passed`. Run the full 13-test list; all pass. `ui_capture` byte-identical to HEAD (nothing draws with the new functions yet).

- [ ] **Step 9: Commit**

```bash
grep -rlP '\xc3\xa2\xc2\x80\xc2\x94' gui/src tools/terminus || echo clean
./git_wrapper commit "Terminus running text: the full printable ASCII set, resolve/measure/draw a string"
```

---

### Task 2: `drawHeader`/`drawButton` gain an opt-in Terminus mode

**Files:**
- Modify: `gui/src/panels/settings_panels.hh`, `gui/src/panels/settings_panels.cc`, `gui/src/player_view.cc` (`drawActivePanel` only)

**Interfaces:**
- Consumes (Task 1): `terminusTextWidth`, `drawTerminusText`.
- Produces: `panels::drawHeader(..., bool terminusChrome = false)`, `panels::drawButton(..., bool terminusChrome = false)` — both default `false`, so `EqSwitcher` and `drawSignalChain` (which also call these) are unaffected.

- [ ] **Step 1: Update the declarations** — `gui/src/panels/settings_panels.hh`

```cpp
// A small rectangular action button (Done/Cancel/Remove/Assign/Select),
// right-aligned text inside a border, matching the settings-page row style.
// terminusChrome: draw the label in Terminus (ALL CAPS, falling back to the
// normal fit-button label automatically if it isn't representable or
// doesn't fit at Terminus's own nominal size) instead of the proportional
// serif label. Default false so EqSwitcher/drawSignalChain, which also call
// this, are untouched -- only Settings passes true.
void drawButton(Canvas& canvas, const LayoutRect& rc, const std::string& label,
                 bool hover, float textSize, bool primary = false,
                 bool terminusChrome = false);

// Panel chrome: title bar + "Close" affordance. Returns the content area
// below the header (what the panel's own drawing should treat as its rect).
// closeRc receives the close button's hit-test rect (top-right corner).
// terminusChrome: draw the title in Terminus. Same default-false reasoning
// as drawButton above.
LayoutRect drawHeader(Canvas& canvas, const LayoutRect& area, const std::string& title,
                      float scale, float headerTextSize, LayoutRect& closeRc,
                      bool terminusChrome = false);
```

- [ ] **Step 2: Implement** — `gui/src/panels/settings_panels.cc`

Add `#include "terminus_glyph.hh"` after the existing includes. Replace `drawButton`'s body:

```cpp
void drawButton(Canvas& canvas, const LayoutRect& rc, const std::string& label,
                 bool hover, float textSize, bool primary, bool terminusChrome) {
    (void)textSize;   // drawFitButton sizes the label to the button proportionally
    Rect r = toRect(rc);
    float radius = UI_CORNER_RADIUS;   // uniform rounding — reads as a real button
    Color bg, fg;
    if (primary) {
        bg = hover ? lift(CLR_ACCENT, 28) : toColor(CLR_ACCENT);
        fg = toColor(CLR_BG_MAIN);
    } else {
        bg = lift(CLR_BG_MAIN, hover ? 56 : 34);
        fg = toColor(CLR_TEXT_PRIMARY);
    }

    if (terminusChrome) {
        // Terminus is monospace, so there is no shrink-to-fit curve to
        // replicate: either the label fits on one line at drawFitButton's
        // own nominal size (r.h * 0.34, "Canvas::button's label proportion")
        // or the ORIGINAL serif path — with its shrink and two-line logic —
        // runs unchanged below. Every label a Settings button actually uses
        // fits at this size (terminus_glyph_test enumerates them); this is
        // the escape hatch for the day one doesn't.
        const float s    = r.h * 0.34f;
        const float maxW = r.w - r.h * 0.35f;
        const float tw   = terminusTextWidth(label, s);
        if (tw >= 0.0f && tw <= maxW) {
            canvas.rect(r.x, r.y, r.w, r.h, bg, radius);
            drawTerminusText(canvas, label, r.x + (r.w - tw) * 0.5f,
                             r.y + (r.h - s) * 0.5f, s, fg);
            return;
        }
    }
    // Single line: shrink-then-ellipsis rather than wrapping a button label.
    widgets::drawFitButton(canvas, r, label, bg, fg, radius, widgets::kTextFit, false);
}
```

Replace `drawHeader`'s body:

```cpp
LayoutRect drawHeader(Canvas& canvas, const LayoutRect& area, const std::string& title,
                      float scale, float headerTextSize, LayoutRect& closeRc,
                      bool terminusChrome) {
    Rect a = toRect(area);
    canvas.rect(a.x, a.y, a.w, a.h, toColor(CLR_BG_MAIN));

    float headerH = 91.0f * scale;
    const float tx = a.x + 39.0f * scale, ty = a.y + headerH * 0.5f - headerTextSize * 0.5f;
    const Color primaryCol = toColor(CLR_TEXT_PRIMARY);
    if (!terminusChrome || !drawTerminusText(canvas, title, tx, ty, headerTextSize, primaryCol))
        canvas.textStyled(title, tx, ty, headerTextSize, primaryCol, FontStyle::Bold);
    canvas.rect(a.x, a.y + headerH, a.w, std::max(1.0f, std::round(scale)),
                toColor(CLR_SEPARATOR));

    float closeW = 147.0f * scale, closeH = 52.0f * scale;
    float closeMargin = 32.0f * scale;
    closeRc = { (int)(area.right - closeW - closeMargin), (int)(area.top + (headerH - closeH) * 0.5f),
                (int)(area.right - closeMargin),          (int)(area.top + (headerH + closeH) * 0.5f) };

    return { area.left, (int)(area.top + headerH), area.right, area.bottom };
}
```

- [ ] **Step 3: `drawActivePanel`'s shared Close button** — `gui/src/player_view.cc`

Find (near line 4810):

```cpp
    if (closeRc)
        panels::drawButton(canvas, *closeRc, "Close", hoverClose, metrics_.text.body);
```

Replace with:

```cpp
    // Covers Manage Folders, Audio Settings, EQ Settings and the folder
    // picker's Close in one place. "Close" is a fixed literal — Terminus,
    // no fallback expected.
    if (closeRc)
        panels::drawButton(canvas, *closeRc, "Close", hoverClose, metrics_.text.body,
                           false, true);
```

- [ ] **Step 4: Build and capture**

Run: `scripts/linux/build.sh --debug`, all 13 tests pass.
Run `matrix_ui_capture --out <dir>` and diff `30-settings`-family states against HEAD: the Close corner on Manage Folders/Audio Settings/EQ Settings/Folder Picker now reads in Terminus; every panel's TITLE is unchanged (headers still pass `false` — Task 3-6 flip that per panel). `EqSwitcher`/`45-eq-switcher` and `drawSignalChain`/`44-*` states must be **byte-identical** to HEAD — the whole point of the default-false parameter.

- [ ] **Step 5: Commit**

```bash
./git_wrapper commit "Settings widgets: drawHeader/drawButton can draw their label in Terminus"
```

---

### Task 3: Manage Folders and the Folder Picker

**Files:** Modify `gui/src/player_view.cc` — `drawManageFolders`, `drawFolderPicker`.

**Interfaces:** Consumes Task 1 (`drawTerminusText`) and Task 2 (`terminusChrome` params).

- [ ] **Step 1: `drawManageFolders`** — replace the whole function body (line 5140):

```cpp
void PlayerWindow::drawManageFolders(Canvas& canvas, const LayoutRect& area) {
    LayoutRect content = panels::drawHeader(canvas, area, "Music Folders", metrics_.scale,
                                            metrics_.text.header, mfCloseRc_, true);
    float pad = metrics_.space(SP_LG);
    float btnH = metrics_.space(58.0f);

    LayoutRect listArea = { content.left, (int)(content.top + pad),
                            content.right, (int)(content.bottom - (btnH + pad * 2)) };
    mfListArea_ = listArea;
    float mfRowH = panelRowH();
    // Folder PATHS are data and go through the (untouchable) submodule row
    // list unchanged -- see CLAUDE.md's bar_a.cc rule, same reasoning here.
    mfListRows_ = widgets::drawScrollList(canvas, toRect(listArea), mfRoots_,
                                          mfSelectedRow_, (float)mfScrollY_, mfRowH,
                                          mfHoverRow_, widgets::kTextFree, matrixListStyle());
    panels::drawScrollbar(canvas, listArea, (int)((float)mfRoots_.size() * mfRowH), mfScrollY_, metrics_.scale);
    if (mfRoots_.empty()) {
        Rect a = toRect(listArea);
        const float ex = a.x + metrics_.space(22.0f), ey = a.y + metrics_.space(22.0f);
        // A fixed literal: Terminus, falling back to the serif italic if it
        // ever isn't representable.
        if (!drawTerminusText(canvas, "No music folders added yet.", ex, ey,
                              metrics_.text.body, toColor(CLR_TEXT_DIM)))
            canvas.textStyled("No music folders added yet.", ex, ey,
                              metrics_.text.body, toColor(CLR_TEXT_DIM), FontStyle::Italic);
    }

    float btnW = metrics_.space(277.0f);
    int by = (int)(content.bottom - (btnH + pad));
    auto mfRects = panels::layoutEdgePair(
        content, pad, btnW, btnW,
        metrics_.space(panels::kMinActionBtnW), metrics_.space(SP_MD), by, (int)btnH);
    mfBtnRemove_ = mfRects.first;
    mfBtnDone_   = mfRects.second;
    panels::drawButton(canvas, mfBtnRemove_, "Remove Selected", mfHoverRemove_,
                       metrics_.text.body, false, true);
    panels::drawButton(canvas, mfBtnDone_, "Done", mfHoverDone_, metrics_.text.body, true, true);
}
```

- [ ] **Step 2: `drawFolderPicker`** — replace the whole function body (line 6794):

```cpp
void PlayerWindow::drawFolderPicker(Canvas& canvas, const LayoutRect& area) {
    LayoutRect content = panels::drawHeader(canvas, area, "Select Music Folder", metrics_.scale,
                                            metrics_.text.header, fpCloseRc_, true);
    Rect c = toRect(content);
    float pad = metrics_.space(SP_LG);

    // A filesystem path is DATA and stays serif unconditionally — it is
    // exactly the string the two-part rule (see this plan's preamble) exists
    // to protect, regardless of whether it happens to be pure ASCII today.
    canvas.textStyled(truncateToWidth(canvas, fpCurrentDir_, c.w - 2.0f * pad, metrics_.text.secondary, FontStyle::Math),
                      c.x + pad, c.y + pad, metrics_.text.secondary, toColor(CLR_TEXT_DIM), FontStyle::Math);

    float listTop = pad * 2.0f + metrics_.text.secondary * 1.4f;
    float btnH = metrics_.space(58.0f);
    LayoutRect listArea = { content.left, (int)(content.top + listTop),
                            content.right, (int)(content.bottom - (btnH + pad * 2.0f)) };
    fpListArea_ = listArea;

    std::vector<std::string> labels;
    labels.reserve(fpEntries_.size() + 1);
    if (fpHasParent_) labels.push_back(".. (parent folder)");
    labels.insert(labels.end(), fpEntries_.begin(), fpEntries_.end());

    float fpRowH = panelRowH();
    fpListRows_ = widgets::drawScrollList(canvas, toRect(listArea), labels,
                                          -1, (float)fpScrollY_, fpRowH,
                                          fpHoverRow_, widgets::kTextFree, matrixListStyle());
    panels::drawScrollbar(canvas, listArea, (int)((float)labels.size() * fpRowH), fpScrollY_, metrics_.scale);
    if (labels.empty()) {
        Rect a = toRect(listArea);
        const float ex = a.x + metrics_.space(22.0f), ey = a.y + metrics_.space(22.0f);
        if (!drawTerminusText(canvas, "No subfolders here.", ex, ey,
                              metrics_.text.body, toColor(CLR_TEXT_DIM)))
            canvas.textStyled("No subfolders here.", ex, ey,
                              metrics_.text.body, toColor(CLR_TEXT_DIM), FontStyle::Italic);
    }

    float btnW = metrics_.space(326.0f);
    int by = (int)(content.bottom - (btnH + pad));
    auto fpRects = panels::layoutEdgePair(
        content, pad, btnW, btnW,
        metrics_.space(panels::kMinActionBtnW), metrics_.space(SP_MD), by, (int)btnH);
    fpBtnCancel_ = fpRects.first;
    fpBtnSelect_ = fpRects.second;
    panels::drawButton(canvas, fpBtnCancel_, "Cancel", fpHoverCancel_, metrics_.text.body, false, true);
    panels::drawButton(canvas, fpBtnSelect_, "Select This Folder", fpHoverSelect_, metrics_.text.body, true, true);
}
```

- [ ] **Step 3: Build, test, capture**

Run: `scripts/linux/build.sh --debug`, all 13 tests pass.
Capture `matrix_ui_capture --out <dir>` — look at Manage Folders and the Folder Picker (open it via Manage Folders' "Add folder" route) at desktop size and both phone sizes: header, buttons and the empty-state message read in Terminus; the folder path and any listed folder names stay serif exactly as before.

- [ ] **Step 4: Commit**

```bash
./git_wrapper commit "Manage Folders and the Folder Picker: chrome in Terminus"
```

---

### Task 4: Interface Settings

**Files:** Modify `gui/src/player_view.cc` — `drawInterfaceSettings`.

**Interfaces:** Same as Task 3.

- [ ] **Step 1: Replace the whole function body** (line 8776):

```cpp
void PlayerWindow::drawInterfaceSettings(Canvas& canvas, const LayoutRect& area) {
    LayoutRect content = panels::drawHeader(canvas, area, "Interface",
                                            metrics_.scale, metrics_.text.header, isCloseRc_, true);
    Rect c = toRect(content);
    // A fixed-literal helper: Terminus, falling back to the original serif
    // draw automatically (several of these sentences use an em dash and are
    // EXPECTED to fall back — see this plan's preamble).
    auto chromeText = [&](const std::string& s, float x, float y, float sz,
                          ColorRef col, FontStyle style) {
        if (!drawTerminusText(canvas, s, x, y, sz, toColor(col)))
            canvas.textStyled(s, x, y, sz, toColor(col), style);
    };

    const float pad  = metrics_.space(SP_LG);
    const float rowH = panelRowH();
    float y = c.y + pad;

    chromeText("Scrolling", c.x + pad, y, metrics_.text.body, CLR_TEXT_DIM, FontStyle::Bold);
    y += rowH;

    // Two toggles, not one, and the reason is worth stating on screen as well
    // as in the code: a finger and a wheel start from opposite conventions, so
    // a single "reverse scrolling" switch is necessarily wrong on one of them.
    // Each row says which direction it currently means rather than making the
    // listener reason about the word "invert".
    struct Row { LayoutRect* rc; bool on; const char* title; const char* onS; const char* offS; };
    const Row rows[] = {
        { &isRowTouch_, scrollInvertTouch_, "Touch",
          "Reversed — the content moves against your finger",
          "Natural — the content follows your finger" },
        { &isRowWheel_, scrollInvertWheel_, "Mouse wheel",
          "Reversed — wheel away scrolls down",
          "Traditional — wheel away scrolls up" },
    };
    for (int i = 0; i < 2; i++) {
        LayoutRect rc = { (int)(c.x + pad), (int)y, (int)(c.x + c.w - pad), (int)(y + rowH) };
        *rows[i].rc = rc;
        if (isHoverRow_ == i)
            canvas.rect((float)rc.left, (float)rc.top, (float)(rc.right - rc.left),
                        (float)(rc.bottom - rc.top), toColor(CLR_HOVER), UI_CORNER_RADIUS);
        widgets::ToggleStyle st;
        st.onColor   = toColor(CLR_ACCENT);
        st.offColor  = toColor(CLR_SEPARATOR);
        st.knobColor = toColor(CLR_TEXT_PRIMARY);
        // widgets::drawToggle draws its own title ("Touch"/"Mouse wheel")
        // internally — a vk_canvas submodule call, left untouched. It stays
        // serif; see this plan's Task 7 "explicitly not done" list.
        widgets::drawToggle(canvas, toRect(rc), rows[i].on, rows[i].title, st);
        y += rowH;
        // The state in words, under the switch. Both sentences use an em
        // dash, so they fall back to serif every time — expected, not a bug.
        chromeText(rows[i].on ? rows[i].onS : rows[i].offS,
                  c.x + pad + metrics_.space(SP_MD), y, metrics_.text.secondary,
                  CLR_TEXT_DIM, FontStyle::Italic);
        y += rowH * 0.9f;
    }

    y += rowH * 0.5f;
    chromeText("Flicking a list throws it, and it slows to a stop on its own.",
              c.x + pad, y, metrics_.text.secondary, CLR_TEXT_DIM, FontStyle::Italic);

    panels::drawButton(canvas, isCloseRc_, "Close", isHoverClose_, metrics_.text.body, false, true);
}
```

- [ ] **Step 2: Build, test, capture, commit**

Run: `scripts/linux/build.sh --debug`, 13 tests pass. Capture the Interface panel; header/"Scrolling"/"Flicking..."/"Close" read in Terminus, the two toggle rows' state sentences stay serif (em dash), the toggle titles ("Touch"/"Mouse wheel") stay serif (submodule).

```bash
./git_wrapper commit "Interface Settings: chrome in Terminus"
```

---

### Task 5: EQ Settings

**Files:** Modify `gui/src/player_view.cc` — `drawEqSettings`.

**Interfaces:** Consumes Task 1's `terminusTextWidth`/`drawTerminusText` directly (the tab strip does its own centering, the pattern the spec calls out as needing width-before-draw) as well as the wrapped-lambda idiom.

- [ ] **Step 1: Replace the whole function body** (line 6027):

```cpp
void PlayerWindow::drawEqSettings(Canvas& canvas, const LayoutRect& area) {
    ensureEqProfiles();
    LayoutRect content = panels::drawHeader(canvas, area, "EQ / AutoEQ Profiles", metrics_.scale,
                                            metrics_.text.header, eqCloseRc_, true);
    Rect c = toRect(content);
    float pad = metrics_.space(SP_LG);
    float y = c.y + pad;
    auto chromeText = [&](const std::string& s, float x, float yy, float sz,
                          ColorRef col, FontStyle style) {
        if (!drawTerminusText(canvas, s, x, yy, sz, toColor(col)))
            canvas.textStyled(s, x, yy, sz, toColor(col), style);
    };

    // Mono, because "32BB:0004" is an identifier, not a name. The family
    // already carries a face that says so, and it is the one with the most
    // legibility headroom of the four (9.14px floor against the serif's
    // 18.29px — see min_text_size in the root CMakeLists). Driver and album
    // names stay serif: the rule is about what the string IS, not where it sits.
    // Both header lines come from the cache: the assignment is a database read,
    // and re-running it per frame bought nothing — see eqAssignLineDirty_.
    if (eqAssignLineDirty_) {
        eqDeviceLine_ = "Device: " + eqDeviceKey_;
        EqAssignment assign;
        if (db_.loadEqAssignment(eqDeviceKey_, assign) || db_.loadEqAssignment("global", assign))
            eqAssignLine_ = "Current EQ: " + assign.name;
        else
            eqAssignLine_ = "No EQ assigned";
        eqAssignLineDirty_ = false;
    }

    // Both lines are LITERAL PREFIX + RUNTIME DATA (a device key, a profile
    // name) — the two-part rule (this plan's preamble) says leave these
    // unconditionally serif, never attempt Terminus even though the data is
    // usually ASCII: a profile's real name must not be re-cased.
    canvas.textStyled(eqDeviceLine_, c.x + pad, y, metrics_.text.secondary, toColor(CLR_TEXT_DIM), FontStyle::Math);
    y += metrics_.text.secondary * 1.6f;

    canvas.textStyled(eqAssignLine_, c.x + pad, y, metrics_.text.secondary, toColor(CLR_ACCENT), FontStyle::Roman);
    y += metrics_.text.secondary * 1.8f;

    if (eqBitperfectActive_) {
        // A fixed literal (falls back — em dash).
        chromeText("Bitperfect mode active — EQ applies once Reference EQ mode is enabled.",
                  c.x + pad, y, metrics_.text.secondary, CLR_TEXT_DIM, FontStyle::Italic);
        y += metrics_.text.secondary * 1.6f;
    }

    // Two views over one list: the saved set, or the whole catalogue. This is
    // also the ONLY place a saved pair is pinned or removed — the sidebar
    // block stays a pure switcher, with no room for a per-row × at space(277).
    {
        float tabH = metrics_.space(52.0f);
        auto tabRects = panels::layoutButtonRow(content, pad, 3, metrics_.space(200.0f),
                                                metrics_.space(SP_SM), metrics_.space(panels::kMinActionBtnW),
                                                (int)y, (int)tabH, /*alignRight=*/false);
        eqTabMine_ = tabRects[0];
        eqTabAll_  = tabRects[1];
        eqTabRecommended_ = tabRects[2];
        auto tab = [&](const LayoutRect& rc, const char* label, bool active, bool hovered) {
            Rect r = toRect(rc);
            // Accent = state, hover = neutral (UI_DESIGN_SYSTEM.md §1.4).
            if (active)
                canvas.rect(r.x, r.y, r.w, r.h,
                            toColor(CLR_ACCENT, UI_SELECT_TINT_ALPHA), UI_CORNER_RADIUS);
            else if (hovered)
                canvas.rect(r.x, r.y, r.w, r.h, toColor(CLR_HOVER), UI_CORNER_RADIUS);
            if (active)
                canvas.rect(r.x, r.y + r.h - metrics_.stroke(2.0f), r.w,
                            metrics_.stroke(2.0f), toColor(CLR_ACCENT));
            const ColorRef col = active ? CLR_ACCENT : CLR_TEXT_SECONDARY;
            // Centering needs the width BEFORE drawing, so terminusTextWidth
            // is called first — the pattern the spec's API doc calls out.
            const float tw = terminusTextWidth(label, metrics_.text.body);
            if (tw >= 0.0f) {
                drawTerminusText(canvas, label, r.x + std::max(0.0f, (r.w - tw) * 0.5f),
                                 r.y + r.h * 0.5f - metrics_.text.body * 0.5f,
                                 metrics_.text.body, toColor(col));
            } else {
                const float stw = canvas.textWidthStyled(label, metrics_.text.body, FontStyle::Roman);
                canvas.textStyled(label, r.x + std::max(0.0f, (r.w - stw) * 0.5f),
                                  r.y + r.h * 0.5f - metrics_.text.body * 0.5f,
                                  metrics_.text.body, toColor(col), FontStyle::Roman);
            }
        };
        tab(eqTabMine_, "My Drivers", eqShowMine_,  eqHoverTabMine_);
        tab(eqTabAll_,  "All Profiles",  !eqShowMine_, eqHoverTabAll_);
        tab(eqTabRecommended_, eqRecommendedOnly_ ? "Best only" : "Every source",
            !eqShowMine_ && eqRecommendedOnly_, eqHoverTabRecommended_);
        y += tabH + metrics_.space(SP_SM);
    }

    eqSearchRc_ = { (int)(c.x + pad), (int)y, (int)(c.x + c.w - pad), (int)(y + metrics_.space(55.0f)) };
    drawSearchField(canvas, eqSearchRc_, eqSearch_, eqSearchFocused_, "Search profiles",
                    metrics_.text.body);
    y += metrics_.space(55.0f) + metrics_.space(16.0f);

    float btnH = metrics_.space(58.0f);
    LayoutRect listArea = { content.left, (int)y, content.right, (int)(content.bottom - (btnH + pad * 2)) };
    eqListArea_ = listArea;

    std::vector<std::string> labels;
    labels.reserve(eqFilteredIndices_.size());
    auto& all = eqProfiles_.getAll();
    for (int idx : eqFilteredIndices_) {
        std::string label = all[idx].name;
        if (!all[idx].form.empty()) label += "  (" + all[idx].form + ")";
        if (!eqRecommendedOnly_ || eqShowMine_) {
            label += "  ·  " + all[idx].source;
            if (!all[idx].rig.empty()) label += " / " + all[idx].rig;
        }
        if (eqShowMine_) {
            for (const auto& h : eqHeadphones_) {
                if (h.name == all[idx].name && h.source == all[idx].source &&
                    h.form == all[idx].form) {
                    if (h.pinned) label += "  — pinned";
                    break;
                }
            }
        }
        labels.push_back(label);
    }
    // Profile names/sources/rigs are data and go through the untouchable
    // row-list widget unchanged.
    float eqRowH = panelRowH();
    eqListRows_ = widgets::drawScrollList(canvas, toRect(listArea), labels,
                                          eqSelectedRow_, (float)eqScrollY_, eqRowH,
                                          eqHoverRow_, widgets::kTextFree, matrixListStyle());
    panels::drawScrollbar(canvas, listArea, (int)((float)labels.size() * eqRowH), eqScrollY_, metrics_.scale);
    if (labels.empty()) {
        Rect a = toRect(listArea);
        // All THREE branches are fixed literals (no data embedded) — safe to
        // wrap uniformly regardless of which one is picked at runtime.
        const char* msg = eqShowMine_
            ? (eqSearch_.empty()
                 ? "No drivers saved yet — pick a profile under All Profiles "
                   "and listen for a minute."
                 : "No saved drivers match.")
            : "No profiles match.";
        chromeText(msg, a.x + metrics_.space(22.0f), a.y + metrics_.space(22.0f),
                  metrics_.text.body, CLR_TEXT_DIM, FontStyle::Italic);
    }

    int by = (int)(content.bottom - (btnH + pad));
    int eqBtnCount = eqShowMine_ ? 3 : 2;
    auto eqBtnRects = panels::layoutButtonRow(content, pad, eqBtnCount, metrics_.space(277.0f),
                                              metrics_.space(SP_MD), metrics_.space(panels::kMinActionBtnW),
                                              by, (int)btnH);
    eqBtnAssign_ = eqBtnRects[0];
    if (eqShowMine_) {
        const EqHeadphone* sel = eqSelectedHeadphone();
        eqBtnPin_    = eqBtnRects[1];
        eqBtnRemove_ = eqBtnRects[2];
        eqBtnClear_  = {};
        panels::drawButton(canvas, eqBtnAssign_, "Select", eqHoverAssign_, metrics_.text.body, true, true);
        panels::drawButton(canvas, eqBtnPin_,
                           (sel && sel->pinned) ? "Unpin" : "Pin",
                           eqHoverPin_, metrics_.text.body, false, true);
        panels::drawButton(canvas, eqBtnRemove_, "Remove", eqHoverRemove_, metrics_.text.body, false, true);
    } else {
        eqBtnClear_  = eqBtnRects[1];
        eqBtnPin_    = {};
        eqBtnRemove_ = {};
        panels::drawButton(canvas, eqBtnAssign_, "Assign to Device", eqHoverAssign_, metrics_.text.body, true, true);
        panels::drawButton(canvas, eqBtnClear_, "Clear", eqHoverClear_, metrics_.text.body, false, true);
    }
}
```

- [ ] **Step 2: Build, test, capture, commit**

Run: `scripts/linux/build.sh --debug`, 13 tests pass. Capture EQ Settings in both tabs (My Drivers / All Profiles) at desktop and phone sizes: header, tab labels, empty-state message, and every button read in Terminus; the device/assignment lines and every profile row stay serif.

```bash
./git_wrapper commit "EQ Settings: chrome in Terminus"
```

---

### Task 6: Audio Settings and its Bluetooth codec section

**Files:** Modify `gui/src/player_view.cc` — `drawAudioSettings`, `drawBluetoothCodecSection`.

**Interfaces:** Same as Task 3-5. This task has the plan's only three genuinely runtime-data call sites (`btDevice_.name`, `bt_codec::adbGrantCommand()`, `btNotice_`) plus two literal-prefix-plus-data strings that must stay **unwrapped** — called out explicitly below.

- [ ] **Step 1: Replace `drawAudioSettings`'s whole body** (line 5299):

```cpp
void PlayerWindow::drawAudioSettings(Canvas& canvas, const LayoutRect& area) {
    LayoutRect content = panels::drawHeader(canvas, area, "Audio Output Settings", metrics_.scale,
                                            metrics_.text.header, asCloseRc_, true);
    Rect c = toRect(content);
    float pad = metrics_.space(SP_LG);
    float y = c.y + pad;
    // Every direct call in this function is a fixed literal (no runtime data
    // concatenated in) — see drawBluetoothCodecSection below for the ones
    // that are NOT and must stay unwrapped.
    auto chromeText = [&](const std::string& s, float x, float yy, float sz,
                          ColorRef col, FontStyle style) {
        if (!drawTerminusText(canvas, s, x, yy, sz, toColor(col)))
            canvas.textStyled(s, x, yy, sz, toColor(col), style);
    };

    chromeText("Output backend:", c.x + pad, y, metrics_.text.body, CLR_TEXT_DIM, FontStyle::Roman);
    y += metrics_.text.body * 1.8f;

    float rowH = metrics_.space(55.0f);
    asBackendRowRects_.assign(asBackendOptions_.size(), LayoutRect{});
    for (int i = 0; i < (int)asBackendOptions_.size(); i++) {
        LayoutRect rc = { (int)(c.x + pad), (int)y, (int)(c.x + c.w - pad), (int)(y + rowH) };
        bool sel = (i == asBackendSelIdx_);
        // widgets::drawRadioRow draws its own label (a backend name here) —
        // a vk_canvas submodule call, left untouched; stays serif.
        Rect hit = widgets::drawRadioRow(canvas, toRect(rc), sel, (i == asHoverBackendRow_),
                                         backendDisplayName(asBackendOptions_[i]),
                                         widgets::kTextFree, matrixRadioStyle());
        asBackendRowRects_[i] = toLayoutRect(hit);
        y += rowH;
    }
    y += metrics_.space(SP_MD);

    AudioBackend sel = asBackendOptions_.empty() ? AudioBackend::Usb : asBackendOptions_[asBackendSelIdx_];

    float btnH = metrics_.space(58.0f);
    float listTop = y + metrics_.text.body * 1.6f;
    float listBottomLimit = (float)content.bottom - pad - btnH - pad;
#ifdef _WIN32
    if (sel == AudioBackend::Wasapi)
        listBottomLimit -= metrics_.space(SP_MD) + metrics_.text.body * 1.6f + 2.0f * rowH + metrics_.space(SP_MD);
#endif
    float listRowH = panelRowH();
    auto listHeightFor = [&](int rowCount) {
        float minH  = 3.0f * listRowH;
        float avail = std::max(listBottomLimit - listTop, minH);
        return std::clamp((float)rowCount * listRowH, minH, avail);
    };

    if (sel == AudioBackend::Usb) {
        chromeText("USB DAC:", c.x + pad, y, metrics_.text.body, CLR_TEXT_DIM, FontStyle::Roman);
        y += metrics_.text.body * 1.6f;
        std::vector<std::string> labels;
        for (auto& d : asUsbDevices_) labels.push_back(d.name);
        float listH = listHeightFor((int)labels.size());
        asDeviceListArea_ = { (int)(c.x + pad), (int)y, (int)(c.x + c.w - pad), (int)(y + listH) };
        asDeviceListRows_ = widgets::drawScrollList(canvas, toRect(asDeviceListArea_), labels,
                                                    asUsbSel_, (float)asDeviceScrollY_, listRowH,
                                                    asHoverDeviceRow_, widgets::kTextFree, matrixListStyle());
        panels::drawScrollbar(canvas, asDeviceListArea_, (int)((float)labels.size() * listRowH),
                              asDeviceScrollY_, metrics_.scale);
        if (labels.empty()) {
            Rect a = toRect(asDeviceListArea_);
            chromeText("No USB audio devices found.", a.x + metrics_.space(22.0f), a.y + metrics_.space(22.0f),
                      metrics_.text.body, CLR_TEXT_DIM, FontStyle::Italic);
        }
        y += listH + metrics_.space(SP_MD);
    }
#ifdef _WIN32
    else if (sel == AudioBackend::Wasapi) {
        chromeText("Device:", c.x + pad, y, metrics_.text.body, CLR_TEXT_DIM, FontStyle::Roman);
        y += metrics_.text.body * 1.6f;
        std::vector<std::string> labels;
        labels.push_back("(Default device)");
        for (auto& d : asWasapiDevices_) labels.push_back(wideToUtf8(d.name));
        float listH = listHeightFor((int)labels.size());
        asDeviceListArea_ = { (int)(c.x + pad), (int)y, (int)(c.x + c.w - pad), (int)(y + listH) };
        asDeviceListRows_ = widgets::drawScrollList(canvas, toRect(asDeviceListArea_), labels,
                                                    asWasapiSel_, (float)asDeviceScrollY_, listRowH,
                                                    asHoverDeviceRow_, widgets::kTextFree, matrixListStyle());
        panels::drawScrollbar(canvas, asDeviceListArea_, (int)((float)labels.size() * listRowH),
                              asDeviceScrollY_, metrics_.scale);
        y += listH + metrics_.space(SP_MD);

        chromeText("Mode:", c.x + pad, y, metrics_.text.body, CLR_TEXT_DIM, FontStyle::Roman);
        y += metrics_.text.body * 1.6f;
        static const char* kModeLabels[2] = {
            "Shared \xE2\x80\x94 other apps can play simultaneously",
            "Exclusive \xE2\x80\x94 lower latency, blocks other apps" };
        for (int i = 0; i < 2; i++) {
            LayoutRect rc = { (int)(c.x + pad), (int)y, (int)(c.x + c.w - pad), (int)(y + rowH) };
            bool s2 = (asExclusive_ == (i == 1));
            Rect hit = widgets::drawRadioRow(canvas, toRect(rc), s2, (i == asHoverModeRow_),
                                             kModeLabels[i], widgets::kTextFree, matrixRadioStyle());
            asModeRows_[i] = toLayoutRect(hit);
            y += rowH;
        }
        y += metrics_.space(SP_MD);
    }
#else
#ifdef MATRIX_HAVE_ALSA
    else if (sel == AudioBackend::Alsa) {
        chromeText("Device:", c.x + pad, y, metrics_.text.body, CLR_TEXT_DIM, FontStyle::Roman);
        y += metrics_.text.body * 1.6f;
        std::vector<std::string> labels;
        labels.push_back("(System default)");
        for (auto& d : asAlsaDevices_) labels.push_back(d.name);
        float listH = listHeightFor((int)labels.size());
        asDeviceListArea_ = { (int)(c.x + pad), (int)y, (int)(c.x + c.w - pad), (int)(y + listH) };
        asDeviceListRows_ = widgets::drawScrollList(canvas, toRect(asDeviceListArea_), labels,
                                                    asAlsaSel_, (float)asDeviceScrollY_, listRowH,
                                                    asHoverDeviceRow_, widgets::kTextFree, matrixListStyle());
        panels::drawScrollbar(canvas, asDeviceListArea_, (int)((float)labels.size() * listRowH),
                              asDeviceScrollY_, metrics_.scale);
        y += listH + metrics_.space(SP_MD);
    }
#endif
#ifdef MATRIX_HAVE_JACK
    else if (sel == AudioBackend::Jack) {
        chromeText("Starting port:", c.x + pad, y, metrics_.text.body, CLR_TEXT_DIM, FontStyle::Roman);
        y += metrics_.text.body * 1.6f;
        std::vector<std::string> labels;
        labels.push_back("(Auto-connect to first available ports)");
        for (auto& p : asJackPorts_) labels.push_back(p.portName);
        float listH = listHeightFor((int)labels.size());
        asDeviceListArea_ = { (int)(c.x + pad), (int)y, (int)(c.x + c.w - pad), (int)(y + listH) };
        asDeviceListRows_ = widgets::drawScrollList(canvas, toRect(asDeviceListArea_), labels,
                                                    asJackSel_, (float)asDeviceScrollY_, listRowH,
                                                    asHoverDeviceRow_, widgets::kTextFree, matrixListStyle());
        panels::drawScrollbar(canvas, asDeviceListArea_, (int)((float)labels.size() * listRowH),
                              asDeviceScrollY_, metrics_.scale);
        if (asJackPorts_.empty()) {
            Rect a = toRect(asDeviceListArea_);
            chromeText("No running JACK server found (or no physical playback ports).",
                      a.x + metrics_.space(22.0f), a.y + metrics_.space(98.0f),
                      metrics_.text.body, CLR_TEXT_DIM, FontStyle::Italic);
        }
        y += listH + metrics_.space(SP_MD);
    }
#endif
#ifdef MATRIX_HAVE_BLUETOOTH
    else if (sel == AudioBackend::Bluetooth) {
        chromeText("Headphones:", c.x + pad, y, metrics_.text.body, CLR_TEXT_DIM, FontStyle::Roman);
        y += metrics_.text.body * 1.6f;
        std::vector<std::string> labels;
        for (auto& d : asBtDevices_) {
            std::string label = d.name;
            if (!d.connected)  label += "  — not connected";
            else if (d.busy)   label += "  — in use by another app";
            labels.push_back(std::move(label));
        }
        float listH = listHeightFor(std::max(1, (int)labels.size()));
        asDeviceListArea_ = { (int)(c.x + pad), (int)y, (int)(c.x + c.w - pad), (int)(y + listH) };
        asDeviceListRows_ = widgets::drawScrollList(canvas, toRect(asDeviceListArea_), labels,
                                                    asBtSel_, (float)asDeviceScrollY_, listRowH,
                                                    asHoverDeviceRow_, widgets::kTextFree, matrixListStyle());
        panels::drawScrollbar(canvas, asDeviceListArea_, (int)((float)labels.size() * listRowH),
                              asDeviceScrollY_, metrics_.scale);
        if (asBtDevices_.empty()) {
            Rect a = toRect(asDeviceListArea_);
            chromeText("No paired A2DP device. Pair and connect a pair of headphones first.",
                      a.x + metrics_.space(22.0f), a.y + metrics_.space(98.0f),
                      metrics_.text.body, CLR_TEXT_DIM, FontStyle::Italic);
        }
        y += listH + metrics_.space(SP_MD);

        chromeText("SBC is a lossy encode — this route can never be bit-perfect.",
                  c.x + pad, y, metrics_.text.body, CLR_WARNING, FontStyle::Italic);
        y += metrics_.text.body * 1.8f;
        chromeText("Release the device in your sound server first; only one app can stream to it.",
                  c.x + pad, y, metrics_.text.body, CLR_TEXT_DIM, FontStyle::Italic);
        y += metrics_.text.body * 1.6f + metrics_.space(SP_MD);
    }
#endif
#ifdef MATRIX_HAVE_AAUDIO
    else if (sel == AudioBackend::AAudio) {
        asDeviceListArea_ = {};
        asDeviceListRows_.clear();
        chromeText("Android chooses the output route itself, and follows it when you",
                  c.x + pad, y, metrics_.text.body, CLR_TEXT_DIM, FontStyle::Italic);
        y += metrics_.text.body * 1.4f;
        chromeText("plug in headphones — there is no device to pick here.",
                  c.x + pad, y, metrics_.text.body, CLR_TEXT_DIM, FontStyle::Italic);
        y += metrics_.text.body * 1.9f;
        chromeText("16-bit output. Not a bit-perfect path for deeper sources.",
                  c.x + pad, y, metrics_.text.body, CLR_TEXT_DIM, FontStyle::Italic);
        y += metrics_.space(SP_MD) + metrics_.text.body;

        drawBluetoothCodecSection(canvas, c, y, pad);
    }
#endif
#ifdef MATRIX_HAVE_AOAS
    else if (sel == AudioBackend::Aoas) {
        asDeviceListArea_ = {};
        asDeviceListRows_.clear();
        chromeText("Playback goes through the AOAS service, which owns the USB",
                  c.x + pad, y, metrics_.text.body, CLR_TEXT_DIM, FontStyle::Italic);
        y += metrics_.text.body * 1.4f;
        chromeText("permission and the DAC's isochronous stream — bit-exact,",
                  c.x + pad, y, metrics_.text.body, CLR_TEXT_DIM, FontStyle::Italic);
        y += metrics_.text.body * 1.4f;
        chromeText("and silent across app switches. AOAS must be installed and",
                  c.x + pad, y, metrics_.text.body, CLR_TEXT_DIM, FontStyle::Italic);
        y += metrics_.text.body * 1.4f;
        chromeText("signed with the same key as this app. There is no device to pick here.",
                  c.x + pad, y, metrics_.text.body, CLR_TEXT_DIM, FontStyle::Italic);
        y += metrics_.space(SP_MD) + metrics_.text.body;
    }
#endif
#endif

    int by = (int)(content.bottom - (btnH + pad));
    auto asRects = panels::layoutButtonRow(content, pad, 1, metrics_.space(196.0f), 0.0f,
                                           metrics_.space(panels::kMinActionBtnW), by, (int)btnH);
    asBtnApply_ = asRects[0];
    panels::drawButton(canvas, asBtnApply_, "Apply", asHoverApply_, metrics_.text.body, true, true);
}
```

(`kModeLabels` keeps its `\xE2\x80\x94` escapes exactly as they already are in the file — this step is a pure `chromeText`-wrapping pass, not a rewrite of existing bytes. Do not retype these two lines from a Python string.)

- [ ] **Step 2: Replace `drawBluetoothCodecSection`'s whole body** (line 5604):

```cpp
void PlayerWindow::drawBluetoothCodecSection(Canvas& canvas, const Rect& c, float& y, float pad) {
    asBtCodecRows_.clear();
    asBtRateRc_ = asBtBitsRc_ = asBtQualityRc_ = asBtEnableRc_ = asBtForgetRc_ = LayoutRect{};

    const bt_codec::Capability cap = asBtCap_;
    if (cap == bt_codec::Capability::Unavailable && btDevice_.empty()) return;

    auto chromeText = [&](const std::string& s, float x, float yy, float sz,
                          ColorRef col, FontStyle style) {
        if (!drawTerminusText(canvas, s, x, yy, sz, toColor(col)))
            canvas.textStyled(s, x, yy, sz, toColor(col), style);
    };

    const float lineH = metrics_.text.body * 1.5f;
    chromeText("Bluetooth codec", c.x + pad, y, metrics_.text.body, CLR_TEXT_DIM, FontStyle::Bold);
    y += lineH * 1.2f;

    if (btDevice_.empty()) {
        chromeText("No Bluetooth headphones connected.", c.x + pad, y,
                  metrics_.text.body, CLR_TEXT_DIM, FontStyle::Italic);
        y += lineH;
        return;
    }

    // The device NAME is pure data — stays serif unconditionally, never
    // attempts Terminus even though most Bluetooth names are ASCII.
    canvas.textStyled(btDevice_.name, c.x + pad, y, metrics_.text.body,
                      toColor(CLR_TEXT_PRIMARY), FontStyle::Roman);
    y += lineH;

    // What is actually running, always — this needs no permission and it is the
    // single most useful line on the page for a listener on Bluetooth.
    const std::string now = btActive_.valid() ? bt_codec::summary(btActive_)
                                              : std::string("unknown");
    // LITERAL PREFIX + DATA (the negotiated codec summary) — unwrapped, per
    // the two-part rule: "Now: SBC 44100Hz 16bit" must read exactly as the
    // stack reports it, never reformatted.
    canvas.textStyled("Now: " + now, c.x + pad, y, metrics_.text.body,
                      toColor(btActive_.valid() ? CLR_WARNING : CLR_TEXT_DIM), FontStyle::Roman);
    y += lineH * 1.3f;

    if (cap != bt_codec::Capability::Writable) {
        chromeText("This phone has not granted codec control. Two ways to get it:",
                  c.x + pad, y, metrics_.text.body, CLR_TEXT_DIM, FontStyle::Italic);
        y += lineH;

        const float btnH = metrics_.space(50.0f);
        asBtEnableRc_ = { (int)(c.x + pad), (int)y,
                          (int)(c.x + pad + metrics_.space(330.0f)), (int)(y + btnH) };
        panels::drawButton(canvas, asBtEnableRc_, "Pair as companion device",
                           asHoverBtEnable_, metrics_.text.body, false, true);
        y += btnH + metrics_.space(SP_SM);

        chromeText("...or, from a computer, once:", c.x + pad, y,
                  metrics_.text.body, CLR_TEXT_DIM, FontStyle::Italic);
        y += lineH;
        // The literal adb command is DATA (it embeds this device's MAC) —
        // stays serif unconditionally. It is also the one string on this
        // page a listener may need to copy character-for-character.
        canvas.textStyled(bt_codec::adbGrantCommand(), c.x + pad, y,
                          metrics_.text.secondary, toColor(CLR_TEXT_SECONDARY), FontStyle::Roman);
        y += lineH * 1.4f;
        return;
    }

    seedBtEditFromDevice();

    const float rowH = metrics_.space(48.0f);
    asBtCodecRows_.assign(asBtSelectable_.size(), LayoutRect{});
    for (int i = 0; i < (int)asBtSelectable_.size(); i++) {
        LayoutRect rc = { (int)(c.x + pad), (int)y, (int)(c.x + c.w - pad), (int)(y + rowH) };
        // widgets::drawRadioRow draws its own label (a codec name) —
        // submodule call, untouched, stays serif.
        Rect hit = widgets::drawRadioRow(canvas, toRect(rc),
                                         asBtEdit_.codec == asBtSelectable_[i].id,
                                         (i == asHoverBtCodecRow_),
                                         asBtSelectable_[i].name,
                                         widgets::kTextFree, matrixRadioStyle());
        asBtCodecRows_[i] = toLayoutRect(hit);
        y += rowH;
    }
    if (asBtSelectable_.empty()) {
        // LITERAL both before AND after the embedded device name — unwrapped
        // per the two-part rule; the name must not be re-cased mid-sentence.
        canvas.textStyled("This phone will not say which codecs " + btDevice_.name +
                          " supports, so none can be offered.",
                          c.x + pad, y, metrics_.text.body, toColor(CLR_TEXT_DIM),
                          FontStyle::Italic);
        y += lineH * 1.4f;
    }
    y += metrics_.space(SP_SM);

    // Rate, depth and LDAC quality: each label is chosen from a small, fixed,
    // developer-authored set (see bt_codec.hh), not open-ended data — chrome,
    // wrapped like any other button.
    const float btnH = metrics_.space(50.0f);
    const float btnW = metrics_.space(200.0f);
    float bx = (float)c.x + pad;
    asBtRateRc_ = { (int)bx, (int)y, (int)(bx + btnW), (int)(y + btnH) };
    panels::drawButton(canvas, asBtRateRc_, bt_codec::sampleRateLabel(asBtEdit_.sampleRate),
                       asHoverBtRate_, metrics_.text.body, false, true);
    bx += btnW + metrics_.space(SP_SM);
    asBtBitsRc_ = { (int)bx, (int)y, (int)(bx + btnW), (int)(y + btnH) };
    panels::drawButton(canvas, asBtBitsRc_, bt_codec::bitDepthLabel(asBtEdit_.bits),
                       asHoverBtBits_, metrics_.text.body, false, true);
    bx += btnW + metrics_.space(SP_SM);
    if (asBtEdit_.codec == bt_codec::kLdac) {
        asBtQualityRc_ = { (int)bx, (int)y, (int)(bx + btnW), (int)(y + btnH) };
        panels::drawButton(canvas, asBtQualityRc_,
                           bt_codec::ldacQualityLabel(asBtEdit_.ldacQuality),
                           asHoverBtQuality_, metrics_.text.body, false, true);
    }
    y += btnH + metrics_.space(SP_SM);

    BtCodecPref existing;
    if (db_.loadBtCodec(btDevice_.mac, existing)) {
        asBtForgetRc_ = { (int)(c.x + pad), (int)y,
                          (int)(c.x + pad + metrics_.space(200.0f)), (int)(y + btnH) };
        panels::drawButton(canvas, asBtForgetRc_, "Forget", asHoverBtForget_, metrics_.text.body, false, true);
        y += btnH + metrics_.space(SP_SM);
    }

    chromeText("Apply saves this against these headphones and re-applies it "
              "whenever they reconnect.",
              c.x + pad, y, metrics_.text.secondary, CLR_TEXT_DIM, FontStyle::Italic);
    y += lineH;
    if (!btNotice_.empty()) {
        // An assembled runtime notice — data, stays serif unconditionally.
        canvas.textStyled(btNotice_, c.x + pad, y, metrics_.text.secondary,
                          toColor(CLR_WARNING), FontStyle::Italic);
        y += lineH;
    }
}
```

- [ ] **Step 3: Build, test, capture**

Run: `scripts/linux/build.sh --debug`, 13 tests pass. Capture Audio Settings with each backend selected that this build has (`USB` always; `ALSA`/`JACK`/`Bluetooth` on Linux) at desktop and phone sizes: labels/captions/buttons read in Terminus; device names, the running-codec line ("Now: ..."), the adb command, and any Bluetooth notice all stay exactly serif, in their original case. Specifically confirm: `"Now: SBC ..."` is NOT drawn as `"NOW: SBC ..."`.

- [ ] **Step 4: Commit**

```bash
grep -rlP '\xc3\xa2\xc2\x80\xc2\x94' gui/src || echo clean
./git_wrapper commit "Audio Settings and its Bluetooth codec section: chrome in Terminus, data stays serif"
```

---

### Task 7: Documentation and full verification

**Files:** Modify `docs/UI_DESIGN_SYSTEM.md`, `CLAUDE.md`.

- [ ] **Step 1: `docs/UI_DESIGN_SYSTEM.md`** — add a short subsection near §8.1's sidebar/nav material (or a new §8.1b) describing: the running-text Terminus layer (`resolveText`/`terminusTextWidth`/`drawTerminusText`), ALL CAPS as the visual result, the two-part classification rule (literal → attempt Terminus; anything built from runtime data → always serif), and the explicit list of what stays serif because of the submodule boundary (`widgets::drawFitButton`'s two-line/shrink path when Terminus doesn't fit, `widgets::drawRadioRow`'s row labels, `widgets::drawToggle`'s titles, `widgets::drawScrollList`'s rows).

- [ ] **Step 2: `CLAUDE.md`** — in the Settings panels section, add a paragraph: Settings' chrome (headers, buttons, fixed captions) draws in Terminus via `drawTerminusText`/`terminusTextWidth` (`gui/src/terminus_glyph.hh`); the classification rule is NOT "is this ASCII" alone — a call site built from runtime data (a device name, a database key, an assembled notice) is written as a plain, permanently-serif `canvas.textStyled` call regardless of that data's script, and only a call whose whole string is a compile-time literal is wrapped. Cross-reference the spec and this plan by filename.

- [ ] **Step 3: Full verification**

```bash
scripts/linux/build.sh --debug && scripts/linux/build.sh --release
```

All 13 test binaries pass (unchanged count — this plan adds assertions to an existing test, not a new binary). Then:

```bash
./build/linux_debug/gui/matrix_ui_capture --out <dir>
```

Every Settings-panel state changed; diff by eye against the pre-Task-1 baseline (not byte-compare — the whole point is that they changed). Specifically confirm `EqSwitcher` (`45-eq-switcher`) and the signal chain (`44-*`) states are **byte-identical** to that same baseline — the `terminusChrome` default-false parameter must have left them untouched. Then:

```bash
./build/linux_debug/gui/matrix_ui_capture --fixture 60 --frame 720x1640 --out <dir2>
./build/linux_debug/gui/matrix_ui_capture --fixture 60 --frame 1640x720 --out <dir2h>
```

Look at all five Settings panels at both phone sizes: the 16px-strike body text (most labels) is legible; a 32px-strike header is comfortably sized. If 16px body text reads too small, that is a follow-up (a third strike), not a defect in this plan — see the spec's "Explicitly not done here."

```bash
cd android && VULKAN_SDK=/opt/shader-slang sh gradlew assembleRelease --no-daemon
nm -C app/build/intermediates/cxx/Release/*/obj/arm64-v8a/libmatrix_player_android.so \
  | grep -c 'drawTerminusText\|terminusTextWidth\|PlayerWindow::drawAudioSettings'
```

Expect all three symbols present (â‰¥1 each).

- [ ] **Step 4: Commit**

```bash
grep -rlP '\xc3\xa2\xc2\x80\xc2\x94' docs CLAUDE.md gui/src || echo clean
./git_wrapper commit "Docs: the Settings-panel Terminus layer and its two-part classification rule"
```

---

## Self-review (against the spec and its refinement)

| Requirement | Task |
|---|---|
| Full printable ASCII baked once, verified empirically to bake clean | 1 |
| `resolveText`/`terminusTextWidth`/`drawTerminusText`, ASCII-fold, whole-string fallback | 1 |
| `terminus_glyph_test` enumerates real chrome strings, zero-fallback assertion | 1 |
| `drawHeader`/`drawButton` opt-in, `EqSwitcher`/signal-chain untouched | 2 |
| All five panels' headers/buttons/captions convert | 3, 4, 5, 6 |
| Row-list data (folder paths, device names, profile names) untouched (submodule) | 3, 5, 6 (documented) |
| Radio-row and toggle labels untouched (submodule) | 6, 4 (documented) |
| Literal-prefix-plus-data strings stay unconditionally serif | 6 (`Now: `, `btDevice_.name`, the codecs sentence, `btNotice_`, `adbGrantCommand()`) |
| Docs updated with the two-part rule | 7 |

## Execution

Inline, in this session, task by task with the verification loop between tasks. Each task is one commit through `./git_wrapper commit`. Not pushed — `./git_wrapper push` is the user's call.

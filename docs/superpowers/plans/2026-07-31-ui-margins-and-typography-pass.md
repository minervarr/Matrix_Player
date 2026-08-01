# UI margins & typography pass — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make Complete-mode look composed — fix the grid's margin drift, cap the track list, unify panel button placement, and set machine text in Mono — without adding a single new element to the screen.

**Architecture:** Six independent edits, five of them inside `gui/src/player_view.cc`'s `recalcLayout()` and draw blocks. The only structural change is Task 1, which introduces three resolved-pixel layout members so that layout, drawing and hit-testing finally read one value instead of three. Nothing else gains state.

**Tech Stack:** C++17, vk_canvas `Canvas`/`FontStyle`, `UiMetrics::space()`/`stroke()`, CMake + Ninja, the headless capture tool `matrix_ui_capture`.

## Global Constraints

- **No type role may shrink.** `kMinReadableTextSizePx = 18.2857` (generated from font geometry, `build/*/generated/ui_min_text_size.gen.h`); `ui_metrics.hh` pins the smallest role (`caption`) to it exactly. Sizes may only grow. **Never hardcode 18.2857** — read it through `metrics_.text.*`.
- **No color token changes.** `theme.hh` is not edited by this plan.
- **No new icons**, no changes to `ui_icons.*` or the icon font.
- **No new elements on screen.** Nothing is drawn that was not already drawn.
- **No bare pixel literals.** Every distance goes through `metrics_.space()`; every stroke through `metrics_.stroke()` (`UI_DESIGN_SYSTEM.md` §4).
- **`UI_CORNER_RADIUS` stays 0.** Square everywhere except the radio dot.
- **Accent green means state, never hover** (§1.4).
- Commits use `./git_wrapper commit "..."` — **never** plain `git commit`. Note `git_wrapper` runs `git add -A`, so the working tree must be clean of unrelated work before starting.

## How this plan is verified (and why it is mostly not by unit tests)

This repo's four test executables are pure-logic (`ui_metrics_test`, `ui_icons_test`, `ui_text_test`, `variants_test`) and there is no visual test framework — `CLAUDE.md` states GUI changes are validated by building and looking. A margin has no assertion that is not a screenshot.

**One exception, Task 1.** Its top-pad derivation is real arithmetic with a
stated property ("the two margins come out optically equal"), so it is
extracted as a pure function and pinned in `ui_metrics_test` — the same reason
`ui_icons.cc` is split from `ui_icons_draw.cc` and `variants.cpp` from
`library.cpp`. No other task in this plan has arithmetic worth asserting; for
those, the loop below IS the test.

Each task's verification is the same loop, and it is not optional:

```bash
scripts/linux/build.sh --debug
cd build/linux_debug/gui
./matrix_ui_capture --out /tmp/shots-1440 --frame 2560x1440
./matrix_ui_capture --out /tmp/shots-1080 --frame 1920x1080
```

**Both resolutions, every time.** 1080 is where `metrics_.scale == 1.0` and a `space()` bug is invisible; 1440 is where it shows. Task 1's defect survived this long precisely because nobody looked at 1440.

`ui_metrics_test` must still pass after every task (it pins the scale math these edits lean on):

```bash
./build/linux_debug/gui/ui_metrics_test && echo OK
```

## File Structure

| File | Change | Responsible for |
|---|---|---|
| `gui/src/ui_metrics.hh` | Modify | `gridTopPad()` — the pure derivation (Task 1) |
| `gui/src/ui_metrics_test.cc` | Modify | Asserts that derivation (Task 1) |
| `gui/src/player_view.hh` | Modify | Three new resolved-pad members (Task 1) |
| `gui/src/player_view.cc` | Modify | All six edits — layout, draw, hit-test |
| `docs/UI_DESIGN_SYSTEM.md` | Modify | §8.2/§8.4/§8.6 updated to match (Task 7) |
| `framework/vk_canvas/core/widgets.hh` + `.cc` | Modify, **optional** | `ScrollListStyle::rowStyle` (Task 8 only) |

---

### Task 1: Unify the grid's padding across layout, draw and hit-test

**Files:**
- Modify: `gui/src/ui_metrics.hh` (add `gridTopPad()`)
- Modify: `gui/src/ui_metrics_test.cc` (assert it)
- Modify: `gui/src/player_view.hh` (near `gridPadX_`/`gridPadY_`, ~`:470`)
- Modify: `gui/src/player_view.cc:1012`, `:1026-1027`, `:1897`, `:1920`, `:2488`, `:2492-2493`, `:2501-2502`

**Interfaces:**
- Consumes: `metrics_.space()`, `gridCols_`, `gridArtSize_`, `gridTileSize_`, `rcGrid_` — all already computed in `recalcLayout()`.
- Produces: `inline int gridTopPad(int padXpx, int cellStepX, int artSize)` in `ui_metrics.hh`; and `int gridPadXpx_`, `int gridPadYpx_`, `int gridStepX_` — resolved device pixels, written only by `recalcLayout()`, read by the grid draw block and `gridHitTest()`. Later tasks do not use them.

**The defect:** `gridPadX_` (24) and `gridPadY_` (16) are passed through `metrics_.space()` when computing layout (`:1897`, `:1920`) but used **raw** when drawing (`:1012`, `:1026-1027`) and hit-testing (`:2488`, `:2492-2493`, `:2501-2502`). At 1440p the column width is reserved against ~32 px while tiles are painted at 24 px, and the top margin stays at 16 unscaled pixels forever.

- [ ] **Step 1: Add the three resolved members**

In `gui/src/player_view.hh`, immediately after `int gridPadY_ = 16;`:

```cpp
    // ── Resolved grid pads, in DEVICE pixels ────────────────────────────────
    // gridPadX_/gridPadY_ above are AUTHORED values (at the 1080 reference).
    // These three are what recalcLayout() resolves them to, and they are the
    // ONLY thing the draw block and gridHitTest() are allowed to read.
    //
    // They exist because those two used the authored numbers raw while
    // recalcLayout() passed the same numbers through space() — so at any
    // height above 1080 the layout reserved one column width and the draw
    // painted another, and the top margin never scaled at all.
    int gridPadXpx_ = 24;
    int gridPadYpx_ = 16;
    int gridStepX_  = 0;    // cell stride incl. margins; was recomputed twice
```

- [ ] **Step 2: Write the failing test for the derivation**

The top pad is derived arithmetic with a stated property, so it gets pinned.
Append to `gui/src/ui_metrics_test.cc`, inside `main()`:

```cpp
    // ── gridTopPad: the grid's two margins are equal BY CONSTRUCTION ────────
    // A tile is centered in its cell, so the visible left margin is the pad
    // plus half the cell's leftover slack. The top has no such slack, so the
    // top pad must absorb it or the first row bleeds against the window edge
    // while the sidebar beside it has air. That is the whole property.
    {
        // 32px pad, 354px cell stride, 314px art -> 20px slack per side.
        assert(gridTopPad(32, 354, 314) == 52);

        // No slack (art exactly fills the cell): the pads coincide.
        assert(gridTopPad(32, 314, 314) == 32);

        // Odd slack truncates like the integer cell math it mirrors, and
        // never exceeds the visible left margin by rounding up.
        assert(gridTopPad(32, 355, 314) == 52);

        // Degenerate: a cell narrower than its art cannot push the row off
        // the top of the page.
        assert(gridTopPad(32, 300, 314) <= 32);
    }
```

- [ ] **Step 3: Run it and watch it fail**

```bash
scripts/linux/build.sh --debug
```

Expected: **compile error**, `gridTopPad` not declared. That is the failure.

- [ ] **Step 4: Add the pure function**

In `gui/src/ui_metrics.hh`, after the `UiMetrics` struct:

```cpp
// The album grid's TOP pad, derived from its horizontal pad and the cell's
// own centering slack — never authored independently.
//
// It lives here, beside space()/stroke(), because it is the same kind of
// thing: resolution-robust geometry derived from one place rather than
// hand-tuned per screen. Pure and header-only so ui_metrics_test can assert
// it without linking a Canvas (same reason ui_icons.cc is split from
// ui_icons_draw.cc).
//
// Clamped at 0 slack: a cell narrower than its art is degenerate, and a
// negative pad would push the first row off the top of the page.
inline int gridTopPad(int padXpx, int cellStepX, int artSize) {
    const int slack = cellStepX - artSize;
    return padXpx + (slack > 0 ? slack / 2 : 0);
}
```

- [ ] **Step 5: Run the test and watch it pass**

```bash
scripts/linux/build.sh --debug
./build/linux_debug/gui/ui_metrics_test && echo METRICS-OK
```

Expected: `METRICS-OK`.

- [ ] **Step 6: Resolve the pads in `recalcLayout()`**

In `gui/src/player_view.cc`, replace line `:1897`:

```cpp
    int gridW = rcGrid_.right - rcGrid_.left - (int)metrics_.space((float)gridPadX_) * 2;
```

with:

```cpp
    gridPadXpx_ = (int)metrics_.space((float)gridPadX_);
    int gridW = rcGrid_.right - rcGrid_.left - gridPadXpx_ * 2;
```

Then, immediately after `gridTileSize_ = gridArtSize_ + artMargin;` (`:1909`), insert:

```cpp
    // Cell stride, resolved once. The draw block and gridHitTest() both used
    // to recompute this from raw pads and disagree with the line above.
    gridStepX_ = gridCols_ > 1 ? gridW / gridCols_ : gridTileSize_;

    // Derived, not authored — see gridTopPad()'s comment in ui_metrics.hh.
    gridPadYpx_ = gridTopPad(gridPadXpx_, gridStepX_, gridArtSize_);
```


- [ ] **Step 7: Point the grid total height at the resolved pad**

Replace line `:1920`:

```cpp
    gridTotalHeight_ = albumRows * (gridTileSize_ + gridRowGap_) + (int)metrics_.space((float)gridPadY_);
```

with:

```cpp
    gridTotalHeight_ = albumRows * (gridTileSize_ + gridRowGap_) + gridPadYpx_;
```

- [ ] **Step 8: Point the draw block at the resolved values**

Replace `:1012-1013`:

```cpp
            int tileSpaceW = rcGrid_.right - rcGrid_.left - gridPadX_ * 2;
            int tileStepX = gridCols_ > 1 ? tileSpaceW / gridCols_ : gridTileSize_;
```

with:

```cpp
            int tileStepX = gridStepX_;   // resolved in recalcLayout()
```

Then replace `:1026-1027`:

```cpp
                    float x = (float)(rcGrid_.left + gridPadX_ + col * tileStepX + (tileStepX - gridArtSize_) / 2);
                    float y = (float)(rcGrid_.top + gridPadY_ + row * tileStepY - gridScrollY_);
```

with:

```cpp
                    float x = (float)(rcGrid_.left + gridPadXpx_ + col * tileStepX + (tileStepX - gridArtSize_) / 2);
                    float y = (float)(rcGrid_.top + gridPadYpx_ + row * tileStepY - gridScrollY_);
```

- [ ] **Step 9: Point the hit-test at the same values**

Replace `:2487-2493`:

```cpp
    int gridW = rcGrid_.right - rcGrid_.left;
    int tileSpaceW = gridW - gridPadX_ * 2;
    int tileStepX = gridCols_ > 1 ? tileSpaceW / gridCols_ : gridTileSize_;
    int tileStepY = gridTileSize_ + gridRowGap_;

    int col = (x - rcGrid_.left - gridPadX_) / tileStepX;
    int row = (y - rcGrid_.top - gridPadY_ + gridScrollY_) / tileStepY;
```

with:

```cpp
    int tileStepX = gridStepX_;
    int tileStepY = gridTileSize_ + gridRowGap_;

    int col = (x - rcGrid_.left - gridPadXpx_) / tileStepX;
    int row = (y - rcGrid_.top - gridPadYpx_ + gridScrollY_) / tileStepY;
```

Then replace `:2501-2502`:

```cpp
    int artX = rcGrid_.left + gridPadX_ + col * tileStepX + (tileStepX - gridArtSize_) / 2;
    int artY = rcGrid_.top + gridPadY_ + row * tileStepY - gridScrollY_;
```

with:

```cpp
    int artX = rcGrid_.left + gridPadXpx_ + col * tileStepX + (tileStepX - gridArtSize_) / 2;
    int artY = rcGrid_.top + gridPadYpx_ + row * tileStepY - gridScrollY_;
```

- [ ] **Step 10: Verify no raw use survives**

```bash
grep -n "gridPadX_\|gridPadY_" gui/src/player_view.cc
```

Expected: exactly two hits, both inside `recalcLayout()` (the `space()` call and the derivation). Any other hit is a site that was missed.

- [ ] **Step 11: Build and capture both resolutions**

```bash
scripts/linux/build.sh --debug
./build/linux_debug/gui/ui_metrics_test && echo METRICS-OK
cd build/linux_debug/gui
./matrix_ui_capture --out /tmp/t1-1440 --frame 2560x1440 --only grid
./matrix_ui_capture --out /tmp/t1-1080 --frame 1920x1080 --only grid
```

Look at `10-grid-albums.png` in both. Expected: the top margin above the first row now visually matches the left margin before the first column, at **both** sizes.

- [ ] **Step 12: Verify hit-testing by hand — this is the risk of the task**

Layout, draw and hit-test disagreed before this change; now they agree, which means every tile's clickable box moved. A screenshot cannot catch a click landing one tile off.

```bash
./build/linux_debug/gui/matrix_player
```

Click the **first** tile of row 1, the **last** tile of row 1, and a tile in row 3. Each must open the album whose art you clicked. Then click the gap *between* two tiles and the text block below a tile — both must do nothing (only the artwork is a target).

- [ ] **Step 13: Commit**

```bash
./git_wrapper commit "Grid: one padding for layout, draw and hit-test; derive the top margin"
```

---

### Task 2: Cap the track list's width

**Files:**
- Modify: `gui/src/player_view.cc:1223`

**Interfaces:**
- Consumes: `metrics_.space()`, `tp` (track panel Rect), `colX`.
- Produces: nothing new. `colW` keeps its name and meaning; only its value is bounded.

**The defect:** `colW = tp.x + tp.w - pad - colX` takes the whole remaining window, so at 2560 px a track title sits ~1045 px from its own duration.

§8.4 already guarantees this is a one-line fix — *"One rectangle governs the row"*: `rowX`/`rowW` (`:1277-1278`) feed the hover fill, the playing bar, the disc-separator rules, the duration column and the hit-test rectangle, all derived from `colX`/`colW`.

- [ ] **Step 1: Bound `colW`**

Replace `:1223`:

```cpp
            float colW = tp.x + tp.w - pad - colX;
```

with:

```cpp
            // Capped, not "whatever is left". Unbounded, a 2560px window put a
            // track title ~1045px from its own duration and the eye had to
            // cross the screen to pair them. The cap is a reading measure, so
            // it scales like type rather than like the window: past it, extra
            // width buys nothing and costs the pairing.
            float colW = std::min(tp.x + tp.w - pad - colX, metrics_.space(820.0f));
```

- [ ] **Step 2: Build and capture**

```bash
scripts/linux/build.sh --debug
cd build/linux_debug/gui
./matrix_ui_capture --out /tmp/t2-1440 --frame 2560x1440 --only album-view
./matrix_ui_capture --out /tmp/t2-1080 --frame 1920x1080 --only album-view
```

Look at `20-album-view.png`. Expected: title and duration read as one row.

**The cap binds at both resolutions, and that is deliberate.** An earlier draft of
this plan used `space(1180.0f)` and did nothing at all: the cap passes through
`space()`, so it grows with the window exactly as fast as the content it was
meant to bound, and both test resolutions are 16:9 — in authored units their
geometry is identical, `colW` resolving to ~925 either way, well under 1180.
A reading measure scaling with the type is right; the NUMBER has to sit below
the uncapped width or the code is decoration. 820 is ~11% under it.

- [ ] **Step 3: Verify the row rectangle followed**

```bash
./build/linux_debug/gui/matrix_player
```

Open an album. Hover a track row: the grey pill must end where the duration column ends, not short of it or past it. Click a row near its **right** edge, just left of the duration — it must still select. This confirms `rowX`/`rowW` and the hit-test moved together, as §8.4 promises.

- [ ] **Step 4: Commit**

```bash
./git_wrapper commit "Album view: cap the track list's reading measure"
```

---

### Task 3: Put the EQ panel's primary button where the other three keep theirs

**Files:**
- Modify: `gui/src/player_view.cc:3845-3870`

**Interfaces:**
- Consumes: `content`, `pad`, `btnH`, `metrics_.space()`.
- Produces: nothing new. `eqBtnAssign_`, `eqBtnPin_`, `eqBtnRemove_`, `eqBtnClear_` keep their names and their existing click handlers.

**The defect:** three panels put the primary on the right (`content.right - pad - btnW`: Manage Folders `:3400`, Audio Settings `:3620`, Folder Picker `:3954`); EQ puts it at `btnAt(0)`, the far left. Mouse muscle memory is learned per-app, and a primary that changes sides between sibling pages cannot be learned.

- [ ] **Step 1: Anchor the row from the right**

Replace the `btnAt` lambda and both branches at `:3845-3870`:

```cpp
    float btnW = metrics_.space(277.0f);
    float gap  = metrics_.space(SP_MD);
    int by = (int)(content.bottom - (btnH + pad));
    auto btnAt = [&](int slot) -> LayoutRect {
        float x0 = content.left + pad + slot * (btnW + gap);
        return { (int)x0, by, (int)(x0 + btnW), (int)(by + btnH) };
    };
```

with:

```cpp
    float btnW = metrics_.space(277.0f);
    float gap  = metrics_.space(SP_MD);
    int by = (int)(content.bottom - (btnH + pad));
    // Slot 0 is the PRIMARY and sits hard right, matching Manage Folders,
    // Audio Settings and the folder picker. Secondaries fill leftward. This
    // panel used to lay out left-to-right, so it was the one page of four
    // where the green button changed sides.
    auto btnAt = [&](int slot) -> LayoutRect {
        float x1 = content.right - pad - slot * (btnW + gap);
        return { (int)(x1 - btnW), by, (int)x1, (int)(by + btnH) };
    };
```

Both existing branches (`eqShowMine_` true/false) keep their `btnAt(0)`/`btnAt(1)`/`btnAt(2)` calls and their `drawButton` calls **unchanged** — slot 0 is still the primary, it simply resolves to the right edge now.

- [ ] **Step 2: Build and capture both EQ tabs**

```bash
scripts/linux/build.sh --debug
cd build/linux_debug/gui
./matrix_ui_capture --out /tmp/t3 --frame 2560x1440 --only eq
```

Look at `33-eq-settings.png` (My Headphones: Select / Pin / Remove) and `34-eq-all-profiles.png` (All Profiles: Assign to Device / Clear). Expected in both: the green button is the rightmost, secondaries to its left, and the row does not run off either edge.

- [ ] **Step 3: Verify the clicks still land**

```bash
./build/linux_debug/gui/matrix_player
```

Settings → EQ / AutoEQ Profiles. Click **Pin** and confirm the row's `pinned` marker toggles. Switch to All Profiles, select a row, click **Clear**. The handlers hit-test the same rects the draw wrote, so this should pass — but a slot the draw assigns and the handler does not read is exactly the failure this step catches.

- [ ] **Step 4: Commit**

```bash
./git_wrapper commit "EQ panel: primary button on the right, matching the other three panels"
```

---

### Task 4: Stop the panel button rows from stranding themselves

**Files:**
- Modify: `gui/src/player_view.cc:3398` (Manage Folders), `:3619` (Audio Settings), `:3847` (EQ), `:3952` (Folder Picker)

**Interfaces:**
- Consumes: `content`, `pad`, `btnH`, and each panel's content extent.
- Produces: `PlayerWindow::panelButtonRowY(const LayoutRect& content, float contentBottomPx, float btnH, float pad) const` — a private helper, declared in `player_view.hh`, used by all four panels.

**The defect:** all four anchor to `content.bottom - (btnH + pad)`. With one folder in the list that leaves ~900 px between the content and the controls that act on it.

- [ ] **Step 1: Declare the helper**

In `gui/src/player_view.hh`, in the private section near the other panel helpers:

```cpp
    // Where a settings panel's button row sits. Anchored to the BOTTOM of the
    // page once the content is tall enough to reach it, and tucked just below
    // the content when it is not — a Done button 900px under a one-row list
    // reads as belonging to the window rather than to the list. One helper so
    // the four panels cannot drift apart again.
    int panelButtonRowY(const LayoutRect& content, float contentBottomPx,
                        float btnH, float pad) const;
```

- [ ] **Step 2: Define it**

In `gui/src/player_view.cc`, immediately before `drawManageFolders`:

```cpp
int PlayerWindow::panelButtonRowY(const LayoutRect& content, float contentBottomPx,
                                  float btnH, float pad) const {
    const int floorY  = (int)(content.bottom - (btnH + pad));
    const int huggedY = (int)(contentBottomPx + pad);
    return std::min(floorY, huggedY);
}
```

- [ ] **Step 3: Use it in Manage Folders**

Replace `:3398`:

```cpp
    int by = (int)(content.bottom - (btnH + pad));
```

with:

```cpp
    int by = panelButtonRowY(content, (float)listArea.top + (float)mfRoots_.size() * kPanelRowH,
                             btnH, pad);
```

- [ ] **Step 4: Use it in the Folder Picker**

Replace `:3952`:

```cpp
    int by = (int)(content.bottom - (btnH + pad));
```

with:

```cpp
    int by = panelButtonRowY(content, (float)listArea.top + (float)labels.size() * kPanelRowH,
                             btnH, pad);
```

- [ ] **Step 5: Use it in the EQ panel**

Replace `:3847`:

```cpp
    int by = (int)(content.bottom - (btnH + pad));
```

with:

```cpp
    int by = panelButtonRowY(content, (float)eqListArea_.top + (float)eqListRows_.size() * kPanelRowH,
                             btnH, pad);
```

- [ ] **Step 6: Use it in Audio Settings**

Audio Settings has no list variable to measure — its content is a radio group plus a device list, and `y` already tracks the running bottom as it draws. Replace `:3619`:

```cpp
    int by = (int)(content.bottom - (btnH + pad));
```

with:

```cpp
    int by = panelButtonRowY(content, y, btnH, pad);
```

Confirm before building that `y` is in scope at that line and holds the bottom of the last drawn block (`grep -n "y += listH" gui/src/player_view.cc`). If it is not, use `content.bottom` for this panel only and note it — Audio Settings' content nearly fills the page anyway, so it is the panel this task helps least.

- [ ] **Step 7: Build and capture all four panels**

```bash
scripts/linux/build.sh --debug
cd build/linux_debug/gui
./matrix_ui_capture --out /tmp/t4 --frame 2560x1440
```

Look at `31-manage-folders.png`, `32-audio-settings.png`, `33-eq-settings.png`, `35-folder-picker.png`. Expected: with a short list, the buttons sit just under it. Expected in `34-eq-all-profiles.png`: the list is long (8600 profiles), so the row stays pinned to the bottom exactly as before — **if that shot changed, the helper is wrong.**

- [ ] **Step 8: Verify the buttons still respond where they moved to**

```bash
./build/linux_debug/gui/matrix_player
```

Settings → Manage Music Folders. Click **Done** at its new position; the panel must close.

- [ ] **Step 9: Commit**

```bash
./git_wrapper commit "Settings panels: button row hugs short content instead of stranding at the page bottom"
```

---

### Task 5: Give the transport's right cluster its own margin

**Files:**
- Modify: `gui/src/player_view.cc:1712`

**Interfaces:**
- Consumes: `t` (transport Rect), `metrics_.space()`.
- Produces: nothing new. `rightEdge` keeps its name; `rcDspBadge_` continues to derive from it.

**The defect:** `rightEdge = t.x + t.w - metrics_.space(16.0f)` while the gap *inside* the cluster is `metrics_.space(24.0f)` (`:1762`). The cluster is held closer to the window edge than its own two parts are held apart, so `0:00 / 0:00  BITPERFECT` reads as one run of text rather than as a reading and a state.

Note: the clock uses `metrics_.text.secondary` and the badge `metrics_.text.caption`, which are **the same size on purpose** (`ui_metrics.hh` — they are separated by the color ladder and by style, not by size). Do not "fix" that; it is the design.

- [ ] **Step 1: Widen the outer margin past the inner gap**

Replace `:1712`:

```cpp
            float rightEdge = t.x + t.w - metrics_.space(16.0f);
```

with:

```cpp
            // The outer margin must exceed the gap INSIDE the cluster
            // (space(24) between clock and badge, below), or the reading and
            // the state read as one string pushed against the window edge.
            float rightEdge = t.x + t.w - metrics_.space(SP_LG);
```

`SP_LG` is 32, comfortably past the 24 inner gap.

- [ ] **Step 2: Build and capture**

```bash
scripts/linux/build.sh --debug
cd build/linux_debug/gui
./matrix_ui_capture --out /tmp/t5-1440 --frame 2560x1440 --only grid-albums
./matrix_ui_capture --out /tmp/t5-1080 --frame 1920x1080 --only grid-albums
```

Look at the bottom-right of `10-grid-albums.png`. Expected: `BITPERFECT` no longer touches the window edge, and the clock reads as a separate group.

- [ ] **Step 3: Verify the badge's hover rect moved with it**

`rcDspBadge_` (`:1717-1718`) is not decoration — it is the hit rect for the expanded signal-path readout, and it derives from `rightEdge`, so it should have followed automatically. Confirm it did:

```bash
./build/linux_debug/gui/matrix_player
```

Hover the `BITPERFECT` badge. The compact tag must swap for the full `source » DSP » backend` readout, and swap back on leaving. If the readout only appears when hovering slightly left of the badge, the rect did not follow.

- [ ] **Step 4: Commit**

```bash
./git_wrapper commit "Transport: outer margin exceeds the cluster's own gap"
```

---

### Task 6: Machine text in Mono, human text in serif

**Files:**
- Modify: `gui/src/player_view.cc:3750` (EQ device key), `:3926-3927` (folder picker's current path)

**Interfaces:**
- Consumes: `FontStyle::Math` — the Mono slot, already baked into the atlas and already used for track numbers, durations and the transport clock.
- Produces: nothing new.

**Why this is safe:** Mono's legibility floor is **9.14 px** against the Regular serif's **18.29 px** (the binding constraint for `kMinReadableTextSizePx`). Text moving to Mono moves onto the face with the most headroom of the four. **No size changes** — style only.

**Scope note:** this task reaches only the two strings drawn directly. The folder *rows* in Manage Folders and the Folder Picker go through `widgets::drawScrollList`, whose `ScrollListStyle` has no font-style field (`framework/vk_canvas/core/widgets.hh:178-190`). Those are Task 8, which is optional and touches the submodule.

- [ ] **Step 1: The EQ device identifier**

Replace `:3750`:

```cpp
    canvas.textStyled("Device: " + eqDeviceKey_, c.x + pad, y, metrics_.text.secondary, toColor(CLR_TEXT_DIM), FontStyle::Roman);
```

with:

```cpp
    // Mono, because "32BB:0004" is an identifier, not a name. The family
    // already carries a face that says so, and it is the one with the most
    // legibility headroom (9.14px floor vs the serif's 18.29px). Headphone
    // and album names stay serif — the rule is about what the string IS.
    canvas.textStyled("Device: " + eqDeviceKey_, c.x + pad, y, metrics_.text.secondary, toColor(CLR_TEXT_DIM), FontStyle::Math);
```

- [ ] **Step 2: The folder picker's current path**

Replace `:3926-3927`:

```cpp
    canvas.textStyled(truncateToWidth(canvas, fpCurrentDir_, c.w - 2.0f * pad, metrics_.text.secondary, FontStyle::Roman),
                      c.x + pad, c.y + pad, metrics_.text.secondary, toColor(CLR_TEXT_DIM), FontStyle::Roman);
```

with:

```cpp
    // A filesystem path is machine text (see the EQ device line) — and the
    // measure passed to truncateToWidth must use the SAME style it is drawn
    // in, or the ellipsis lands at the wrong character.
    canvas.textStyled(truncateToWidth(canvas, fpCurrentDir_, c.w - 2.0f * pad, metrics_.text.secondary, FontStyle::Math),
                      c.x + pad, c.y + pad, metrics_.text.secondary, toColor(CLR_TEXT_DIM), FontStyle::Math);
```

- [ ] **Step 3: Build and capture**

```bash
scripts/linux/build.sh --debug
cd build/linux_debug/gui
./matrix_ui_capture --out /tmp/t6 --frame 2560x1440 --only 3
```

Look at `33-eq-settings.png` (`Device: 32BB:0004` now monospaced) and `35-folder-picker.png` (`/home/nava` now monospaced, its subfolder rows still serif — that difference is expected until Task 8).

- [ ] **Step 4: Verify against a long path**

```bash
./build/linux_debug/gui/matrix_player
```

Settings → Add Music Folder, then navigate several levels deep until the path is long enough to ellipsize. The truncation must land inside the visible box, not overflow it — this is what Step 2's matched-style `truncateToWidth` guarantees, and a mismatched measure shows up only here.

- [ ] **Step 5: Commit**

```bash
./git_wrapper commit "Panels: filesystem paths and device identifiers set in Mono"
```

---

### Task 7: Update the design system to match

**Files:**
- Modify: `docs/UI_DESIGN_SYSTEM.md` §8.2, §8.4, §8.6

`UI_DESIGN_SYSTEM.md` is described in `CLAUDE.md` as the single source of truth, meant to be updated alongside the code rather than left to drift. Six changes just landed that it does not describe.

- [ ] **Step 1: §8.2 (album grid)** — add that the grid's horizontal pad is authored and the vertical pad is *derived* from it plus the cell's centering slack, so the two margins are optically equal by construction, and that layout, drawing and hit-testing all read the resolved `gridPadXpx_`/`gridPadYpx_`/`gridStepX_`.

- [ ] **Step 2: §8.4 (track panel)** — add that the track list has a capped reading measure, and that the cap propagates through `rowX`/`rowW` to the hover fill, the playing bar, the separators and the hit-test, which the section already documents as one rectangle.

- [ ] **Step 3: §8.6 (settings)** — add two rules: the primary button is right-anchored in all four panels, and the button row hugs short content instead of pinning to the page bottom. Add the typography rule: machine text (paths, device identifiers) is Mono, names stay serif.

- [ ] **Step 4: Commit**

```bash
./git_wrapper commit "Design system: document the margins and typography pass"
```

---

### Task 8 (OPTIONAL — touches the vk_canvas submodule): row text style

**Files:**
- Modify: `framework/vk_canvas/core/widgets.hh:178-190`, `framework/vk_canvas/core/widgets.cc` (`drawScrollList`)
- Modify: `gui/src/player_view.cc:136-148` (`matrixListStyle`), `:3389`, `:3943`

**Do not start this task without asking.** `framework/vk_canvas` is an independently developed submodule with its own `CLAUDE.md`; changing it is a decision about that library's API, not about this app. It is written up here so the option is concrete, not so it gets done by default.

**Why it exists:** Task 6 could not reach the folder *rows* in Manage Folders and the Folder Picker, because `drawScrollList` gives its caller no say over row font style. So a path header is Mono while the paths under it are serif — visibly half-finished.

- [ ] **Step 1: Add the field**

In `framework/vk_canvas/core/widgets.hh`, inside `struct ScrollListStyle`, after `bool fitWidth = false;`:

```cpp
  FontStyle rowStyle = FontStyle::Roman;  // row text face; Roman keeps every existing caller identical
```

- [ ] **Step 2: Honor it in `drawScrollList`** — pass `style.rowStyle` to the text draw and to whatever measurement `TextFit` performs. Both must use the same style or the fit will be computed against a face the row is not drawn in.

- [ ] **Step 3: Opt in from the two path lists only.** `matrixListStyle()` must keep its serif default (the EQ profile list and the audio device list carry *names*). Add a second, narrow style function in `player_view.cc`:

```cpp
// Same list styling, Mono rows — for lists whose rows are filesystem paths.
static widgets::ScrollListStyle matrixPathListStyle() {
    widgets::ScrollListStyle s = matrixListStyle();
    s.rowStyle = FontStyle::Math;
    return s;
}
```

and pass it at `:3389` (Manage Folders) and `:3943` (Folder Picker) in place of `matrixListStyle()`.

- [ ] **Step 4: Verify no other consumer changed.** The default is `Roman`, so every existing call site keeps its current rendering. Confirm the EQ profile list and the audio device list are still serif in `/tmp/t8/33-eq-settings.png` and `32-audio-settings.png`.

- [ ] **Step 5: Commit — submodule first**

```bash
./git_wrapper commit "ScrollListStyle: caller-selectable row font style"
./git_wrapper push    # pushes the submodule before the parent
```

---

## Self-review

**Spec coverage:** A1→Task 1, A2→Task 2, A3→Task 3, A4→Task 4, A5→Task 5, B1→Task 6 (+ Task 8 for the part the framework blocks). The spec's verification section → the loop at the top plus per-task steps. The spec's "Deferred" list is intentionally not implemented.

**Corrections made against the spec while writing this plan:**
- A5 in the spec said the clock and badge share a size and implied that was the defect. They share it **deliberately** (`ui_metrics.hh`). The actual defect is the outer margin (16) being smaller than the inner gap (24). Task 5 fixes that and explicitly says not to touch the sizes.
- B1 in the spec assumed all path text was reachable. Two of the four sites go through a framework widget with no font-style hook. Split into Task 6 (reachable) and Task 8 (optional, submodule).
- A1 in the spec said "raise the vertical pad until it optically matches". Task 1 derives it from the cell slack instead, so it is provably equal and stays equal across resizes.

**Placeholder scan:** no TBDs. Every code step carries the real before/after. Task 4 Step 6 carries a conditional (`y` in scope) with a stated fallback rather than an assumption. Task 2's cap value is stated as a starting number with an explicit "raise it if the 1080 shot moved" check, because a reading measure is judged by eye, not derived.

**Type consistency:** `gridPadXpx_`/`gridPadYpx_`/`gridStepX_` are declared in Task 1 Step 1 and used under those names in Steps 2-6 and nowhere else. `panelButtonRowY` is declared in Task 4 Step 1 with the signature used in Steps 3-6. `matrixPathListStyle` (Task 8) builds on `matrixListStyle`, which exists at `:136`.

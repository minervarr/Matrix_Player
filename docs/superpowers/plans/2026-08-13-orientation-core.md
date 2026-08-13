# Orientation Core Implementation Plan (phase 1 of 5)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace `UiMode{Essential, Complete}` — a window size asked of the OS — with `Orientation{Horizontal, Vertical}` derived from the window's own shape, and delete Essential mode entirely.

**Architecture:** A new pure header/source pair (`gui/src/orientation.hh/.cc`) owns the whole decision: derive an orientation from a width and a height, and resolve it against a manual override. It links nothing — no Canvas, no Host, no Db — so its test runs as plain arithmetic, the same convention as `ui_metrics_test`. `PlayerWindow` holds one `OrientationState` and consults it in `recalcLayout()`. Nothing about the *layout itself* changes in this phase: after it lands the app looks exactly as it does today, Essential mode is gone, Alt+L flips an orientation that no drawing code reads yet, and `Host` no longer knows what a UI mode is. Phase 2 gives the orientation something to do.

**Tech Stack:** C++17, CMake + Ninja, assert-based Debug-only tests (`#undef NDEBUG`, no framework), vk_canvas `Canvas`, SQLite via `core/db.h`.

**Spec:** `docs/superpowers/specs/2026-08-12-orientation-modes-design.md`

## Global Constraints

- **Bar thickness is `space(130.0f)`** — the current transport bar's height, `gui/src/player_view.cc:2247`. Not a new constant. (Phase 2 consumes this; phase 1 must not change it.)
- **Horizontal is Vertical rotated 90° counter-clockwise.** Any position expressed in one orientation must be derivable from the other by that single transform.
- **`Canvas::image()` does not honour `setRotation()`** (`framework/vk_canvas/core/canvas.hh:124`). Text does (`:175`). No design may depend on a rotated bitmap.
- **The window is fixed and non-resizable** (`gui/src/player_view.cc:198`). Automatic orientation therefore follows the *monitor's* shape on desktop, and the real window resize on Android. The manual override is what covers a listener who wants the other layout on the monitor they have.
- **Tests are assert-based, Debug-only, `#undef NDEBUG` at the top of the test source, no framework, no engine linkage.** Convention: `gui/src/ui_metrics_test.cc`.
- **Never `git commit`/`git push` directly** — use `git_wrapper` (`USAGE_gitWrapper.md`). NOTE: only `git_wrapper.exe` (Windows) is present in this checkout; on Linux, stop and ask the human rather than falling back to plain `git`.
- **`core/` takes zero OS headers.** Nothing in this plan adds any; `orientation.*` lives in `gui/src/`, not `core/`, because it is a presentation concern. Phase 5 revisits this when the presentation layer moves.

---

### Task 1: The orientation decision, as pure arithmetic

**Files:**
- Create: `gui/src/orientation.hh`
- Create: `gui/src/orientation.cc`
- Create: `gui/src/orientation_test.cc`
- Modify: `gui/CMakeLists.txt:249-258` (add a test target beside `ui_metrics_test`)

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `enum class Orientation { Horizontal, Vertical };`
  - `Orientation autoOrientationFor(int windowW, int windowH);`
  - `struct OrientationState` with fields `bool autoEnabled`, `Orientation manual`, and methods `Orientation resolve(int w, int h) const;` and `void toggleManual(int w, int h);`

- [ ] **Step 1: Write the failing test**

Create `gui/src/orientation_test.cc`:

```cpp
// Asserts must stay live even though the app builds Release (NDEBUG).
#undef NDEBUG
#include <cassert>
#include <cstdio>

#include "orientation.hh"

int main() {
    // ── A wider-than-tall window is Horizontal; taller-than-wide is Vertical ──
    assert(autoOrientationFor(1920, 1080) == Orientation::Horizontal);
    assert(autoOrientationFor(1080, 1920) == Orientation::Vertical);
    assert(autoOrientationFor(2560, 1440) == Orientation::Horizontal);

    // A phone in portrait and the same phone in landscape are the two cases
    // Android delivers as ordinary resizes — no sensor is consulted.
    assert(autoOrientationFor(1080, 2400) == Orientation::Vertical);
    assert(autoOrientationFor(2400, 1080) == Orientation::Horizontal);

    // ── An exactly square window is Horizontal, deliberately ──
    // Something must win, and Horizontal is the layout every existing monitor
    // and every existing capture already assumes. Ties never flip.
    assert(autoOrientationFor(1000, 1000) == Orientation::Horizontal);

    // ── Degenerate sizes never crash and never flip ──
    // A Wayland surface can be configured 0x0 before the first real configure
    // arrives; the answer must be defined rather than accidental.
    assert(autoOrientationFor(0, 0) == Orientation::Horizontal);
    assert(autoOrientationFor(0, 500) == Orientation::Vertical);
    assert(autoOrientationFor(500, 0) == Orientation::Horizontal);

    // ── Auto state follows the window ──
    OrientationState st;                       // auto is the default
    assert(st.autoEnabled);
    assert(st.resolve(1920, 1080) == Orientation::Horizontal);
    assert(st.resolve(1080, 1920) == Orientation::Vertical);

    // ── Toggling takes manual control, and STICKS ──
    // The listener asked for the other layout; a later resize must not
    // silently take it back, which is the whole point of the override.
    st.toggleManual(1920, 1080);
    assert(!st.autoEnabled);
    assert(st.resolve(1920, 1080) == Orientation::Vertical);
    assert(st.resolve(1080, 1920) == Orientation::Vertical);

    // ── Toggling again flips the manual choice, still manual ──
    st.toggleManual(1920, 1080);
    assert(!st.autoEnabled);
    assert(st.resolve(1080, 1920) == Orientation::Horizontal);

    // ── The first toggle flips away from what auto WOULD have said ──
    // Not from a stored default: pressing the key on a vertical monitor must
    // give Horizontal, otherwise the first press appears to do nothing.
    OrientationState st2;
    st2.toggleManual(1080, 1920);              // auto here would say Vertical
    assert(st2.resolve(1080, 1920) == Orientation::Horizontal);

    // ── Re-enabling auto discards the manual choice ──
    st2.autoEnabled = true;
    assert(st2.resolve(1080, 1920) == Orientation::Vertical);

    printf("orientation_test: all assertions passed\n");
    return 0;
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run:
```bash
scripts/linux/build.sh --debug
```
Expected: FAIL at configure or compile time — `orientation_test` is not yet a target, and `orientation.hh` does not exist. This is the expected failure; do not proceed to step 4 without seeing it.

- [ ] **Step 3: Write the minimal implementation**

Create `gui/src/orientation.hh`:

```cpp
#pragma once

// Which of the two layouts the UI draws. NOT a window size: this is derived
// from the shape the window ALREADY has, never asked of the OS. That is the
// whole reason it works identically on Wayland (where a client cannot
// position or size itself), on Windows, and on Android (where a device
// rotation arrives as an ordinary resize).
//
// Replaces UiMode{Essential, Complete}, which was a window rectangle wearing
// a mode's name -- see the spec, and the dead no-ops it left in
// os/linux_host.cc.
//
// Horizontal is Vertical rotated 90 degrees counter-clockwise. There are not
// two layouts; there is one layout and a rotation.
enum class Orientation { Horizontal, Vertical };

// The automatic answer for a window of this size.
//
// A tie (w == h) resolves to Horizontal deliberately: something has to win,
// and Horizontal is what every existing monitor, capture and screenshot
// already assumes. Degenerate sizes (a Wayland surface configured 0x0 before
// its first real configure) fall out of the same comparison rather than
// needing a guard -- the answer is defined, not accidental.
Orientation autoOrientationFor(int windowW, int windowH);

// Automatic by default, with a manual override that sticks.
//
// `manual` is meaningless while `autoEnabled` is true, and toggleManual()
// seeds it from what auto WOULD have said. Without that seeding the first
// key press on a vertical monitor would appear to do nothing.
struct OrientationState {
    bool        autoEnabled = true;
    Orientation manual      = Orientation::Horizontal;

    Orientation resolve(int windowW, int windowH) const;

    // Leaves automatic mode (if it was on) and flips to the other layout.
    void toggleManual(int windowW, int windowH);
};
```

Create `gui/src/orientation.cc`:

```cpp
#include "orientation.hh"

Orientation autoOrientationFor(int windowW, int windowH) {
    return (windowH > windowW) ? Orientation::Vertical : Orientation::Horizontal;
}

Orientation OrientationState::resolve(int windowW, int windowH) const {
    return autoEnabled ? autoOrientationFor(windowW, windowH) : manual;
}

void OrientationState::toggleManual(int windowW, int windowH) {
    const Orientation current = resolve(windowW, windowH);
    manual = (current == Orientation::Horizontal) ? Orientation::Vertical
                                                  : Orientation::Horizontal;
    autoEnabled = false;
}
```

Add the test target to `gui/CMakeLists.txt`, immediately after the `ui_metrics_test` block (which ends at line 258 with its `endif()` for `NOT WIN32`) and before the `ui_icons_test` comment at line 260:

```cmake
    # orientation_test links the REAL src/orientation.cc rather than restating
    # its arithmetic. orientation.hh includes nothing at all -- no Canvas, no
    # Host, no theme -- which is what keeps this a pure-logic test and what
    # will let phase 5 hand the same file to Android untouched.
    add_executable(orientation_test src/orientation_test.cc src/orientation.cc)
    target_include_directories(orientation_test PRIVATE src)
    if(NOT WIN32)
        target_compile_options(orientation_test PRIVATE -Wall)
    endif()
```

- [ ] **Step 4: Run the test to verify it passes**

Run:
```bash
scripts/linux/build.sh --debug && ./build/linux_debug/gui/orientation_test
```
Expected: `orientation_test: all assertions passed`, exit code 0.

- [ ] **Step 5: Verify the existing tests still pass**

Run:
```bash
./build/linux_debug/gui/ui_metrics_test && \
./build/linux_debug/gui/ui_icons_test && \
./build/linux_debug/gui/ui_text_test && \
./build/linux_debug/core/variants_test && \
./build/linux_debug/core/stats_test && \
./build/linux_debug/core/facets_test
```
Expected: every one exits 0. This task adds a file and a target and touches nothing else, so any failure here is a build-environment problem, not a regression — stop and report it rather than editing around it.

- [ ] **Step 6: Commit**

```bash
./git_wrapper commit -m "Orientation: derive Horizontal/Vertical from window shape

A pure header/source pair plus its assert-based test. Nothing consumes it
yet -- UiMode is still in place and this changes no pixels." -- \
  gui/src/orientation.hh gui/src/orientation.cc gui/src/orientation_test.cc \
  gui/CMakeLists.txt
```

If `./git_wrapper` does not exist on this machine (only `git_wrapper.exe` is checked in), **stop and ask the human** — CLAUDE.md forbids plain `git commit`.

---

### Task 2: PlayerWindow holds an OrientationState, and Alt+L flips it

**Files:**
- Modify: `gui/src/player_view.hh:338-339` (replace the `UiMode uiMode_` field)
- Modify: `gui/src/player_view.hh` (declare `toggleOrientation()`, remove `toggleUiMode()`)
- Modify: `gui/src/player_view.cc:2448-2451` (`toggleUiMode` → `toggleOrientation`)
- Modify: `gui/src/player_view.cc:6626-6628` (`onHotkey`)
- Modify: `gui/src/hotkey_ids.hh:15` (rename `kHotkeyToggleMode`)
- Modify: `gui/CMakeLists.txt` (add `src/orientation.cc` to the `matrix_player` and `matrix_ui_capture` source lists)

**Interfaces:**
- Consumes: `Orientation`, `OrientationState`, `autoOrientationFor` from Task 1.
- Produces: `PlayerWindow::orientation_` (an `OrientationState`), `PlayerWindow::curOrientation_` (an `Orientation`, recomputed in `recalcLayout()`), and `void PlayerWindow::toggleOrientation();`

- [ ] **Step 1: Add the field and include**

In `gui/src/player_view.hh`, add near the other local includes:

```cpp
#include "orientation.hh"
```

Replace the block at `gui/src/player_view.hh:338-339`:

```cpp
    // UI mode state
    UiMode  uiMode_ = UiMode::Complete;
    // (Essential/Complete toggle is keyboard-only: Alt+L. No on-screen button.)
```

with:

```cpp
    // Orientation state. `orientation_` is the POLICY (auto, or a manual
    // choice that sticks); `curOrientation_` is the ANSWER for the window as
    // it is right now, recomputed once per recalcLayout() so that every
    // drawing and hit-testing site reads one consistent value for the whole
    // frame. Reading resolve() ad hoc from draw code would let two halves of
    // one frame disagree during a resize.
    OrientationState orientation_    = {};
    Orientation      curOrientation_ = Orientation::Horizontal;
```

- [ ] **Step 2: Recompute it in recalcLayout()**

In `gui/src/player_view.cc`, at the top of `recalcLayout()` — immediately before the existing `if (uiMode_ == UiMode::Essential) return;` at line 2238 — insert:

```cpp
    curOrientation_ = orientation_.resolve(W, H);
```

`W` and `H` are the locals `recalcLayout()` already computes at `gui/src/player_view.cc:2158` (`int W = (int)renderer_->width(), H = (int)renderer_->height();`), guarded three lines above by an early `if (!renderer_) return;`. Insert **after** that guard, so the resolve never runs against a null renderer.

- [ ] **Step 3: Replace the toggle**

In `gui/src/player_view.hh`, replace the declaration at line 263:

```cpp
    void toggleUiMode();
```

with:

```cpp
    void toggleOrientation();
```

In `gui/src/player_view.cc`, replace `toggleUiMode()` (lines 2448-2451) with:

```cpp
// Alt+L. Leaves automatic orientation and flips to the other layout; a later
// window resize will NOT take the choice back (see OrientationState). There
// is no host call here on purpose -- orientation is a layout decision, not a
// window size, which is exactly why it works on Wayland where the old
// applyUiMode() path could only ask and wait.
void PlayerWindow::toggleOrientation() {
    // PlayerWindow caches no window size of its own: recalcLayout() reads it
    // straight off the renderer each pass (player_view.cc:2158), and so does
    // this. The null guard matters because a hotkey can in principle arrive
    // before create() finishes constructing the renderer.
    if (!renderer_) return;
    orientation_.toggleManual((int)renderer_->width(), (int)renderer_->height());
    recalcLayout();
    invalidate();
}
```

- [ ] **Step 4: Rewire the hotkey**

In `gui/src/hotkey_ids.hh`, replace line 15:

```cpp
    kHotkeyToggleMode,   // Alt+L: Essential <-> Complete
```

with:

```cpp
    kHotkeyToggleOrientation,  // Alt+L: Horizontal <-> Vertical
```

In `gui/src/player_view.cc:6627`, replace:

```cpp
    if (hotkeyId == kHotkeyToggleMode) toggleUiMode();
```

with:

```cpp
    if (hotkeyId == kHotkeyToggleOrientation) toggleOrientation();
```

Then fix every remaining reference to `kHotkeyToggleMode` — find them with:

```bash
grep -rn "kHotkeyToggleMode\|toggleUiMode" gui/ tools/
```

Expected hits: `gui/src/os/windows_host.cc` and `gui/src/os/linux_host.cc`, which register/dispatch the id. Rename in place; the key (Alt+L) does not change.

- [ ] **Step 5: Add orientation.cc to the app's source lists**

In `gui/CMakeLists.txt`, add `src/orientation.cc` beside `src/ui_metrics.cc` in **both** the `matrix_player` target's source list and the `matrix_ui_capture` target's source list (the latter is visible at the tail of the file, listing `src/ui_metrics.cc` among others). Missing the second one produces a link error in `matrix_ui_capture` only, and only in Debug.

- [ ] **Step 6: Build and verify nothing moved**

Run:
```bash
scripts/linux/build.sh --debug && ./build/linux_debug/gui/orientation_test
```
Expected: builds clean, test passes.

Then run the app and confirm Alt+L **no longer changes the window size**:
```bash
./build/linux_debug/gui/matrix_player
```
Expected: the UI is byte-for-byte what it was before this task (no drawing code reads `curOrientation_` yet), and Alt+L now does nothing visible instead of toggling Essential mode. That "nothing visible" is the correct result for this task — the layout learns to respond in phase 2.

- [ ] **Step 7: Commit**

```bash
./git_wrapper commit -m "PlayerWindow: hold an OrientationState; Alt+L flips orientation

Alt+L no longer resizes the window. Essential mode is now unreachable and
is deleted in the next commit." -- \
  gui/src/player_view.hh gui/src/player_view.cc gui/src/hotkey_ids.hh \
  gui/CMakeLists.txt
```

---

### Task 3: Delete Essential mode

**Files:**
- Modify: `gui/src/player_view.hh:255-266` (remove `essentialHitTest`)
- Modify: `gui/src/player_view.hh:341-347` (remove the Essential layout zones)
- Modify: `gui/src/player_view.hh:60-70` (the `kMinWindowContentH` / `UiMode` comments)
- Modify: `gui/src/player_view.cc` — the Essential branches at lines 1007, 2238, 3159, 3227, 3284, 6817, 6822, and `essentialHitTest()` at 2458-2466
- Modify: `gui/src/player_view.cc:195-215` (`create()`'s mode choice)

**Interfaces:**
- Consumes: `curOrientation_` from Task 2 (only to confirm nothing else needs the removed state).
- Produces: no new symbols. Removes `PlayerWindow::essentialHitTest`, `rcEssentialArt_`, `rcEssentialTitle_`, `rcEssentialPrev_`, `rcEssentialPlayStop_`, `rcEssentialNext_`, `hoverEssentialBtn_`.

- [ ] **Step 1: Find every site**

Run:
```bash
grep -rn "Essential\|essential" gui/ tools/ | grep -v "^gui/src/orientation"
```
Write the list down before editing anything. Every hit is either a branch to delete, a member to delete, or a comment to rewrite — there should be no third category. If you find one, stop and report it.

- [ ] **Step 2: Delete the drawing and hit-testing branches**

For each `if (uiMode_ == UiMode::Essential) { ... }` block in `drawFrame()` and its helpers (`gui/src/player_view.cc:1007`, `:3159`, `:3227`, `:3284`), delete the **whole branch and its body** — this is the minimal now-playing widget, and it has no successor. For the bare guards `if (uiMode_ == UiMode::Essential) return;` (`:2238`) delete the guard line alone, leaving the code after it to run unconditionally.

At `:6817` and `:6822` the guards read `if (uiMode_ == UiMode::Essential) return;   // nothing to go back FROM` inside the nav-back/nav-forward handlers. Delete both guard lines — with Essential gone there is always somewhere to go back from.

Delete `essentialHitTest()` entirely (`gui/src/player_view.cc:2458-2466`) and its declaration (`gui/src/player_view.hh:264`), plus every call site the grep found.

- [ ] **Step 3: Delete the state**

Remove from `gui/src/player_view.hh` (lines 341-347):

```cpp
    // Essential-mode layout zones (see toggleUiMode())
    LayoutRect rcEssentialArt_      = {};
    LayoutRect rcEssentialTitle_    = {};
    LayoutRect rcEssentialPrev_     = {};
    LayoutRect rcEssentialPlayStop_ = {};
    LayoutRect rcEssentialNext_     = {};
    int  hoverEssentialBtn_   = -1;  // 0=prev,1=playStop,2=next
```

Delete every assignment to those members that the grep in Step 1 found (the largest cluster is in `recalcLayout()` just above line 2238).

- [ ] **Step 4: Fix create()'s startup decision**

Replace `gui/src/player_view.cc:195-210` — the block that queries the monitor to pick a mode — with:

```cpp
bool PlayerWindow::create(std::unique_ptr<Host> injectedHost) {
    host_ = injectedHost ? std::move(injectedHost) : make_host();

    // Fixed, non-resizable window: the app sets its size once and never
    // leaves it to interactive resize/maximize. Orientation is therefore
    // derived from the shape that size gives us -- on desktop that is the
    // monitor's own shape (a vertical monitor gets the vertical layout), on
    // Android it is whatever the device reports and follows rotation for
    // free. Alt+L overrides it either way.
    if (!host_->init(this)) return false;
```

Delete the now-unused `MonitorInfo primaryMon` / `monitorH` locals **only if nothing later in `create()` reads them** — grep the rest of the function before deleting, since `primaryMon` may feed window sizing further down. If it is still used, keep the query and delete only the `uiMode_` assignment.

`kMinWindowContentH` stays: it is the font scale's geometric floor (`ui_metrics.hh`), not an Essential-mode value. Rewrite its comment at `gui/src/player_view.hh:60-65` to drop the `UiMode` reference:

```cpp
// The smallest role IS the floor, so the minimum content height at which the
// scale is not clamped is exactly the reference height. Below it the whole
// scale clamps uniformly rather than distorting.
static constexpr float kMinWindowContentH = kUiReferenceHeight;
```

and delete the three-line `// UiMode itself is declared in host.hh ...` comment at `:66-68`.

- [ ] **Step 5: Build and run**

Run:
```bash
scripts/linux/build.sh --debug && ./build/linux_debug/gui/orientation_test && \
./build/linux_debug/gui/matrix_player
```
Expected: builds with no warnings about unused members, and the app opens exactly as before. Click through the sidebar, open an album, open each of the four settings panels, and press Alt+L — nothing should have changed except that Alt+L no longer produces a small window.

- [ ] **Step 6: Verify no Essential references survive**

Run:
```bash
grep -rn "Essential\|essentialHitTest" gui/ tools/
```
Expected: **no output**. Any hit is a leftover; fix it before committing.

- [ ] **Step 7: Commit**

```bash
./git_wrapper commit -m "Delete Essential mode

Its minimal now-playing widget has no successor: the simple variant of each
orientation is a separate design. Removing it first keeps the UiMode removal
in the next commit a pure interface change." -- \
  gui/src/player_view.hh gui/src/player_view.cc
```

---

### Task 4: Remove UiMode from the Host interface

**Files:**
- Modify: `gui/src/host.hh:19-23` (delete `UiMode`), `:71-75` (`init`), `:94-102` (`applyUiMode`, `adaptToCurrentMonitor`)
- Modify: `gui/src/os/linux_host.cc:46`, `:63`, `:94-105`
- Modify: `gui/src/os/windows_host.cc:92`, `:109`, `:154-190`
- Modify: `tools/ui_capture/main.cc:58`, `:102-103`
- Modify: `gui/src/player_view.cc:2453-2455` (`adaptToCurrentMonitor`)

**Interfaces:**
- Consumes: nothing from earlier tasks — this is a pure interface narrowing.
- Produces: `Host::init(PlayerWindow* owner)` (one parameter), `Host::adaptToCurrentMonitor()` (no parameters). `Host::applyUiMode` ceases to exist.

- [ ] **Step 1: Narrow the interface**

In `gui/src/host.hh`, delete the `UiMode` enum and its comment (lines 19-23). Change:

```cpp
    virtual bool init(PlayerWindow* owner, UiMode initialMode) = 0;
```
to
```cpp
    virtual bool init(PlayerWindow* owner) = 0;
```

Delete `virtual void applyUiMode(UiMode mode) = 0;` and its comment (`:94-97`) outright. Change:

```cpp
    virtual void adaptToCurrentMonitor(UiMode mode) = 0;
```
to
```cpp
    virtual void adaptToCurrentMonitor() = 0;
```

- [ ] **Step 2: Update the Linux host**

In `gui/src/os/linux_host.cc`:

- Line 46: `bool init(PlayerWindow* owner, UiMode initialMode) override {` → `bool init(PlayerWindow* owner) override {`
- Line 63: replace `if (initialMode == UiMode::Complete) window_->set_fullscreen(nullptr);` with an unconditional `window_->set_fullscreen(nullptr);` — Complete was the only surviving mode, so this is what the app has always done for it.
- Lines 94-102: delete `applyUiMode()` entirely, **including its comment about the asynchronous configure**. Move that comment's substance to the class doc: it explains why fullscreen lands late, and phase 2 will want it.
- Line 103: `void adaptToCurrentMonitor(UiMode) override {` → `void adaptToCurrentMonitor() override {`. Keep the no-op body and its comment: this one is still a genuine Wayland limitation, not dead mode plumbing.

Also update the comment at `gui/src/os/linux_host.cc:36` (`"BEFORE calling init() (to decide the initial UiMode)"`), which is now describing something that no longer happens.

- [ ] **Step 3: Update the Windows host**

In `gui/src/os/windows_host.cc`:

- Line 92: `bool init(PlayerWindow* owner, UiMode initialMode) override {` → `bool init(PlayerWindow* owner) override {`
- Line 109: `RECT startRect = (initialMode == UiMode::Complete) ? A : B;` → keep the `Complete` branch's expression only, dropping the conditional and the Essential rect helper it called. If that helper (`computeEssentialWindowRect` or similar) has no other caller, delete it too.
- Lines 154-167: delete `applyUiMode()` entirely.
- Lines 169-190: `adaptToCurrentMonitor(UiMode mode)` → `adaptToCurrentMonitor()`, keeping only the Complete branch of its internal `RECT r = (mode == UiMode::Complete) ? A : B;`.
- Lines 31 and 350: rewrite the two comments that describe fixed sizes "the app sets itself (see `PlayerWindow::toggleUiMode()`)" — that function no longer exists.

- [ ] **Step 4: Update the capture tool**

In `tools/ui_capture/main.cc`:
- Line 58: `bool init(PlayerWindow*, UiMode) override { return true; }` → `bool init(PlayerWindow*) override { return true; }`
- Line 102: delete `void applyUiMode(UiMode) override {}`
- Line 103: `void adaptToCurrentMonitor(UiMode) override {}` → `void adaptToCurrentMonitor() override {}`

- [ ] **Step 5: Update the one caller**

In `gui/src/player_view.cc:2453-2455`:

```cpp
void PlayerWindow::adaptToCurrentMonitor() {
    host_->adaptToCurrentMonitor();
}
```

And rewrite the section header comment at `:2440-2446`, which currently explains the Essential/Complete split, to describe what the section now holds.

- [ ] **Step 6: Verify UiMode is gone everywhere**

Run:
```bash
grep -rn "UiMode\|applyUiMode" gui/ tools/ core/ android/
```
Expected: **no output**.

- [ ] **Step 7: Build both the app and the capture tool**

Run:
```bash
scripts/linux/build.sh --debug && \
./build/linux_debug/gui/orientation_test && \
./build/linux_debug/gui/ui_metrics_test && \
./build/linux_debug/gui/ui_icons_test && \
./build/linux_debug/gui/ui_text_test && \
./build/linux_debug/core/variants_test && \
./build/linux_debug/core/stats_test && \
./build/linux_debug/core/facets_test
```
Expected: clean build, all seven tests exit 0.

Then run the app once more and confirm it still opens fullscreen and behaves identically:
```bash
./build/linux_debug/gui/matrix_player
```

**Windows is not verifiable from this machine.** `windows_host.cc` was edited blind. Say so explicitly in the completion report rather than claiming the change is verified — it needs a build from an MSYS2 UCRT64 shell (`scripts\windows\build.ps1 -Debug`) before it can be called done.

- [ ] **Step 8: Commit**

```bash
./git_wrapper commit -m "Host no longer knows what a UI mode is

init() and adaptToCurrentMonitor() lose their mode argument; applyUiMode is
deleted along with the Wayland fullscreen round-trip it forced. Windows half
is untested from this machine." -- \
  gui/src/host.hh gui/src/os/linux_host.cc gui/src/os/windows_host.cc \
  gui/src/player_view.cc tools/ui_capture/main.cc
```

---

### Task 5: Persist the orientation preference

**Files:**
- Modify: `gui/src/player_view.cc` — `create()` (load) and `toggleOrientation()` (save)
- Test: covered by `orientation_test` from Task 1 plus the manual check below; no new test target.

**Interfaces:**
- Consumes: `OrientationState` (Task 1), `PlayerWindow::orientation_` (Task 2), `Db::getSetting`/`Db::setSetting`.
- Produces: two settings keys — `"orientation_auto"` (`"1"`/`"0"`) and `"orientation_manual"` (`"h"`/`"v"`).

- [ ] **Step 1: Load on startup**

The settings API is `void Db::saveSetting(const std::string& key, const std::string& value)` and `std::string Db::loadSetting(const std::string& key)` (`core/include/core/db.h:60-61`). **`loadSetting` takes no default** — it returns an empty string for an absent key, and both snippets below rely on that: empty means "never set", which must resolve to automatic.

The member is `Db db_` (`gui/src/player_view.hh:952`).

In `PlayerWindow::create()`, after the `Db` is opened and before the first `recalcLayout()` call (`gui/src/player_view.cc:377`), add:

```cpp
    // Orientation preference. An absent key means "automatic", which is the
    // right default for a fresh install: the window's own shape is a better
    // first guess than any stored value. Only an explicit "0" disables it.
    orientation_.autoEnabled = (db_.loadSetting("orientation_auto") != "0");
    orientation_.manual = (db_.loadSetting("orientation_manual") == "v")
        ? Orientation::Vertical : Orientation::Horizontal;
```

- [ ] **Step 2: Save on toggle**

Extend `toggleOrientation()` from Task 2:

```cpp
void PlayerWindow::toggleOrientation() {
    if (!renderer_) return;
    orientation_.toggleManual((int)renderer_->width(), (int)renderer_->height());
    db_.saveSetting("orientation_auto", orientation_.autoEnabled ? "1" : "0");
    db_.saveSetting("orientation_manual",
                    orientation_.manual == Orientation::Vertical ? "v" : "h");
    recalcLayout();
    invalidate();
}
```

- [ ] **Step 3: Verify by round-trip**

Run:
```bash
scripts/linux/build.sh --debug
./build/linux_debug/gui/matrix_player
```
Press Alt+L, quit, and reopen. Then check the stored values directly:
```bash
sqlite3 build/linux_debug/gui/matrix_player.db \
  "SELECT key, value FROM settings WHERE key LIKE 'orientation%';"
```
Expected: `orientation_auto|0` and `orientation_manual` holding `h` or `v`. (Confirm the settings table's real name and column names from `core/src/db.cpp` first — adapt the query, do not adapt the schema.)

Since no drawing code reads the orientation yet, the round-trip is only observable in the database. That is the correct scope for this phase.

- [ ] **Step 4: Commit**

```bash
./git_wrapper commit -m "Persist the orientation preference

Automatic is the default for a fresh install: the window's own shape beats
any stored guess." -- gui/src/player_view.cc
```

---

## What phase 1 deliberately does NOT do

After this plan lands, the app looks exactly as it does today. Nothing reads `curOrientation_` to draw anything. That is intentional: this phase removes a wrong abstraction and installs a right one, and mixing the layout rewrite into the same change would make both unreviewable.

The remaining phases, each getting its own plan:

- **Phase 2 — Bar A layout arithmetic.** A pure `rail_layout.hh/.cc`: given a bar rectangle, an orientation, whether bit-perfect is on, and whether search is open, produce the rect for every anchor. Tested without a Canvas. This is the part most likely to break silently, so it gets tests before it gets pixels.
- **Phase 3 — Drawing Bar A.** The letters and their lightness ladder, the AutoEQ box, the one unfurling-list widget used by both the profile list and the search suggestions, and the open-search state. `kEqHpMaxRows` is retired here.
- **Phase 4 — Bar B.** The transport bar rotated, with `Canvas::setRotation()` for its text and an unrotated square for the artwork.
- **Phase 5 — Presentation extraction.** Lift the two bars out of `player_view.cc` behind a view model and an intent list, and have `android/CMakeLists.txt` consume the same files.

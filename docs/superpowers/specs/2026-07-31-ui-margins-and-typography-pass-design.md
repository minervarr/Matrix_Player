# UI pass: margins, positioning, typography

Date: 2026-07-31
Status: approved, pending implementation plan

## What this is, and what it deliberately is not

A refinement pass over the Complete-mode UI, derived from reading thirteen
headless screenshots of the real app (`tools/ui_capture`, see below) rather
than from reading the code. Every change here is **geometry or type style**.

Explicitly out of scope, by the author's instruction:

- No color token changes.
- No new icons, and no changes to the icon set.
- No new elements on screen — nothing is added that was not already drawn.
- No behavioral or option changes.

The purpose is that the app look more composed while remaining recognizably
the same app. `docs/UI_DESIGN_SYSTEM.md` §1 (dark, serif, single-accent,
square, accent-means-state) is the identity being preserved, not revisited.

Two findings from the review were dropped on exactly that ground: the §8.4
per-track quality marks and the §8.2 two-member mosaic quadrants are both
decisions the design system already defends with reasons, and neither is a
margin or a typeface. They are recorded under "Deferred" below rather than
silently discarded.

## The hard constraint: the legibility floor

`CMakeLists.txt:204-219` runs `min_text_size --emit-header` at build time over
exactly the four faces `gui/src/ui_fonts.hh` loads, and emits the WORST case
across them:

| Face | thinnest stroke (em) | min readable (px) |
|---|---|---|
| NewCM10-Regular | 0.027344 | **18.29** ← binding |
| NewCM10-Bold | 0.044922 | 11.13 |
| NewCM10-Italic | 0.029297 | 17.07 |
| NewCMMono10-Regular | 0.054688 | 9.14 |

`ui_metrics.hh` pins the SMALLEST type role (`caption`) to that floor exactly,
and derives every other role from it by `×kUiTypeRatio^n`. Therefore:

**No type role in this pass may shrink.** There is no headroom underneath the
serif — the smallest role is already sitting on the floor, and the scale
clamps uniformly below the reference height precisely so that it cannot be
squeezed further. Proportions may be re-spaced upward, or changed through
`space()`/`stroke()` geometry, never by taking type below `kMinReadableTextSizePx`.

The Mono figure is not trivia. It is why B1 below is safe: machine text moving
to Mono moves onto the face with twice the headroom of the one it leaves.

## A. Geometry

### A1 — One padding for the grid, used the same way everywhere

`gridPadX_`/`gridPadY_` are used two different ways in one file. Layout passes
them through `metrics_.space()` (`player_view.cc:1897,1920`); drawing and
hit-testing use them **raw** (`:1012`, `:1026-1027`, `:2488`, `:2492-2493`,
`:2501-2502`). At 1440p the column width is reserved against ~32 px
while tiles are painted at 24 px, and the top margin stays pinned at 16 real
pixels that never scale with anything.

This is the drift `UI_DESIGN_SYSTEM.md` §4 forbids ("no bare pixel literals,
no hand-written multiplications"), and it is what makes the first row of
artwork bleed against the top edge of the window while the sidebar beside it
has air. It is the highest-impact change in the pass and the only one that
also closes a real inconsistency.

Route both through `space()` at every site, and raise the vertical pad until
the top air optically matches the sides. Optically, not numerically: the
horizontal pad is *followed by* per-cell centering slack
(`(tileStepX - gridArtSize_)/2`) that the vertical has no equivalent of, so
equal numbers would not produce equal-looking margins.

Because layout, drawing and hit-testing would then finally read one value,
tile hit-testing must be re-verified after the change — those three are
currently in disagreement, and a click landing one tile off is the failure
mode to watch for.

### A2 — Cap the track list's width

`colW = tp.x + tp.w - pad - colX` (`player_view.cc:1223`) takes the entire
remaining window. At 2560 px a track title sits ~1045 px from its own
duration, so the eye has to cross the screen to pair a song with its minute.

Cap `colW`. §8.4 already promises this is all it takes — *"One rectangle
governs the row"*: `rowX`/`rowW` (`:1277-1278`) feed the hover fill, the
playing bar, the disc-separator rules, the duration column and the hit-test
rectangle, so one clamp moves all of them together and none is touched
individually.

The cap is a measure-derived width, not a magic number. It should be
expressed through `space()` so it scales like everything else.

### A3 — The primary button is always on the same side

Today, across four panels of one family:

| Panel | Primary | Position |
|---|---|---|
| Manage Folders | `Done` | right (`:3400`) |
| Audio Settings | `Apply` | right (`:3620`) |
| Folder Picker | `Select This Folder` | right (`:3954`) |
| EQ Settings | `Select` / `Assign to Device` | **left** (`btnAt(0)`, `:3852`) |

Three to one. Move EQ's onto the majority convention: primary right,
secondaries to its left. Only rect assignment in `drawEqSettings` changes; the
click handlers already read the rects.

This is not a taste call. Mouse muscle memory is learned per-application, and
a primary that changes sides between sibling pages cannot be learned at all.

### A4 — Buttons that do not strand themselves

All four panels anchor their button row to `content.bottom - (btnH + pad)`.
With a single folder in the list, that leaves ~900 px between the content and
the controls that act on it — a content-to-void ratio near 1:10.

Anchor the row below the content when the content is short, and let it fall to
the bottom only once the list actually reaches it. One shared expression, so
the four panels cannot drift apart again.

### A5 — Let the transport's right edge breathe

The clock and the DSP badge share one `rightEdge = t.x + t.w - space(16)`
(`:1712`) and the same `caption` size, so `0:00 / 0:00  BITPERFECT` reads as a
single run of text rather than as a reading and a state.

Separate the badge from the clock by a real step of the spacing scale, and
give the cluster its own margin from the window edge. `rcDspBadge_`
(`:1717-1718`) is the badge's hover hit rect and must move with it — the
expanded signal-path readout on hover depends on that rect staying over the
compact tag's home.

## B. Typography

Only styles already baked into the atlas: Regular, **Bold**, *Italic*, and
Mono (the `FontStyle::Math` slot, already carrying track numbers, durations and
the transport clock). No new font file, no re-bake, no cache invalidation.

### B1 — Machine text is Mono; human text stays serif

Filesystem paths and machine identifiers are currently set in the same serif
as album titles:

- `Music Folders` — the music-root paths
- `Select Music Folder` — the current path and every directory row
- `EQ / AutoEQ Profiles` — `Device: 32BB:0004`

These are data, not names. The family already contains a face that says so,
and (per the floor table above) it is the face with the most legibility
headroom of the four.

What stays serif, deliberately: headphone model names, album and artist names,
and every panel title. A headphone is a name, not an identifier. The rule is
about what the string *is*, not about where it appears.

Size does not change anywhere — only style. `caption` and `secondary` share a
size on purpose (`ui_metrics.hh`), separated by the color ladder and by style;
this change works with that grain rather than against it.

## Verification

There is no automated visual test and this pass does not add one. It is judged
by looking:

```bash
scripts/linux/build.sh --debug
./build/linux_debug/gui/matrix_ui_capture --out ui-shots --frame 2560x1440
./build/linux_debug/gui/matrix_ui_capture --out ui-shots-1080 --frame 1920x1080
```

Both resolutions matter here: 1080 is the reference height where the scale
factor is exactly 1.0, and 1440 is where every `space()`/raw-literal
disagreement becomes visible. A1 in particular is invisible at 1080 and
obvious at 1440 — which is why it survived this long.

`ui_metrics_test` must still pass; it pins the type scale and the
spacing/stroke math this pass leans on.

Manual checks that the screenshots cannot make:

- Grid tile hit-testing after A1 (click the first and last tile of a row).
- The DSP badge's hover readout after A5.
- Panel button clicks after A3/A4.

## Deferred — recorded, not dropped

- **Grid tile hover is nearly invisible** against high-contrast artwork. A
  contrast problem, not a margin one. The in-scope route would be a heavier
  frame via `stroke()`; the honest route is revisiting the grey.
- **The grid is the only list with no scroll affordance**, while
  `panels::drawScrollbar` already exists and is used by every panel. Reuse,
  not invention — but it puts a new element on screen, so it waits.
- **Per-track quality marks when every track shares a tier**: the column then
  repeats what the album header already states. Omitting it in that one case
  contradicts nothing in §8.4, which never considered the uniform-tier album.
- **Two-member remix mosaics** read as a half-loaded cover rather than as
  §8.2's intended "absence". Worth judging together on screen before touching.
- **A section header** in the content area, for symmetry with Settings, which
  has one. Rejected for this pass as a new element; the A1 margin was chosen
  instead.

## How the screenshots were obtained

`tools/ui_capture/main.cc` (Debug-only, Linux, target `matrix_ui_capture`)
gives `PlayerWindow` a `Host` whose window is a `VK_EXT_headless_surface`, then
drives the app through its ordinary entry points — `onLButtonDown()` for state,
`drawFrame()` for frames — and writes one PNG per state. It synthesizes clicks
on the rects `recalcLayout()` itself computed, so the captures follow the
layout instead of drifting behind it. That property is what makes it useful
for verifying this pass: after A1 changes the padding, the capture clicks the
new geometry, not the old.

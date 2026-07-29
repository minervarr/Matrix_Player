# UI Design System — Rigor Pass

**Date:** 2026-07-28
**Status:** Design approved, ready for implementation planning
**Scope:** `gui/src/` — type scale, geometry scale, text-color ladder, Essential-mode
defects, and `docs/UI_DESIGN_SYSTEM.md`. No change to the visual *language*
(dark / serif / single-accent / square / static) — this pass makes the code
match the language it already claims.

---

## 1. Why

`docs/UI_DESIGN_SYSTEM.md` describes a coherent system. Four places where the
code contradicts it:

1. **The type hierarchy is compressed.** Seven roles are authored as percentages
   of a 661px reference height, but every role is clamped by
   `kMinReadableTextSizePx` = 18.2857. At the app's real render height (H = 1080
   — Complete mode force-fullscreens via `linux_host.cc:70`, and both connected
   displays are 1920x1080) the seven roles render at 18.79 / 18.79 / 20.42 /
   21.24 / 22.87 / 26.14 / 27.77. The bottom four span 13%; seven names produce
   about three visually distinct sizes. The smallest role clears the floor by
   0.5px, so a slightly shorter window clamps it alone and distorts the scale
   non-uniformly.

2. **Two populations of geometry.** `uiScale_` = `textSizes_.nav / 13.0f` =
   **1.634** at H = 1080. Literals written `x uiScale_` render at 1.634x their
   source value; literals written bare render at 1.0x and never scale at all.
   The bare set includes the accent selection bar, the grid focus halo, the
   now-playing glow radii, the brand inset, and the transport info gap — so
   those details stay physically fixed while the art and text around them
   scale. `SP_*` appears 5 times in a 3854-line file.

3. **The text-color ladder is inverted.** `CLR_TEXT_DIM` (140) is *brighter*
   than `CLR_TEXT_SECONDARY` (128). Raising DIM 80 -> 140 for WCAG inverted the
   ordering: badges, hints and placeholders now read louder than artist names
   and durations, the opposite of the intent stated in `theme.hh`.

4. **Doc drift.** `CLR_WARNING`, the bitperfect warning banner, `drawWarningIcon`
   and `panels::drawScrollbar` are undocumented; the doc still says `CLR_ERROR`
   is the only reserved semantic color and "not yet drawn". Every `file:line`
   citation is 40-150 lines stale.

Two pre-existing Essential-mode defects were found while verifying the above
(section 6). They are unrelated to the redesign but sit in the code being
opened.

### Non-goals

Quality-color tier weight, the no-motion rule, the missing seekbar, the
album-view draw/layout coupling, and `player_view.cc`'s overall size beyond
extracting the metrics module. Those are redesign questions, not rigor.

---

## 2. The floor is correct — design within it

`min_text_size.cc:26-34` documents that `pixelCriterion = 0.5` is already
calibrated for *this* renderer's MSDF antialiasing ("a sub-pixel-wide stroke
shows up as a fainter line rather than vanishing outright"), and
`thinnestStrokeEm` is already the 10th percentile of stroke widths rather than
the absolute minimum. `kMinReadableTextSizePx` = 18.2857 is a twice-calibrated
number, not a conservative guess. It is not loosened.

Per-style floors were considered and rejected: the generated header
deliberately emits the max across the four baked styles because "whichever
style ends up smallest on screen is the binding one", and roles do not map 1:1
to styles anyway (`secondary` is drawn Italic, Mono *and* Roman). Changing that
would be a framework change, out of scope.

---

## 3. `gui/src/ui_metrics.hh/.cc`

The type scale and the geometry scale collapse into one factor:

```
uiScale  = UiScale{ .referenceHeight = 1080, .floorScale = 1.0 }.factor(H)
caption  = kMinReadableTextSizePx * uiScale
role(n)  = caption * pow(kTypeRatio, n)          // kTypeRatio = 1.18
```

`UiScale` is `framework/vk_canvas/core/layout.hh`'s existing helper, whose own
comment says it was "validated by matrix_player's `recalcLayout()` ... promoted
into core so every consumer app stops reimplementing it". The app never adopted
it; `uiScale_ = nav / 13.0f` is exactly that reimplementation. Adopting it
deletes the hand-rolled factor.

**Pinning the smallest role to the floor is what fixes the distortion.** Because
every role derives from `caption`, and `caption` is the floor times the factor,
the entire scale clamps *uniformly* below H = 1080 instead of the bottom role
clamping alone. Hierarchy is preserved at every window size; it simply stops
growing. `floorScale = 1.0` (rather than `UiScale`'s 0.5 default) is what
expresses "never smaller than the reference".

### Roles

Five roles, four sizes, at H = 1080 (uiScale = 1.0):

| role | px | n | used for |
|---|---|---|---|
| `caption` | 18.29 | 0 | badges, hints, placeholders, section captions |
| `secondary` | 18.29 | 0 | artist, time, duration, search text |
| `body` | 21.58 | 1 | grid titles, track titles, nav, settings rows |
| `title` | 25.46 | 2 | now-playing title, album-view title |
| `header` | 30.04 | 3 | page + panel headers |

`caption` and `secondary` share a size deliberately — they are separated by the
color ladder (section 5) and font style, not by size. This is only coherent
*because* section 5 un-inverts the ladder; at today's values they would be
indistinguishable.

### Role mapping and the visible delta

This section intentionally changes what is on screen — recovering hierarchy is
its purpose. "No visual change" applies only to section 4's geometry work.

| old role | new role | @1080 | delta |
|---|---|---|---|
| `badge` | `caption` | 18.79 -> 18.29 | -0.50 |
| `secondary` | `secondary` | 18.79 -> 18.29 | -0.50 |
| `body` | `body` | 20.42 -> 21.58 | +1.16 |
| `nav` | `body` | 21.24 -> 21.58 | +0.34 |
| `transportTitle` | `title` | 22.87 -> 25.46 | **+2.59** |
| `trackPanelTitle` | `title` | 26.14 -> 25.46 | -0.68 |
| `header` | `header` | 27.77 -> 30.04 | **+2.27** |

Scale spread widens from 1.48x to 1.64x. The now-playing title growing 11%
inside a 131px transport bar is the item to check first on the initial run.

### Module surface

```cpp
struct UiTextSizes { float caption, secondary, body, title, header; };

struct UiMetrics {
    float       scale;   // 1.0 at H <= 1080, H/1080 above
    UiTextSizes text;

    float space(float authored)  const;  // authored * scale
    float stroke(float authored) const;  // max(1, round(authored * scale))
};

UiMetrics computeUiMetrics(float contentHeight);
```

Pure arithmetic — no Canvas, no Vulkan, no `player_view.hh` include. The scale
constants move out of `player_view.hh`, which currently holds a 7-constant type
scale next to ~200 members.

### Test

`gui/src/ui_metrics_test.cc` — assert-based, no framework, matching the
`framework/vk_canvas/core/tests/` convention (`#undef NDEBUG` so asserts survive
Release). Covers:

- exact role values at H = 700, 900, 1080, 1440, 2160
- `scale == 1.0` for every H <= 1080; `scale == H/1080` above
- `role(n+1) / role(n) == kTypeRatio` at every height (the invariant that the
  old system violated)
- `caption >= kMinReadableTextSizePx` at every height
- `stroke(1) >= 1` at every height (hairlines never vanish)

This is the first automated test in the repository.

---

## 4. Scaling policy and the re-authoring sweep

`uiScale` goes from 1.634 to 1.0 at H = 1080. Two populations, two treatments:

| today | renders @1080 | treatment |
|---|---|---|
| `40 * uiScale_` | 65.4 px | re-author to `space(65)` |
| `40` (bare) | 40 px | keep 40, becomes `space(40)` |

Both then render identically at 1080 and, for the first time, scale correctly
above it. **Applying x1.634 to the bare literals as well would grow every
decorative detail by 63% — the distinction is the whole correctness argument of
this section.**

### Re-authored (currently `x uiScale_`, multiply source by 1.634)

| | now | authored |
|---|---|---|
| `SP_XS` .. `SP_XL` | 4 / 8 / 12 / 20 / 40 | **6 / 13 / 20 / 33 / 65** |
| transport height | 80 | 131 |
| sidebar width | 170 | 278 |
| track row height | 40 | 65 |
| transport button / gap / pad | 44 / 12 / 12 | 72 / 20 / 20 |
| grid tile text offset | 10 | 16 |
| grid row gap trailing term | 18 | 29 |
| gear icon half / inset / rule gap | 9 / 16 / 4 | 15 / 26 / 7 |
| album view: art gap / num col / title col | 40 / 30 / 46 | 65 / 49 / 75 |
| album view: artist image | 120 | 196 |
| panel list inset / row height | 14 / 34 | 23 / 56 |
| panel buttons (Remove / Apply / Select) | 170 / 120 / 200 | 278 / 196 / 327 |
| nav row height / top | 40 / 102 | 65 / 167 |
| brand band / search band | 50 / 58-90 | 82 / 95-147 |
| settings rows (top/halfW/h/gap) | 90 / 220 / 52 / 14 | 147 / 360 / 85 / 23 |
| panel header / close w / close h | 56 / 90 / 32 | 91 / 147 / 52 |
| panel button | 120 x 36 | 196 x 59 |
| warning strip height | 28 | 46 |

### Kept as-is (currently bare, gain `space()`)

`gridPadX_` 24, `gridPadY_` 16, `kTargetTilePitch` 250, `kMinGridArtSize` 80,
`kGridArtMargin` 30, **`kPanelRowH` 44**, brand inset 16, search field inset 12,
nav label inset 20, selection pill +4/-8, grid focus halo -6/+12, now-playing
glow insets -9/-6/-3 at radii 12/10/8, transport right margin 16, time gap 24,
album-view sidecar spacing 36/28/16, and the `rcBtnPrev_.left - 76` transport
info gap.

`kPanelRowH` is the trap in this list: it *looks* like a scaled value and sits
among scaled neighbours, but every use passes it bare (`(float)kPanelRowH` into
`widgets::drawScrollList`, `rowCount * kPanelRowH` into the scrollbar). It
renders at 44px today and must keep the number 44.

### Strokes

`stroke(n) = max(1, round(n * uiScale))`. All identical to today at 1080:

| | authored | @1080 | @1440 | @2160 |
|---|---|---|---|---|
| hairline separators, field underlines | 1 | 1 | 1 | 2 |
| accent selection bar | 3 | 3 | 4 | 6 |
| quality frame (grid) / aura (track list) | 3 | 3 | 4 | 6 |
| quality per-row border (mixed tiers) | 2 | 2 | 3 | 4 |
| last-played bar, settings row outline | 2 | 2 | 3 | 4 |

Rationale for a separate class: continuous scaling puts a 1px hairline at
1.33px on a 1440p display, landing blurred across two device pixels. Rounding to
whole device pixels keeps hairlines crisp at every height.

**Which class a literal belongs to is decided per-argument, not per-call.** A
single draw can carry both: the last-played marker
(`canvas.rect(x, y + a + 2, a, 2, ...)`, `player_view.cc:1005`) has a spacing
offset (`+2`, the gap below the art) and a stroke height (`2`, the bar's
thickness). These become `space(2)` and `stroke(2)` respectively. Where a value
is ambiguous, the test is whether it reads as a *gap* or as a *line*.

### Spacing-scale adoption falls out of the re-authoring

Several bare literals land exactly on a re-authored `SP_*` token — 12 -> 20 is
`SP_MD`, 8 -> 13 is `SP_SM`, 4 -> 6 is `SP_XS`, 20 -> 33 is `SP_LG`, 40 -> 65 is
`SP_XL`. Those call sites adopt the token during the sweep. Adoption is a
consequence of this work, not a separate chore.

### Fixed rather than re-authored

The grid empty-state messages center with `g.x + g.w * 0.5f - 160` and `- 120`
(`player_view.cc:928, 940`) — hardcoded half-widths guessed for one string at
one size, already slightly off and made worse by every size change in section 3.
These become measured centering via `textCenteredStyled` from
`framework/vk_canvas/core/text_util.hh`.

### Net effect

At H = 1080, geometry is pixel-identical to today. The only movement is the six
text roles from section 3 and the grid empty-state messages (two call sites,
`player_view.cc:927` and `:940`) finding their true center.

---

## 5. Text-color ladder

| token | now | new | contrast on `CLR_BG_MAIN` (10) |
|---|---|---|---|
| `CLR_TEXT_PRIMARY` | 242 | 242 | 17.7:1 |
| `CLR_TEXT_SECONDARY` | 128 | **170** | 8.5:1 |
| `CLR_TEXT_DIM` | 140 | **128** | 5.0:1 |

Restores the ordering, keeps all three above WCAG AA, and makes the steps
visible rather than 12 grey levels apart. Visible effect: artist lines, times
and durations brighten; badges, hints and placeholders dim slightly.

**Tightest contrast in the app after this change:** the DSP badge draws
`CLR_TEXT_DIM` on `CLR_BG_TRANSPORT` (22), which goes 5.38:1 -> **4.58:1**.
Still above AA (4.5:1), but with little margin. This gets an explicit comment in
`theme.hh` recording the value and the binding pair, so it is not quietly
lowered later — the same failure mode that produced the current inversion.

---

## 6. Essential-mode defects

Both pre-existing, both independent of the redesign, both in code this pass
opens.

### 6.1 Controls overlap the art and title

At Essential mode's only Linux size (1200x700 — `linux_host.cc:27-28`;
Essential-mode window sizing is unimplemented on Linux per `CLAUDE.md`),
`recalcLayout()` produces:

```
margin          = max(12, W/20)              = 60
bottomReserve   = max(120, H/5)              = 140
artSize         = min(1080, 500, 700)        = 500   -> art     y  60..560
titleY          = 560 + 12                          -> title   y 572..602
btnSize         = max(40, W/8)               = 150
btnY            = 700 - 60 - 150             = 490   -> buttons y 490..640
```

`bottomReserve` is intended to hold the title and the button row, but `btnSize`
is derived from **W** and exceeds the reserve. The buttons overlap the bottom
70px of the album art and sit on top of the title band.

**Fix:** derive `btnSize` from `bottomReserve` rather than `W`, so the reserve
is authoritative and the three zones cannot collide at any window shape.

### 6.2 Album art is blurry in Essential mode

`loadTransportArtTexture()` (`player_view.cc:1886-1901`) already intends to
handle the two-context sizing, decoding at `max(transport, essential)` so a mode
switch never stretches the texture. But `rcEssentialArt_` is computed **only**
inside `recalcLayout()`'s Essential branch, which returns early
(`player_view.cc:1499`). The Complete branch never assigns it, and it is
zero-initialized.

On the normal path — launch (Complete, fullscreen) -> play -> Alt+L:

```
essentialW/H = 0                 (never computed)
targetW/H    = max(~92, 0) = ~92 (transport thumb: transportH - 2*tPad)
drawn at rcEssentialArt_ = 500   -> ~92px texture stretched ~5.4x
```

Confirming signature: the art is blurry for the track that was already playing
at the moment of the switch, and becomes sharp on the next track change —
because that reload runs with `rcEssentialArt_` finally populated.

**Fix:** compute both modes' rects unconditionally in `recalcLayout()` and drop
the early return, so the existing `max()` receives real values. The Complete
branch's `clearGridArtTexCache()` call (fired when `gridArtSize_` changes) must
be guarded to `uiMode_ == Complete`, so running Complete's layout math while in
Essential mode does not churn the grid art cache.

### 6.3 Text through the new roles

Essential mode's title moves from `transportTitle` to `title`. Its
proportional-to-window geometry (`W/20`, `H/5`, `W/8`) is **kept** — deriving a
compact mode's layout from window fractions is a legitimate choice, not drift,
and it is what makes the mode work at sizes Complete mode refuses.

---

## 7. Documentation

`docs/UI_DESIGN_SYSTEM.md`:

- §2 color tokens: new ladder values with contrast figures; add `CLR_WARNING`
  and its role; correct the claim that `CLR_ERROR` is the only reserved
  semantic color.
- §3 typography: replace the seven-percentage table with the single
  floor x ratio^n model; state the uniform-clamp property explicitly.
- §4 spacing: re-authored `SP_*` values; state the space/stroke split as the
  rule it now is.
- §5 shape: unchanged.
- §7 iconography: add `drawWarningIcon`.
- §8 components: document the warning banner and `panels::drawScrollbar`.
- §10 checklist: add "spacing through `space()`, strokes through `stroke()`,
  never a bare literal" and "the text-color ladder is ordered 242 > 170 > 128".
- Re-anchor every `file:line` citation.

---

## 8. Files touched

| file | change |
|---|---|
| `gui/src/ui_metrics.hh` / `.cc` | **new** — scale, roles, `space()`, `stroke()` |
| `gui/src/ui_metrics_test.cc` | **new** — assert-based test |
| `gui/src/theme.hh` | color ladder, re-authored `SP_*`, contrast comment |
| `gui/src/player_view.hh` | remove 7 scale constants + `UiTextSizes`; hold a `UiMetrics` |
| `gui/src/player_view.cc` | `recalcLayout()` restructure, call-site sweep, both Essential fixes, empty-state centering |
| `gui/src/panels/settings_panels.cc` | call-site sweep |
| `gui/src/art_view.cc` | call-site sweep (`CLR_TEXT_DIM` empty state) |
| `gui/CMakeLists.txt` | build `ui_metrics`, add the test target |
| `docs/UI_DESIGN_SYSTEM.md` | section 7 above |

---

## 9. Verification

1. `ui_metrics_test` passes (section 3).
2. Build and run at 1080 against a pre-change build. Expected deltas, and
   nothing else:
   - six text roles resize per section 3's table
   - the grid empty-state messages sit at their true center
   - artist / time / duration brighten; badges / hints dim (section 5)
   - Essential mode: no overlap, sharp art
3. Geometry is pixel-identical everywhere else. Any other movement is a
   re-authoring arithmetic error — check that literal's population in section 4.

There is no automated GUI test in this repo. `framework/vk_canvas/core/headless.hh`
plus `core/capture/` could render deterministic panel states to PNG for a real
visual regression check; that is **deferred**, not part of this pass, and noted
here as the obvious next step if this kind of change recurs.

---

## 10. Sequencing

**The two scales coexist during the sweep.** `uiScale_` cannot flip from 1.634
to 1.0 in the same commit that re-authors only *some* call sites — every
not-yet-migrated `x uiScale_` literal would shrink by 1.634x until the sweep
finished. So `UiMetrics metrics_` is added alongside the existing `uiScale_`,
call sites migrate file by file (each using whichever factor matches its
authored value, so every intermediate commit renders correctly), and the legacy
factor is deleted only once nothing references it.

Ordered so each step is independently buildable, runnable and revertible:

1. `ui_metrics` module + test (nothing consumes it yet)
2. Adopt it in `recalcLayout()`; re-author the scaled literals
3. Sweep `drawFrame()`'s bare literals onto `space()` / `stroke()`
4. Sweep `settings_panels.cc` and `art_view.cc`
5. Color ladder
6. Essential-mode fixes 6.1 and 6.2
7. Empty-state centering
8. Documentation

Steps 2-4 carry the "no visual change" claim and are the ones to verify
individually rather than in aggregate.

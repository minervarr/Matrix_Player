# Matrix Player — UI Design System

This document is the written map of Matrix Player's visual language. The GUI is
**fully custom-rendered** — no OS controls anywhere — through the first-party
`vk_canvas` Vulkan engine: SDF rounded rectangles, analytic triangles (for
icons), and MSDF text. Almost everything is drawn in one place,
`PlayerWindow::drawFrame()` (`gui/src/player_view.cc:793`), with the fullscreen
album-art window in `gui/src/art_view.cc` and the shared settings-panel widgets
in `gui/src/panels/settings_panels.cc`, the scale/type model in
`framework/app_shell/ui_metrics.hh` (+ the reusable
`framework/vk_canvas/core/widgets.*`).

`file:line` references throughout point at the source of truth. When you change
the look, update both the code and this doc.

---

## 1. Design principles

1. **Custom-rendered, no OS chrome.** Every pixel is drawn by the app. There are
   no native buttons, lists, scrollbars, or dialogs — settings are full-page
   overlays, not modal windows.
2. **Dark, serif-typographic, single-accent.** A near-black stack of surfaces,
   New Computer Modern (a serif) for all text, and exactly one accent — a vivid green
   (`CLR_ACCENT` `rgb(0,200,83)`).
3. **Square throughout.** Artwork, structural surfaces, and interactive chrome
   (buttons, hover/selection highlights, search fields) all use square corners
   (`UI_CORNER_RADIUS = 0`). **The circular radio dot is the only rounded shape
   left.** The album tiles' state rings used to be the other exception — a
   three-layer rounded glow — and they were squared: curves and a soft falloff
   read as another UI's vocabulary next to this one's hard edges, and the fade
   said nothing the solid band doesn't. Selection/hover highlights fill the full row
   height (matching the action-button height) and, in lists/radios, hug their
   text.
4. **Accent = state, hover = neutral.** Green *only* signals state — focus,
   selected, active, playing. Merely hovering something is always a **neutral
   grey** treatment, never green. This is the rule that keeps the accent
   meaningful.
5. **Static and instant by design.** There is no animation. The UI redraws on
   dirty and flips between discrete states immediately (see §9).
6. **Resolution-robust, not pixel-pinned.** One factor drives everything:
   text roles derive from a font-geometry floor times a fixed ratio, and all
   other geometry goes through `space()`/`stroke()` off the same factor
   (see §3, §4). No bare pixel literals, no hand-written multiplications.

---

## 2. Color tokens

Single source: `gui/src/theme.hh`. All colors are `ColorRef` (`RGB()` macro),
converted to the engine's float `Color` via `toColor(ref, alpha=1)`.

| Token | RGB | Role |
|---|---|---|
| `CLR_BG_MAIN` | 10,10,10 | Main content / grid background (base) |
| `CLR_BG_TRACKPANEL` | 14,14,14 | Album/track full-page view |
| `CLR_BG_SIDEBAR` | 18,18,18 | Left nav sidebar |
| `CLR_BG_TRANSPORT` | 22,22,22 | Bottom transport bar |
| `CLR_TEXT_PRIMARY` | 242 | Primary text (17.7:1 on `CLR_BG_MAIN`) |
| `CLR_TEXT_SECONDARY` | 170 | Artist, time, duration (8.5:1) |
| `CLR_TEXT_DIM` | 128 | De-emphasized: badges, hints, placeholders (5.0:1) |
| `CLR_TEXT_ALBUM_TITLE` | 255 | Grid album title (pure white) |
| `CLR_ACCENT` | 0,200,83 | Brand + all state (focus/selected/active/playing) |
| `CLR_HOVER` | 38 | Neutral hover pill / focus frame |
| `CLR_SEPARATOR` | 36 | 1px hairline separators, unfocused input underline |
| `CLR_INPUT_BG` | 24 | Search / text-field fill |
| `CLR_TILE_PLACEHOLDER` | 28 | Album-art placeholder fill |
| `CLR_TILE_MORE_GREEN` | 30,104,62 | The mosaic tile's "and more" quadrant (§8.2) |
| `CLR_WARNING` | 224,180,40 | Non-blocking warnings (bitperfect mismatch strip, §8.8) |
| `CLR_ERROR` | 220,70,70 | Reserved for hard/fatal failures (not yet drawn) |

**`CLR_TILE_MORE_GREEN` is deliberately not the accent.** It marks *"there are
more than four here"* — information, not state — and principle #4 keeps the
accent green for state alone. A deeper, less saturated green still reads
unmistakably as green on the near-black tile without ever being read as "this
is playing". The two meet on screen whenever a now-playing remix group is
visible, which is the pairing to check if either is ever retuned.

**Quality-color tier (a second, scoped palette).** Album art borders (§8.2) and
the track-list quality mark (§8.4) are colored by objective audio quality, not
UI state — this is the one deliberate exception to "one palette":

| Token | RGB | Tier |
|---|---|---|
| `CLR_QUALITY_DSD` | 255,255,255 | DSD |
| `CLR_QUALITY_DXD` | 255,165,0 | >=352.8kHz (DXD) |
| `CLR_QUALITY_HIRES` | 0,255,255 | >=64kHz (hi-res PCM) |
| `CLR_QUALITY_STANDARD` | 255,255,0 | >=44.1kHz (CD quality) |

Below 44.1kHz, no border is drawn. `qualityColorFor(sampleRate, isDsd)`
(`gui/src/theme.hh`) is the single place this mapping lives.

**This palette appears in exactly one place: the per-track quality mark in the
album view (§8.4).** The album grid used to carry a tier-colored frame per tile
too. It was removed, and the reason is worth keeping: a tier color per tile
described nothing worth its noise on a grid already dense with artwork, and it
gets less meaningful still once one tile is meant to stand for a whole set of
quality and edition variants (see `TODO.md`). Scoping the second palette to one
small mark is what keeps it a second palette rather than a competing one.

Two lessons from that frame, both worth not relearning. **Never layer a state
ring over a tier frame:** the tile's state halos are rounded (§1.3) while the
frame was square, so the tier color showed through at the four corners and read
as a rendering defect rather than as two signals. And **state outranks
metadata** (principle #4) — where both want the same pixels, resolve to one
color *before* drawing rather than painting one over the other.

**The text ladder is ORDERED — 242 > 170 > 128 — and must stay that way.** It
was previously inverted: `CLR_TEXT_DIM` had been raised 80→140 for WCAG without
re-spacing `CLR_TEXT_SECONDARY` (128) around it, so badges, hints and
placeholders read *louder* than artist names and durations. Both values already
cleared AA; the defect was purely ordering. All three tiers now clear AA with
visible steps between them.

**Tightest contrast in the app:** the transport bar's DSP badge draws
`CLR_TEXT_DIM` on `CLR_BG_TRANSPORT` (22) — **4.58:1**, above AA's 4.5:1 but
with almost no margin. Re-check that pair before lowering `CLR_TEXT_DIM`.

**Accent alpha ramp** — the accent is layered at set opacities for tint and glow.
Keep new uses on this ramp:

| Alpha | Use |
|---|---|
| `UI_SELECT_TINT_ALPHA` = 0.16 | selection pill tint (lists, nav, playing row) |
| 0.10 / 0.22 | (grid glow) idle now-playing outer/inner |
| 0.20 / 0.45 / 1.0 | (grid glow) active now-playing three-layer |
| 0.40 | last-played bottom marker |
| 0.80 | selected-tile ring |

**One palette.** The app palette is `theme.hh`. The `vk_canvas` framework ships
its own bluer `col::` default (`framework/vk_canvas/core/canvas.hh`) for its
demos — app code must not draw from it. (Removed dead tokens: `CLR_SEEKBAR_*`
— there is no seekbar, see §8.3.)

---

## 3. Typography scale

**Model.** One factor, one ratio, one floor — all in
`framework/app_shell/ui_metrics.hh`:

```
uiScale  = UiScale{ referenceHeight = 1080, floorScale = 1.0 }.factor(H)
caption  = kMinReadableTextSizePx * uiScale          // 18.2857 * s
role(n)  = caption * pow(kUiTypeRatio, n)            // kUiTypeRatio = 1.18
```

`UiScale` is the framework helper (`framework/vk_canvas/core/layout.hh`).
`kMinReadableTextSizePx` (~18.29px) is generated at build time from the shipped
font's thinnest-stroke geometry (`ui_min_text_size.gen.h`) — the tool already
calibrates its 0.5px criterion to *this* renderer's MSDF antialiasing and takes
the 10th percentile of stroke widths, so it is a twice-calibrated floor, not a
guess. Don't loosen it; design within it.

**Why the smallest role IS the floor.** Because every role derives from one
clamped value, the scale clamps **uniformly** below the reference height. The
hierarchy is preserved at every window size and simply stops growing. The
previous system authored seven independent percentages against a 661px
reference — every one of which sat *below* the floor at that reference — so the
smallest role clamped alone and squashed the scale: fully flat below ~711px, and
still compressed to ~13% between the bottom four roles at 1080p.

**Roles** (`framework/app_shell/ui_metrics.hh`; px shown at H = 1080, where uiScale = 1.0):

| Role (`metrics_.text.*`) | px @1080 | n | Used for |
|---|---|---|---|
| `caption` | 18.29 | 0 | badges, hints, placeholders, section captions |
| `secondary` | 18.29 | 0 | artist, time, duration, search text |
| `body` | 21.58 | 1 | grid titles, track titles, nav, settings rows |
| `title` | 25.46 | 2 | now-playing title, album-view title |
| `header` | 30.04 | 3 | page + panel headers |

`caption` and `secondary` share a size **deliberately** — they are separated by
the ordered color ladder (§2) and by font style, not by size. This only works
because that ladder is ordered; at the old inverted values they were
indistinguishable.

Computed once per `recalcLayout()` (`player_view.cc:1489`). Verified by
`framework/app_shell/tests/ui_metrics_test.cc`, which pins the exact values and asserts the
ratio invariant holds at every height — the property the old system violated.

**Font styles** (`FontStyle`: Roman / Bold / Math / Italic):
- **Bold** — titles and headers (brand, album titles, page headers, playing row).
- **Italic** — artist lines, placeholders, empty-state sentences.
- **Math** — repurposed to New Computer Modern **Mono** for numeric readouts (track #,
  duration, time, quality badge) so digits don't jitter as they change.
- **Roman** — everything else.

**Faces.** Base is New Computer Modern (`fonts/newcomputermodern/NewCM10-*`, serif); paths live in `gui/src/ui_fonts.hh`. Script fallbacks match the
serif lineage: New Computer Modern for Cyrillic/Greek (baked eagerly), and
Fandol Song / Harano Aji Mincho / UnBatang for CJK/Japanese/Korean (baked lazily
from scanned metadata). The art window mirrors the same mapping.

**Overflow helpers** (`framework/vk_canvas/core/text_util.hh`, UTF-8-safe):
`truncateToWidth` (prefix + …), `splitTwoLines` (two-line grid titles),
`wrapText` (album-view prose), `fitTextSize`, `textCenteredStyled` (MSDF-correct
centering).

---

## 4. Spacing, size & stroke

**Two classes, two helpers.** Nothing multiplies by a scale factor by hand:

| helper | for | behavior |
|---|---|---|
| `metrics_.space(x)` | pads, gaps, row heights, widths, icon boxes | `x * uiScale` |
| `metrics_.stroke(x)` | hairlines, borders, bars | `max(1, round(x * uiScale))` |

`stroke()` snaps to whole device pixels because continuous scaling puts a 1px
hairline at 1.33px on a 1440p display, landing blurred across two pixels.

**Spacing scale** (`theme.hh`, authored at the 1080 reference):
`SP_XS 6 · SP_SM 13 · SP_MD 19 · SP_LG 32 · SP_XL 65`. Prefer these over new
literals.

**Stroke weights** (authored; @1080 / @1440 / @2160):

| | authored | 1080 | 1440 | 2160 |
|---|---|---|---|---|
| hairline separators, field underlines | 1 | 1 | 1 | 2 |
| accent selection bar | 3 | 3 | 4 | 6 |
| last-played bar, settings row outline | 2 | 2 | 3 | 4 |

**A single call can carry both classes.** The last-played marker
(`canvas.rect(x, y + a + space(2), a, stroke(2), …)`) has a spacing *offset* and
a stroke *thickness*. The test is whether the value reads as a gap or as a line.

**Everything is authored at 1080**, the height this app actually renders at
(Complete mode force-fullscreens — `os/linux_host.cc`). Values were derived from
the pre-rigor-pass factor by **truncation**, matching the `(int)` casts the old
code used; rounding instead shifts each by a pixel and compounded to ~8px on the
settings rows.

**Regions** (`recalcLayout()`, `player_view.cc:1489`):
- Transport bar: full width, bottom, height `space(130)`, bg `CLR_BG_TRANSPORT`.
- Sidebar: left, width `space(277)`, bg `CLR_BG_SIDEBAR`, 1px right hairline.
- Grid / content: remaining area, bg `CLR_BG_MAIN`.
- Track panel: when an album is open it **replaces** the grid (same rect), bg
  `CLR_BG_TRACKPANEL`; settings are full-page overlays in the same area.

**Grid tiles:** columns from a `space(250)` target pitch (clamped 2–8),
`gridArtSize_ = max(space(80), gridW/cols − space(30))`; square art + centered
title + artist.

**Row heights** (three, by context): `kPanelRowH 44` (settings list rows — note
it is passed *bare*, never pre-scaled, so it keeps the number 44),
`trackRowHeight_ space(65)` (album-view tracks), `space(55)` (settings-page
inline rows, backend radios, search fields). Documented rather than merged —
each suits its density.

---

## 5. Shape & radius

- **One radius token:** `UI_CORNER_RADIUS = 0` — the whole app is square
  (buttons, hover/selection highlights, search fields, grid hover frame). This
  single token enforces it; nudge it up if a softer look is ever wanted.
- **Exceptions (own radii):** the circular radio dot (`dot.w*0.5`), the
  decorative multi-layer now-playing tile glow (10/12px, §8.2), and the
  Settings gear icon's circular hub + rounded teeth (§7) — icon geometry,
  not chrome, so it sits outside the `UI_CORNER_RADIUS` rule like the glow.
- **Icons** use their own 36-unit design grid (§7).
- **Highlights** fill the full row height (match the action-button height); in
  lists and radio groups they hug the row's text rather than spanning full
  width.

---

## 6. Elevation & separators

Depth is conveyed by **surface value + 1px hairlines**, never shadows (the engine
has no shadow/blur primitive). The four surfaces read as a gentle stack: base
content is darkest (`CLR_BG_MAIN` 10) and recedes; chrome sits above (sidebar 18,
transport 22); the album view (14) overlays the grid.

Separators are all 1px `CLR_SEPARATOR`: sidebar right edge, transport top edge,
panel-header underline, album-view column rule, and the unfocused search
underline (which turns `CLR_ACCENT` on focus).

---

## 7. Iconography

Icons are **glyphs in the shared MTSDF atlas** — the same atlas, shader and
draw pass as the UI text. Not images: they carry no colour, cost no extra GPU
pass, and are tinted at draw time exactly like text. Same approach Apple uses
for SF Symbols.

Artwork is authored in Inkscape as SVG (`tools/icon_font/icons/*.svg`) and
packaged into `assets/fonts/icons/matrix-icons.otf` by
`tools/icon_font/build_icon_font.py`. `PlayerWindow::bakeIconGlyphs()` bakes it
into the atlas at Private Use Area codepoints; `drawUiIconGlyph`
(`ui_icons_draw.cc`) draws it.

| Icon | Shape |
|---|---|
| Play | one right-pointing triangle |
| Stop | one rounded square (radius `2/36`) |
| Prev | left bar + left-pointing triangle |
| Next | right-pointing triangle + right bar |
| Settings | 5-tooth gear: circular hub unioned with 5 teeth 72° apart — **baked but no longer drawn by the app**, see below |
| Warning | triangle with the `!` **cut out as holes** — used only by the warning strip (§8.8) |

Each icon is authored inside a square **design box** measured in ems, whose
bottom edge sits on the baseline. Two consequences worth knowing:

- The *box*, not each icon's ink, maps onto the target rect — so icons keep
  their relative sizes, exactly as the old shared 36-unit grid gave them.
- The box size is the **resolution knob**, and it is set *per icon*: atlas cell
  size derives from the outline's own bounds, so an N-em glyph is baked at
  N × 96px. Transport icons use 2 em (192px, drawn 71–284px); the warning and
  the gear use 1 em (96px; the warning is drawn 30–148px). Total ~3.3 MB of
  atlas.

Denser is **not** automatically better. A bake far above the drawn size means
heavy minification, and the shader's single bilinear tap smears it — a flat
4-em box left the gear at 12.8× and the warning at 10.4× minification, and both
looked blurry. `ui_icons_test` bounds every box on both sides for this reason.

`tools/icon_preview` (Debug, Linux) draws only the icons, at a size ladder plus
the sizes the app really uses, for judging exactly this.

Because this build sets `MSDFGEN_USE_SKIA=OFF`, msdfgen has no overlap
resolver: **every icon must be one non-self-intersecting outline** (`Path →
Stroke to Path`, then `Path → Union` in Inkscape). Nested contours become
holes automatically. See `tools/icon_font/README.md`.

If the icon font is missing, `drawUiIcon` (`player_view.cc`) falls back to the
original primitive construction (triangles + rounded rects) so buttons degrade
to the old look rather than going blank. That fallback cannot draw the
warning's `!` — it fills bar and dot in the icon's own colour, so they
composite invisibly.

Color is passed per button — normally `CLR_TEXT_PRIMARY`; the idle **Play** is
`CLR_ACCENT` (a call to action). The transport bar and Essential mode share
the Play/Stop/Prev/Next bank.

The **Settings gear has no draw site left**: the sidebar's settings entry is
the word *Settings* (§8.1), because the four rows above it are words and a lone
icon among them read as a different kind of control than it is. The glyph stays
in the set — still baked, still bounded by `ui_icons_test`, still on
`icon_preview`'s ladder — so restoring it is a one-line change, and the icon
pipeline (and the atlas-cache fingerprint that keys off it) doesn't churn for a
layout decision.

---

## 8. Components & states

The universal state rules (apply everywhere):

- **Hover** → neutral grey (`CLR_HOVER`) square, full-height highlight.
- **Selected / active / playing (lists & rows)** → accent-tint fill
  (`CLR_ACCENT @ UI_SELECT_TINT_ALPHA`) + a thin accent **left bar** + accent
  text, square and full-height, hugging the row's text. One family across
  sidebar nav, settings lists/radios, and the album-view playing row.
- **Content emphasis (album art)** → accent **ring** (selected) or multi-layer
  **glow** (now-playing); artwork can't carry a fill+bar, so this is the
  documented content variant.
- **Settings-menu rows** (Add Music Folder, etc.) → an **outlined box** (4-side
  border); hover fills the box, the active toggle gets a 2px accent border.
- **Empty states** → italic `CLR_TEXT_DIM` sentence.

### 8.1 Sidebar / nav
Brand "MATRIX PLAYER" (Bold, accent, truncated). Shared **search field** (§8.6).
Six content-type filters — *Albums* / *EPs* / *Singles* / *Compilations* /
*Live* / *Remixes*, each filtering the grid by `Album::releaseType` —
hover = grey pill; active = accent-tint pill + left bar + accent label (same
family as before, just six rows instead of one). They read in that order for a
reason: original material by descending size, then the artist's own material
re-presented, then other people's reworkings of it. **The row order is not the
`ReleaseType` enum order** — those values are frozen by the `albums` table that
stores them — and it lives in `recalcLayout()` alone. Below a hairline separator
sits a further row, **Settings** — spatially isolated from the filters because
the app reads as albums-and-music first, configuration second. It is a *word*,
set exactly like the six filter labels (same `space(20)` inset, same `body`
size, same accent-when-active rule), so they all read as one family; the gear glyph it
replaced is still in the icon set but is no longer drawn (§7). It shares the
same hover/active visual language; opening it replaces the whole content area
(§8.6) and closing it returns to whichever filter was active, never
resetting to Albums.

**DRIVER'S AUTOEQ block** (`drawHeadphoneBlock`, `player_view.cc`) — anchored to
the BOTTOM of the sidebar under a `CLR_SEPARATOR` hairline, in the space the old
now-playing mini card used to occupy. A `DRIVER'S AUTOEQ` header (Bold,
`secondary` size, `CLR_TEXT_DIM`), then `No AutoEQ`, then the saved profiles,
then a `Search more…` row that opens the EQ panel. Rows reuse the nav's exact
selection family —
accent-tint pill + `stroke(3)` left bar when active, `CLR_HOVER` when merely
hovered (§1.4) — at `secondary` size, one step down from the nav proper, because
this is equipment configuration and not navigation. Every profile label goes
through `truncateToWidth`: names run long and the sidebar is only `space(277)`.
The **header does not**, deliberately — a section header that quietly loses its
tail hides a fit failure rather than showing one.

**The label is not "HEADPHONES".** What an AutoEQ profile corrects is the
*driver*, and the same list serves IEMs and speakers; but `DRIVERS` alone would
read as an output driver in an app whose primary path is a USB DAC, so the
header has to carry both halves.

Rows are `space(36)`, not the `space(48)` this block was authored with. The
sidebar holds three rows below Settings and no more (the nav is eight
`space(65.36)` rows), so at the original height the `No AutoEQ` row would have
been paid for by a saved pair.

Three states are specific to this block:

- **`No AutoEQ`** — the OFF position of the switch, and an ordinary row wearing
  the ordinary active state when nothing is assigned, so "no EQ" is something
  the block SHOWS rather than the absence of any highlight. It is the FIRST row,
  the radio-list convention where "none of these" heads the set rather than
  trailing it, and it is never the row that gets clamped away when space runs
  short — the saved list below it is.
- **On trial** — `CLR_TEXT_DIM` + Italic, first among the profiles (directly
  under `No AutoEQ`). The profile is already audible; what is pending is whether
  it keeps a row (it needs 60 s of real listening first). Italic-and-dim rather
  than a badge because the row is temporary, and a badge would imply a durable
  property.
- **Hidden in bitperfect mode.** Not greyed — *not drawn at all*, and it gives
  its space back. There is no EQ to pick a profile for, and a disabled control
  still asks to be read before it can be dismissed.

If the window is short enough that the block would collide with the Settings
row, what yields is the SAVED LIST, not the block: the header, `No AutoEQ` and
`Search more…` are the minimum, because a pair that doesn't fit is still one
click away under the latter while the off switch has nowhere else to live. Only when
even one list row won't fit does the block disappear entirely (browsing the
library is the app's primary job).

Saved rows are ordered **pinned first, then most-used**, with recency only as
the tie-break (`Db::loadEqHeadphones`) — with three rows on screen the order
decides what is reachable in one click, and "what I touched last" is not the
same question as "what I actually use".

### 8.2 Album grid
**The two margins are equal by construction, not by hand.** `gridPadX_` is the
only authored pad; `recalcLayout()` resolves it through `space()` into
`gridPadXpx_`, and the TOP pad is *derived* from it by `gridTopPad()`
(`ui_metrics.hh`, pure and pinned by `ui_metrics_test`): a tile is centered in
its cell, so the visible left margin is the pad PLUS half the cell's leftover
slack, which the vertical axis has no equivalent of. Setting the two to the same
number measured equal and looked wrong — the first row bled against the window
edge while the sidebar beside it had air. There is deliberately no independent
vertical knob; changing `gridPadX_` moves both.

Layout, drawing AND hit-testing all read the resolved `gridPadXpx_` /
`gridPadYpx_` / `gridStepX_`. They used to disagree — layout passed the authored
pads through `space()` while draw and hit-test used them raw — so above the 1080
reference the clickable box and the painted box drifted apart.

Square art tiles (placeholder = flat `CLR_TILE_PLACEHOLDER`) + Bold white title
(two-line fallback) + italic secondary artist. **Hover** = neutral grey focus
frame. **State ring** = one solid `space(3)` accent band hugging the art, square
(§1.3): full alpha for **now-playing**, 0.80 for **selected** — the two differ by
weight, never by shape. A thin accent bottom bar marks
**last-played only** (now-playing is already unmistakable — no
double-marking). **No quality-tier frame** — the grid speaks artwork and state
(green) and nothing else; quality is read per track in §8.4. See the
color-tokens section for why the frame that used to be here was dropped.
Empty states (italic `CLR_TEXT_DIM`, measured-centered): "No albums yet…" / a per-filter "No EPs yet"-style message when
a type filter has no matches / "No matches for …" when a search does.

**One tile per release, not per folder.** An album held twice — another edition
(Deluxe, Edición Especial) or the same edition at another quality — contributes
a single tile, showing the group's best member (`core/variants.h` ranks them:
DSD, then sample rate, then bit depth, then track count). The rest are reachable
from that album's page, under `OTHER VERSIONS` / `MORE REMIXES` (§8.4). Search still matches on
*any* member, so grouping tightens the grid without ever hiding a result. A
group never straddles two tabs — release type is part of its key.

**Remix groups wear a 2×2 mosaic instead of one cover.** An edition group can
show one member's art honestly: a deluxe is the same record with more on it.
A remix set is not — it is different music by different hands — so a single
cover would misrepresent what sits behind the tile. Its quadrants carry the
group's covers in ranked order, left-to-right, top-to-bottom:

| Members | Quadrants |
|---|---|
| 2 | two covers, then two flat `CLR_TILE_PLACEHOLDER` |
| 3 | three covers, one flat |
| 4 | four covers |
| 5+ | **three** covers, then a vertical fade from `CLR_TILE_PLACEHOLDER` into `CLR_TILE_MORE_GREEN` |

An empty quadrant is the tile's own background, so it reads as absence rather
than as a broken image. The overflow quadrant replaces a cover rather than
being added beside them: an arbitrary fourth cover would claim to be the whole
set, and the fade says plainly that it is not. Mosaics are **Remix-tab only**;
albums, EPs and singles keep the single cover. Quadrant art is decoded at half
the tile's edge and cached separately from the full-size tile
(`getGridArtTexture`'s `sizeClass`) — there are no mip chains here, so serving
a 2:1 minification of the big texture would alias.

### 8.3 Transport bar
Left: art thumb + Bold title + italic artist. Center: Prev / Play-Stop / Next
vector buttons (grey pill on hover). Right: `m:ss / m:ss` time (Mono) + a DSP
badge (BITPERFECT accent / REF EQ dim) that expands to the signal path on hover.
*There is no seekbar* — position is text only (a deliberate current limitation).
The cluster's margin from the window edge (`space(SP_LG)`) must stay LARGER than
the gap inside it (`space(24)` between time and badge). At 16 it did not, and the
reading and the state read as one run of text pushed against the edge.

### 8.4 Track panel (album view)
Full-page (replaces grid). Large scrolling art + wrapped title/artist + quality
badge + separator, then track rows: track # (Mono) · title · duration (Mono).
Row hover = grey pill; **playing row** = accent-tint pill + left bar + accent
Bold text (the shared selection family).

**Quality is a mark, not a border.** Each row carries one small `UiIcon::Quality`
glyph — a four-point spark — in its own column immediately left of the duration,
tinted by the track's tier. Identical shape and size on every row, always: the
tier lives in the color alone, so the mark can never read as comparative. It
keeps its tier color on the playing row too, since it is the only reading of
quality left in the list and the duration beside it is already neutral there.
Below 44.1kHz nothing is drawn.

This replaced per-row tier *borders*. One outlined rectangle per row is
defensible on paper and unusable in practice: an album of mixed tiers (*Bad
Ideas* — one cyan row, four yellow, six cyan) became eleven stacked frames in two
colors over a list whose whole character was restraint. A mark says the same
thing in a tenth of the ink.

**The list has a capped reading measure.** `colW` is `min(available,
space(820))`, so a wide window stops stretching the row: unbounded, a 2560px
window put a track title ~1045px from its own duration. Note the cap passes
through `space()` — a reading measure should scale with the type — which means
the NUMBER must sit below the width the layout produces unbounded (~925 authored)
or the cap is inert decoration. An earlier attempt used 1180 and did nothing at
any 16:9 size.

**One rectangle governs the row.** `rowX`/`rowW` (the text column plus a
`space(7)` overhang on each side) is the single source for every layer: the
hover/playing fill, the playing bar at `rowX`, the disc-separator rules, and the
hit-test rectangle. The mark's column is anchored to the duration's *reserved*
width (measured once on `"88:88"`), never to each stamp's actual width, or it
would shuffle between `3:52` and `10:05` instead of forming a column.

**Disc separators.** When an album's tracks span more than one tagged disc, a
`DISC n` row precedes each disc's first track: the label (caption, Bold,
`CLR_TEXT_DIM`) centered, with a hairline rule reaching out to either side.
Both rules stop `space(SP_MD)` short of the row box, and the row is deliberately
taller than the label needs so the separator carries its own breathing room
rather than crowding the tracks around it. Separators occupy
layout space but never a track index; row tops are recorded per track for
hit-testing rather than derived from a fixed row pitch.

**The sidecar block reads as a printed page.** Below the track list sit "ABOUT
THIS ALBUM", the artist photo and the artist bio. **Nothing in it takes a hover
treatment** — a background that lights up under the cursor is the wrong
vocabulary for a page of prose. The photo is the one clickable thing (it opens
full-window, Escape or a click to dismiss); its only intended affordance is a
cursor change, not a highlight.

Two traps live here, both already paid for. **Cull by cropping, not by
skipping:** the photo draws through `imageFg`, which composites above the vector
layer, so an overflowing photo would paint over the transport bar — and
`setClip` is a tile-granular (~16px) safety net, not an exact mask. Gating the
draw on the photo fitting *entirely* inside the panel was the cheap fix and it
cost a bug: the photo's height still counted toward layout while nothing was
drawn, so scrolling hit a long dead gap and then the photo snapped in. Draw the
visible slice via `imageFg`'s UV sub-rect instead. And **decode at draw size
with `mips=false`** — a mip chain softens anything drawn even slightly below
1:1 (`art_texture.hh`), which is what made the photo look washed out.

**The variant strip closes the page.** When the album belongs to a variant
group — the same release also held as another edition, at another quality, or
as another remix set (see `core/include/core/variants.h`) — a strip of tiles
sits below the bio, one per *other* member. The album you are looking at is
never in its own strip.

Its caption follows what the group actually is: **`OTHER VERSIONS`** for
albums, EPs and singles, **`MORE REMIXES`** on a remix page. "Other versions"
is empty words there — a remix already *is* a version, so the phrase adds
nothing; what is below is simply more of them.

They are **full-size grid tiles** — `gridArtSize_` art over the same centered
Bold title / italic dim modifier / italic secondary artist stack as §8.2,
wrapping to a second row when the panel is too narrow. A variant *is* an album,
so it is drawn as one; a shrunken tile would say it were a lesser thing. Drawing
at grid size also puts the art at 1:1 with the density `getGridArtTexture()`
decoded it for. One subtraction and one addition against §8.2:

- **No shrink-to-fit and no truncation.** The page scrolls as one, so a second
  row costs only height — and dropping a version defeats the whole strip.
- **A format line**, below the artist. **Not** a quality readout: sample rate
  and bit depth do not tell you which version you want (in a FLAC library every
  tile would repeat much the same figure), so the §8.4 tier mark and the
  `24/96` badge are deliberately absent here. Only a format that changes what
  those numbers *mean* is named — `MP3`, because its 16/44.1 is reconstructed
  and lossy, and `DSD`, because it is a different representation entirely.
  FLAC and WAV are the baseline and stay silent. Mono, `CLR_TEXT_DIM`, and the
  slot is reserved whether or not it is filled so tiles in a row keep one
  height. See `variantFormatLabel()`.

The **modifier line carries the weight** here that it does not on the main
grid: `(Deluxe)`, `- Edición Especial`, a remix tag. It is what tells one
version from another at a glance. The artist line is printed rather than
assumed, because a group shares a base name but not necessarily a credit — a
collaboration is its own version.

Hover is the grid's neutral grey frame, drawn *before* the art so it reads as a
halo — never accent (§1.4: accent is state). Clicking a tile **opens that
version in the same panel**, and the strip recomputes to list the one you just
left, so moving between versions is one click each way. The tiles are `imageFg`
like the artist photo above, so they are cropped to the visible band by the same
UV sub-rect trick, for the same reason.

### 8.5 Essential / mini mode
A separate compact layout: centered large art + Bold title + three big centered
Prev/Play-Stop/Next buttons (grey pill hover). No artist line, no seekbar.

### 8.6b Playlists (`drawPlaylistSection`)
The SEVENTH content section, not a panel. Its sidebar row sits in the upper
group with the six release filters and behaves identically to them
(`PlayerWindow::NavSection`): the content area changes, and the sidebar, the
Settings row, the transport bar and the play/stop key all stay live behind it.

It used to borrow the settings overlay instead (`SettingsPanel::Playlists` +
`panelFromSidebar_`), and that is the thing not to go back to. The panel
dispatchers divert every mouse and key event before the sidebar is ever
hit-tested — correct for a dialog, wrong for a way of browsing music. While a
playlist was on screen you could not click Singles, could not click Settings,
and could not press Space to stop the music.

**Two levels, exactly like the album section.** A grid of three tiles, then one
list opened full-page — `plKind_` holds which, the same way `trackPanelOpen_`
does on the album side. Going back (§8.10) steps out of the list before it
steps out of the section, because a listener who opened a list meant to leave
the list.

**The tile art is generated, because a generated list has no cover.** A tile is
the album grid's own square on the album grid's own geometry, so a playlist
tile is the same object in the same place as an album tile. For an ORDERED list
it is a 2×2 mosaic of the covers behind its top entries, numbered the
mathematical way: **quadrant 1 is top-right and holds first place**, then
top-left, bottom-left, bottom-right. The fourth quadrant is the one carrying
information — with exactly four records behind the list it is the fourth
record's cover; past that it fades to black, three covers plus "and there is
more". Covers are DISTINCT records: five tracks off one album are five rows but
one cover. Empty quadrants stay flat `CLR_TILE_PLACEHOLDER`, so absence reads
as absence rather than as a broken image. (§8.2's remix mosaic is the same
shape for a different reason and numbers its quadrants left-to-right — a remix
group has no ranking pointing at a particular corner.)

An UNORDERED list has no first place to put in a corner, so it wears a flat
treatment: a custom image, a solid colour, or a gradient. Never Heard is the
only unordered list today and it is generated, so there is nobody to pick an
image for it — it gets the gradient. The other two arms belong to hand-made
playlists, which do not exist yet.

**One row per track, and the ranking is spelled only where there is one.**
`ordinal()` (`gui/src/ui_text.hh`) prefixes Heavy Rotation and Forgotten
Favourites. **Never Heard gets no ordinal**: its query orders by artist ⨯ album ⨯
disc ⨯ track — a browsing order — and "7th" would claim a standing that ordering
does not confer. Row height is derived from the two type roles it stacks (title
Bold over artist Italic), never from `kPanelRowH`, which is sized for the single
line the settings panels draw. Plays and duration are Mono, right-inset past the
scrollbar. The list carries the same capped reading measure as §8.4.

**The lists are queries, not data.** Nothing is stored, so nothing can drift from
the log; a range change re-runs the query. Only Heavy Rotation shows range tabs —
it is the one query that takes a `StatsRange`, and offering the control where it
does nothing would misdescribe the other two.

**Empty states carry the reason.** Never Heard distinguishes an empty library
from having heard everything: the first is a dead end, the second an achievement,
and only the GUI can tell them apart.

### 8.6 Settings
Full-page overlays. Shared `panels::drawHeader` (Bold title + Close) and
`panels::drawButton` (filled: primary = solid accent + dark label; secondary =
elevated grey), both at `UI_CORNER_RADIUS`. Row lists via
`widgets::drawScrollList` and radio groups via `widgets::drawRadioRow` (framework
widgets) styled by `matrixListStyle()` / `matrixRadioStyle()` — the shared
selection family. Search fields via §8.6's shared field.

**The primary button is right-anchored in all four panels.** EQ Settings used to
lay its row out left-to-right from `content.left`, making it the one page of four
where the green button changed sides. Mouse muscle memory is learned per app; a
primary that moves between sibling pages cannot be learned.

**Machine text is Mono; names stay serif.** Filesystem paths (the folder picker's
current directory) and identifiers (`Device: 32BB:0004`) are data, not names, and
the family already carries a face that says so — the one with the most legibility
headroom of the four (9.14px floor against the Regular serif's 18.29px, which is
the binding constraint behind `kMinReadableTextSizePx`). Driver names, album
and artist names stay serif: the rule is about what the string IS, not where it
appears. **Known gap:** the folder *rows* inside `widgets::drawScrollList` are
still serif — `ScrollListStyle` has no font-style field, and adding one is a
change to the vk_canvas submodule's API, not to this app.

**Shared search field** (`drawSearchField`, `player_view.cc`): `CLR_INPUT_BG`
fill, `UI_CORNER_RADIUS`, dim placeholder, caret when focused, a bottom underline
that turns accent on focus. One implementation for the sidebar and EQ searches.

**EQ panel tabs** (`drawEqSettings`): `My Drivers` / `All Profiles`, drawn
above the search field. Active = accent-tint fill + a `stroke(2)` accent
underline + accent label; hover = `CLR_HOVER`; otherwise `CLR_TEXT_SECONDARY`.
They are two views over **one list and one selection**, not two lists — so
`Pin`/`Remove` can only ever act on the visibly highlighted row. This panel is
the only place a saved pair is pinned or removed; the sidebar block stays a pure
switcher, since a per-row `×` at `space(277)` would be too small to hit — its
`No AutoEQ` row is a switch POSITION, not an edit, which is why it belongs there
and `Pin`/`Remove` do not. The vocabulary matches the sidebar (§8.1): drivers,
not headphones.

### 8.7 Audio notice strip
A **non-modal** full-width strip drawn directly above the transport bar
whenever the audio path could not do what was asked (`player_view.cc`, guarded
on `audioNotice_`). `CLR_WARNING` tint at `UI_SELECT_TINT_ALPHA`, a `stroke(1)`
hairline top and bottom, the §7 warning icon, then the message in `secondary`.
It does not reserve or shrink grid space — a rare transient event isn't worth a
permanent layout dependency — and it clears on the next play attempt or on
click. This is the only use of `CLR_WARNING`; green stays state-only and
`CLR_ERROR` stays reserved for hard failures.

**Every** audio failure comes through here: a bit-perfect rate mismatch, a
backend that will not configure, and a device that faults mid-playback. It was
`bitperfectWarning_` and wired to the first of those alone, which left the
visibility exactly inverted — a rate mismatch drew a banner, while an ALSA card
that would not open at all failed through `Host::showErrorMessage`, which is
stderr-only on Linux and so showed nothing anywhere. **The message carries the
driver's own words** (`AudioOutput::lastError()` — "Device or resource busy",
"no running JACK server"), never a generic "check Audio Settings": the whole
value of the strip is that it ends the guessing, and a message that says
nothing specific is the same silence with a border around it.

### 8.8 Scrollbar affordance (`panels::drawScrollbar`)
A thin track + proportional thumb docked inside a scrolling list's right edge
(`settings_panels.cc:69`). `CLR_SEPARATOR` track, `CLR_TEXT_SECONDARY` thumb —
**chrome, not state**, so it never uses the accent. Draws nothing when the
content fits, so callers can call it unconditionally. It is purely an
affordance: not hit-tested, not draggable; scrolling stays on the wheel.
It exists because `widgets::drawScrollList` clips overflowing rows silently —
a USB DAC in row 7 of a 6-row viewport was invisible and looked unreachable.
Every panel that scrolls draws it.

### 8.9 Fullscreen art window (`art_view.cc`)
A separate window with its own swapchain. Renders only the album texture,
aspect-fit and centered on `CLR_BG_MAIN`; empty state is centered "No artwork" in
`CLR_TEXT_DIM` — the same app palette as the main window.

### 8.10 Going back (`PlayerWindow::goBack`)
There is **no back button drawn anywhere**, and that is the rule, not an
omission: nothing in this UI is closed by a chrome affordance. Going back is
Escape, or the back button on the side of the mouse — the two go through one
function so they cannot describe different shapes of "back".

One step, outermost first: a settings panel, then a live search filter, then
the Settings page, then an opened playlist, then the album view. At the root of
a section it does nothing, exactly as Escape on the album grid always has.

**Forward is one step and one step only.** The mouse's forward button re-enters
what the last back left (`navForward_`) and then the stack is spent; any other
navigation clears it, the same way following a link drops a browser's forward
history. Closing a settings panel records no forward: the state it came from
was the Settings page it was borrowing, which is not where the listener was.

---

## 9. Motion

**The UI is intentionally static.** There are no animations, tweens, eases, or
fades anywhere. Rendering is redraw-on-dirty (`markDirty()` pushes a fixed number
of frames); hover / selected / playing / focus all flip instantly. This is a
deliberate choice — a bit-perfect audio tool that never spends GPU on motion —
not a gap. If motion is ever added, it belongs behind the framework's
`AnimatedFloat` (already available, currently unused by the app).

---

## 10. Consistency rules (enforceable checklist)

1. **Hover is neutral, accent is state.** Never use `CLR_ACCENT` for a plain
   hover. Never leave a selected/active row with no accent.
2. **One radius token.** All chrome uses `UI_CORNER_RADIUS` (currently 0 —
   square). Only the radio dot and the tile glow define their own radii.
3. **One selection family for rows.** Reuse the accent-tint pill + left bar +
   accent text (see `matrixListStyle()` / `matrixRadioStyle()`); don't invent a
   new selected look.
4. **One input field.** Text fields go through `drawSearchField`; don't hand-roll
   a box with a raw `RGB()` fill.
5. **One palette.** Draw from `theme.hh`; never from the framework's `col::`.
6. **Spacing from the scale.** Prefer `SP_*` over new literals.
7. **Text through the role scale.** Use a `metrics_.text.*` role + the right
   `FontStyle`; never hardcode a pixel size.
8. **Spacing through `space()`, strokes through `stroke()`.** Never a bare pixel
   literal in a draw call, and never a hand-written `* scale`. If a value reads
   as a gap it's `space()`; if it reads as a line it's `stroke()`.
9. **The text ladder stays ordered:** `CLR_TEXT_PRIMARY` 242 > `CLR_TEXT_SECONDARY`
   170 > `CLR_TEXT_DIM` 128. Re-check contrast — including DIM on
   `CLR_BG_TRANSPORT`, the app's tightest pair at 4.58:1 — before touching any
   of the three.
10. **`kPanelRowH` is passed bare.** It renders at its literal value and is not
   pre-scaled; wrap it in `space()` at the call site, don't re-author the constant.

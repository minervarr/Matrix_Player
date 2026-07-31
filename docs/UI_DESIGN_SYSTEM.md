# Matrix Player — UI Design System

This document is the written map of Matrix Player's visual language. The GUI is
**fully custom-rendered** — no OS controls anywhere — through the first-party
`vk_canvas` Vulkan engine: SDF rounded rectangles, analytic triangles (for
icons), and MSDF text. Almost everything is drawn in one place,
`PlayerWindow::drawFrame()` (`gui/src/player_view.cc:793`), with the fullscreen
album-art window in `gui/src/art_view.cc` and the shared settings-panel widgets
in `gui/src/panels/settings_panels.cc`, the scale/type model in
`gui/src/ui_metrics.hh` (+ the reusable
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
`gui/src/ui_metrics.hh`:

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

**Roles** (`gui/src/ui_metrics.hh`; px shown at H = 1080, where uiScale = 1.0):

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
`gui/src/ui_metrics_test.cc`, which pins the exact values and asserts the
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
Four content-type filters — *Albums* / *EPs* / *Singles* / *Remixes*, each
filtering the grid by `Album::releaseType` — hover = grey pill; active =
accent-tint pill + left bar + accent label (same family as before, just four
rows instead of one). Below a hairline separator sits a fifth row, **Settings**
— spatially isolated from the filters because the app reads as
albums-and-music first, configuration second. It is a *word*, set exactly like
the four filter labels (same `space(20)` inset, same `body` size, same
accent-when-active rule), so all five read as one family; the gear glyph it
replaced is still in the icon set but is no longer drawn (§7). It shares the
same hover/active visual language; opening it replaces the whole content area
(§8.6) and closing it returns to whichever filter was active, never
resetting to Albums.

### 8.2 Album grid
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

### 8.6 Settings
Full-page overlays. Shared `panels::drawHeader` (Bold title + Close) and
`panels::drawButton` (filled: primary = solid accent + dark label; secondary =
elevated grey), both at `UI_CORNER_RADIUS`. Row lists via
`widgets::drawScrollList` and radio groups via `widgets::drawRadioRow` (framework
widgets) styled by `matrixListStyle()` / `matrixRadioStyle()` — the shared
selection family. Search fields via §8.6's shared field.

**Shared search field** (`drawSearchField`, `player_view.cc`): `CLR_INPUT_BG`
fill, `UI_CORNER_RADIUS`, dim placeholder, caret when focused, a bottom underline
that turns accent on focus. One implementation for the sidebar and EQ searches.

### 8.7 Bitperfect warning strip
A **non-modal** full-width strip drawn directly above the transport bar when a
bitperfect-mismatch playback attempt fails (`player_view.cc`, guarded on
`bitperfectWarning_`). `CLR_WARNING` tint at `UI_SELECT_TINT_ALPHA`, a
`stroke(1)` hairline top and bottom, the §7 warning icon, then the message in
`secondary`. It does not reserve or shrink grid space — a rare transient event
isn't worth a permanent layout dependency — and it clears on the next play
attempt or on click. This is the only use of `CLR_WARNING`; green stays
state-only and `CLR_ERROR` stays reserved for hard failures.

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

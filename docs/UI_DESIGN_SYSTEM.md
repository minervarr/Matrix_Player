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
2. **Dark, serif-typographic, single-accent. Two atmospheres, and they do not mix.**
   Matrix, the default theme, is a near-black stack of surfaces and exactly one
   accent — a vivid green (`CLR_ACCENT` `rgb(0,200,83)`). The catalogue in
   `gui/src/theme_catalog.cc` replaces these chrome tokens at runtime: the
   built-in palettes, plus a `.theme` or Alacritty colors file dropped in the
   themes directory beside the database. Each palette still has exactly one
   accent, and that accent still means state only.

   The type is a hard limit, not a preference:

   - **User atmosphere.** The music surface: bar A, bar B, the album grid, the
     filters, playlists, the album view (cover, identity, track list, disc
     cue, artist page), the signal chain, and the AutoEQ switcher opened from
     the home bar. New Computer Modern, the serif. A Terminus glyph on this
     surface is a bug.
   - **Nerd atmosphere.** Everything inside Settings: the main page and every
     sub-page, including the EQ catalogue reached from Settings. Terminus, the
     1-bit bitmap, ALL CAPS, integer scale only.

   The same AutoEQ names are allowed to wear both faces, and that split is
   intentional. From the home bar they are a music control, so they stay
   Computer Modern (§8.11). Inside Settings they are configuration, so they
   are Terminus (§8.6). A control does not carry its face across the boundary.
3. **Square throughout.** Artwork, structural surfaces, and interactive chrome
   (buttons, hover/selection highlights, search fields) all use square corners
   (`UI_CORNER_RADIUS = 0`). Settings radios and switches are bitmap circles
   on the Terminus grid (`gui/src/panels/pixel_marks.hh`), the same pixels as
   the letters beside them — a vector disc is a different resolution from
   that face. The album tiles' state rings used to be the other exception — a
   three-layer rounded glow — and they were squared: curves and a soft falloff
   read as another UI's vocabulary next to this one's hard edges, and the fade
   said nothing the solid band doesn't. Selection/hover highlights fill the full row
   height (matching the action-button height) and, in lists/radios, hug their
   text.
4. **Accent = state, hover = neutral.** The accent *only* signals state —
   focus, selected, active, playing. On Matrix that accent is green; Ink and
   Paper use black or white, and the night and terminal themes use their own
   single hue. Merely hovering something is always a **neutral** step of the
   page background, never the accent. This is the rule that keeps the accent
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
| `CLR_ERROR` | 255,110,110 | Hard failures. Lightened so it clears 4.5:1 on every dark transport bar |

**`CLR_TILE_MORE_GREEN` is deliberately not the accent.** It marks *"there are
more than four here"* — information, not state — and principle #4 keeps the
accent for state alone. A deeper, less saturated green still reads
unmistakably as green on the near-black tile without ever being read as "this
is playing". The two meet on screen whenever a now-playing remix group is
visible, which is the pairing to check if either is ever retuned.

The table above is the **Matrix** default. `applyTheme()` copies one palette
onto these tokens; an unknown or missing `ui_theme` setting stays on Matrix.
Body text, the accent, the error and the warning each clear 4.5:1 on the page
and on the transport bar, and the text ladder stays primary, then secondary,
then dim. Hover is a neutral background step in every theme.

**Quality-color tier (a second, scoped palette).** Album art borders (§8.2) and
the track-list quality mark (§8.4) are colored by objective audio quality, not
UI state — this is the one deliberate exception to "one palette". Dark themes
keep the values below. Paper darkens them so the marks still read on a white
page:

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

**Grid tiles:** `grid::computeShape()` (`gui/src/grid_layout.hh`, pinned by
`grid_layout_test`). Album columns are the nearest whole number of `space(400)`
target pitches (clamped 2–8). Playlists keep `space(250)`, because a generated
list is named. The nearest count is the one that kept a 720 px portrait from
collapsing 2.69 columns into 2. Rows are the nearest whole number of natural
row heights, and the row pitch is then stretched or trimmed so **whole rows
fill the height exactly**. The album tile is the cover. The name, the edition
line and the artist-year line stay available to search and are not drawn.
`gridRowGap_` on that grid is the same air the columns leave beside a cover,
so the picture can take the row. Playlists still reserve their name block.

**Every list scrolls by whole steps and never animates.** `scrollDiscrete()`
tracks the input continuously (`grid::RowScroll`) but the drawn offset is only
ever a whole number of the surface's pitch: a step after 0.2 of a pitch's
travel and then once per pitch, a wheel notch is one step, a finger drag is
1:1 into wheel units, and a lift is a stop — there is no fling. The album
grid's pitch is a row (`gridStepY_`); album-view tracks, playlists, the EQ
switcher, Settings lists and the signal chain each use their own. No partial
grid row is drawn — `lastRow` is exactly `firstRow + gridRows_ − 1`.

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
- **Exceptions (own radii):** the decorative multi-layer now-playing tile
  glow (10/12px, §8.2), and the Settings gear icon's circular hub + rounded
  teeth (§7) — icon geometry, not chrome, so it sits outside the
  `UI_CORNER_RADIUS` rule like the glow. The settings radio used to be the
  other one (`dot.w*0.5`, an analytic disc). It is a bitmap mark now, and so
  is the Interface switch: see below.
- **Settings radios and switches** are 1-bit marks on the label's Terminus
  grid. The circle steps one font-pixel at a time, at the face's 2px stroke.
  Idle ink is `CLR_TEXT_DIM`; the chosen state is `CLR_ACCENT`. The switch's
  off position draws the pill's outline and its on position fills it. The
  knob is the same disc in both.
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

Structural hairlines go through `themeRule()` (`theme.hh`): bar A's inner
edge and the AutoEQ box, the transport edge, the panel-header underline, the
album-view identity and column rules, disc rules, the chip-strip rule, and
the scrollbar track. A separator that already clears 1.2:1 on the page is
used as-is — Matrix, Paper and Hyper stay on `CLR_SEPARATOR`. One that would
disappear into the page is walked toward `CLR_TEXT_DIM` until it clears about
1.35:1. Search underlines stay on `CLR_SEPARATOR` and turn `CLR_ACCENT` when
focused. Those are controls; the rule is structure. Settings radios and
switches do not use the separator: a 2px bitmap stroke in that grey
disappears into the page, so the idle mark is `CLR_TEXT_DIM` and the on
state is `CLR_ACCENT`.

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

Color is passed per button. The center control — Play and Stop, both poses —
is `CLR_ACCENT`, because it is the state. Previous and next stay
`CLR_TEXT_PRIMARY`, the ink. On Matrix that is a green center and white
flanks. A cover palette uses the same split: the mark the eye separates from
the sleeve, and the sleeve's own ink. The transport bar and Essential mode
share the Play/Stop/Prev/Next bank.

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

**THE AUTOEQ BOX** (`drawEqBox` in `bar_a.cc`) — a framed region in bar A,
immediately after Find, carrying ONE thing: the active profile's name, or
`No AutoEQ` in `CLR_TEXT_DIM` when there is none. It is the one text in bar A
whose LENGTH runs along the bar, so it is the one text that rotates in the
horizontal layout. On trial (selected, not yet credited its 60 seconds) it
drops to `CLR_TEXT_SECONDARY`; while its scene is open it wears the accent
tint, the same selected state every other cell uses. It is not drawn at all in
bit-perfect mode — there is no EQ to pick a profile for, and a disabled control
still asks to be read before it can be dismissed.

Touching it opens the **EQ switcher scene** (§8.11). The box used to carry a
discreet `×` for *no profile* beside the name, and the saved list used to
unfurl along the bar itself; both are gone — a profile name in a cell the size
of a filter letter drew as `S...`, sideways.

**The label is not "HEADPHONES".** What an AutoEQ profile corrects is the
*driver*, and the same list serves IEMs and speakers; but `DRIVERS` alone would
read as an output driver in an app whose primary path is a USB DAC, so the
header has to carry both halves.

Saved rows are ordered **pinned first, then most-used**, with recency only as
the tie-break (`Db::loadEqHeadphones`) — the order decides what is reachable
first, and "what I touched last" is not the same question as "what I actually
use".

### 8.1a Bar A's cells: one typeface

Bar A is a rail of single letters at `space(130)`. The music page is one face
— Computer Modern Regular — so the rail does not fight the album grid.
`theme.hh`'s grey ladder and position tell the cells apart.

| Cell | Glyph | Face | Weight | Colour |
|---|---|---|---|---|
| Settings | `S` | Computer Modern | Regular | `CLR_TEXT_DIM` 128 |
| Find | `F` | Computer Modern | Regular | `CLR_TEXT_SECONDARY` 170 |
| Filters | `A E S C L R P` | Computer Modern | Regular | `CLR_TEXT_PRIMARY` 242 |

Settings vs Singles (both `S`) is near-end DIM beside the filter-group
PRIMARY. Terminus stays inside Settings panels (§8.6), not on this rail.

**Order along the bar:** `[Settings][Find][AutoEQ box] · · gap · · [letters]`.
Rare cells cluster at the near end, frequent cells sit alone at the far end,
and the gap stands between them. Find used to be the cell immediately beside
Albums, at the same size, so a slightly-off tap opened search and collapsed
every letter. Settings never moves in any state — opening search must not cost
what was typed.

**An empty search closes itself.** With no text and no chips, a tap anywhere
else closes the field rather than requiring a trip to the `×` at the far end;
that tap is then spent on closing, except on the transport and on Settings,
which are exactly the two things that did not move. A search holding anything
stays open — a stray tap must never discard what was typed.

### 8.11 EQ switcher scene (`drawEqSwitcher`)

A full-page scene over the content area, opened by the AutoEQ box (§8.1) and
closed by a pick, the box again, `Close`, Escape, or any bar A navigation. Page
header `AutoEQ` + `Close` (`panels::drawHeader`, serif — this scene is the
music surface, not Settings), then full-width rows at
`space(SP_XL)`:

1. **`No EQ`** — the OFF position, first, the radio-list convention where "none
   of these" heads the set. Active (accent tint + accent label) when nothing is
   assigned, so "no EQ" is something the page SHOWS rather than the absence of
   any highlight.
2. **The profile on trial**, when one is pending its 60 seconds of real
   listening. It leads the saved rows and reads as active, because it is what
   is audible right now.
3. **The saved pairs**, pinned → most-used → most-recent, each as its FULL
   name — the point of moving the list off the bar.
4. **`All profiles…`** in `CLR_TEXT_DIM` Italic — the route to the whole
   catalogue in the EQ panel. Italic and dim because it is a door, not a
   profile.

Accent is state and never hover: the active row wears the accent tint, a
hovered row wears `CLR_HOVER`. The list scrolls with `panels::drawScrollbar`
and the wheel, and the transport stays live underneath — choosing a profile is
something done WITH the music on.

### 8.2 Album grid
**One authored pad, on all four edges.** `gridPadX_` is the only authored pad;
`recalcLayout()` resolves it through `space()` into `gridPadXpx_` and takes it
off the top and bottom of the grid as well as the sides. Inside that, each row
is stretched to a whole-rows pitch and its art + text are centred in it, so
`gridPadYpx_` is the pad plus that centring offset — it plays the part
`gridTopPad()` used to (the pad plus half the cell's horizontal slack), and the
visible top margin is never less than the side margin. There is deliberately no
independent vertical knob; changing `gridPadX_` moves both.

Layout, drawing AND hit-testing all read the resolved `gridPadXpx_` /
`gridPadYpx_` / `gridStepX_`. They used to disagree — layout passed the authored
pads through `space()` while draw and hit-test used them raw — so above the 1080
reference the clickable box and the painted box drifted apart.

Square covers (placeholder = flat `CLR_TILE_PLACEHOLDER`). Nothing is drawn
under them: no title, no edition line, no artist, no year, no last-played bar.
Those strings are the program's, and the tile is the picture. Search still
matches them. **Hover** = neutral grey focus frame. **Now-playing** = one solid
`space(3)` accent band hugging the art, square (§1.3). Selection is not that
ring — opening an album and leaving it must not leave the playing colour on
the cover. **No quality-tier frame** — the grid speaks artwork and state
(green) and nothing else; quality is read per track in §8.4. See the
color-tokens section for why the frame that used to be here was dropped.

**Order is the shelf, inside the filter that is open.** `grid::orderByArtist`
(`grid_layout_test`) runs after the release-type, chip and query filters, on
the one tile each variant group already contributed. The artist with more
releases in *this* filter comes first: Albums counts albums, Singles counts
singles. Equal counts follow the folded name. Within an artist the older year
comes first (`facets::albumYear`, the modal non-zero track year). A release
whose files name no year follows that artist's dated ones. ASCII case and
surrounding spaces do not split one artist.
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
Full-page (replaces grid). **Two territories, never both on screen.**

**Album world.** Cover + name + details are FIXED. Only the track list (and
OTHER VERSIONS tiles) scroll, with the same discrete snap as every other list.
The album name wraps and is never ellipsized. Layout follows `curOrientation_`:

- **Horizontal** (wide content): one group, centred between the bars
  (`layoutAlbumStage` in `album_stage.hh`). The sleeve is the anchor. It is
  the only thing on the page that came with the record; the name, the
  durations and the ruler are drawn by the app, so the list is placed on
  the picture and the picture is not placed on the list. The sleeve and the
  lines under it (title Bold, modifier dim italic, artist secondary italic,
  badge caption Math) stay centred in the column, and each line is centred
  on the cover. The track-list frame — disc label, row viewport, disc-cue
  footer — is the largest viewport whose rows sit on the sleeve's middle
  while the label fits above them and the footer fits below. That frame is
  one rectangle for every disc of the album, so changing disc does not move
  the sleeve. A short disc centres its rows inside the frame; a single
  track's row is the sleeve's middle. A long disc fills the frame from the
  top and scrolls. The group's width is the sleeve, the column gap
  (`space(SP_XL)`), the row box, and the ruler when this album needs one.
  Omitting the ruler narrows the group and the side margins grow. Including
  it widens the group; the margins shrink and the group stays inside the
  panel pad. No rule between the sleeve and the list.
- **Vertical** (tall content): the same measured group, centred between
  the side margins. The sleeve and the lines share the reading measure
  (`space(820)`), the same cap as the wide row, so a wide tall window
  does not stretch the header to the panel. The sleeve is the group's
  left edge. The lines sit beside it at `space(SP_MD)`, vertically
  centred on the sleeve, and each line is centred in that column. The
  list is that same block and starts at the top of the area under the
  header. The ruler, when this album needs one, is the right edge of the
  group — immediately right of the list — and is centred on the list's
  full height. Omitting it narrows the group and both margins grow. The
  header must leave ~4 track rows (`trackRowHeight_` = `space(SP_XL)`);
  shrink the sleeve to make that true. A very long title may use the
  remaining identity budget, but the name is still never ellipsized. Art
  is `imageFg` / `drawArtOrPlaceholder` and is never drawn with
  `- trackScrollY_`. No hairline between the identity and the list. The
  margins around the panel stay `space(SP_XL)`.
  A **single** is a different picture, and it does not change: the sleeve
  at the top, inside the margin, centred; the lines under it, centred on
  the sleeve; the one track centred in the area that remains, with no
  number and no ruler.

Row hover = grey pill; **playing row** = accent-tint pill + left bar + accent
Bold text (the shared selection family). Track rows keep the current design:
number, title, quality mark, duration, DISC n rules, reading-measure cap
`space(820)`. A single (`ReleaseType::Single`) draws its one track without
a number — the title begins at the row's text inset. One track of a longer
album still carries its number.

**The row shares one centre line.** Number, title, quality mark and duration
meet at the middle of the cap height, not on a shared baseline. `textStyled`
is given the top of the em, so centring the em leaves the capitals and the
lining figures low — the floor the row used to stand on, with the spark
apparently sitting on it. `albumRowTextTop` lifts each run by its own cap
ratio (NewCM10-Regular 0.683, NewCMMono10-Regular 0.611, the Math slot the
number and the duration use). The spark's box is centred on that same line.
The `DISC n` capitals use the same lift, so they share the header's middle
with the rules.

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

**One disc per page.** When an album's tracks span more than one tagged disc,
only the current disc is in the list. Its `DISC n` label (caption, Bold,
`CLR_TEXT_DIM`) is centered on the row box, with a hairline reaching out
to either side. Both rules stop `space(SP_MD)` short of the row box. On a
long disc the label is pinned at the top of the frame. On a short disc it
travels with the rows, so the block — label and tracks — has its middle on
the sleeve. The label never scrolls up against the next disc's label —
there is no next block. Scrolling to the end of this disc and gesturing
again replaces the list with the next disc, from the top. Scrolling up off
the top of a later disc returns to the previous one, at its end. Tracks on
another disc record row top `-1` and do not hit-test. Clicks on the label
are not track hits. Glyphs composite after every rect, so the list is
clipped below the header slot at the top of the frame, and a row whose
glyph band would be sliced is not drawn.

At that end, a small DIM down-arrow sits in a short footer (`space(SP_LG)`),
horizontally centered on the **track-list group** — the capped row box
(number, title, quality mark, duration, and the margins around them), the
same centre as the `DISC n` label. The leftover width of the column is not
that group: on a wide window the reading measure stops at `space(820)` and
centering on the leftover put the arrow to the right of the rows. The
footer is part of the group on every disc, so a short list does not pull
the arrow up onto the rows and the sleeve does not move when the arrow
appears. It is not a hit target. It means another disc. It is Computer
Modern: U+2193 from the Regular face (`drawSerifDownArrow`), the text arrow
— a stem and two curves meeting at one point. It already points down.
U+2206 is an outlined increment and reads as a math operator; the straight
chevron in this face (U+02C5) has no curve, and a staircase of rectangles
reads as Terminus, which is the other atmosphere. After the last disc that
arrow is gone. The footer stays, empty, so the row viewport does not grow.

**The ruler is not the arrow.** The arrow turns the page (another disc, or
the artist page). The ruler says where you are: which disc, and where in
that disc. It is one column of vertical strokes to the right of the
durations, inside the panel pad, with `space(SP_LG)` between the row box
and the column. The strokes stack downward. They are not spread across
the width — the next stroke is the next one down, with an empty gap so
the two read as two strokes.

A stroke is as tall as one track row (`trackRowHeight_`). That is the
unit the list moves by, and it is long enough to see without becoming a
second bar beside the sleeve. In the wide layout the column is centred
on the sleeve and capped to the sleeve's height, because the sleeve is
the anchor: when a long album would stack past the picture, the strokes
shorten first and the gaps after that, and the column stays on the
picture. In the tall layout the column is centred on the full height of
the track list and capped to that height.

One group per disc. Strokes of the same disc sit `space(SP_SM)` apart.
The next disc's group starts `space(SP_LG)` later, so the pause between
discs is wider than the pause inside one. Five discs are five groups. A
disc whose tracks fit is a single stroke — there is one place to be. A
disc that scrolls has one stroke per screen of that disc (the same extent
the list scrolls). Every stroke is the thin one (`stroke(2)`). The stroke
for the screen on screen is `CLR_TEXT_PRIMARY`; the others are
`CLR_TEXT_DIM`. The width does not change. There is no animation. A
single-disc album whose tracks fit draws no column. The column is that
one width on every disc of an album that needs it, including the last
disc's variant strip, so the sleeve does not jump when the disc changes.
If the centred group cannot hold that column inside the pad, the column
is dropped before the list shrinks. The tall layout uses the same column.
It sits to the right of the list and is centred on the list's full height.
In the wide layout the column stays centred on the sleeve.

**Artist world is a second page, not a sticky header.** It is reached only
from the last disc, and only when the page has something to show: an album
description, an artist biography, an artist image, or a credits list. No
album cover, no track list.

The portrait is centred in the content canvas — the panel between the bars,
inside the pad — on both axes (`layoutArtistPage`). Its centre is that
canvas centre. Anything that would place the centre lower is pulled back
up, so the portrait may sit higher and it does not sit lower. That is
admiration for the artist: the regard kept for the person who made the
music. On entry the scroll is zero, so the picture is on that centre and
not below the page. The name, the biography, the album text and the credits
continue below it in the same document, and scrolling down is how they come
into view. The portrait is one and a half times the sleeve, clamped so the
whole picture fits in the inner canvas. It is not shrunk to keep a paragraph
on screen beside it.

The way back is the same serif arrow, pointing up (U+2191). It is drawn in
the air above the portrait once a scroll has come to rest at the top — the
boundary the next gesture crosses — and it is absent when the page is
entered and while the text is on screen. It reserves no band. Reserving one
would push the portrait off the centre. The track list's down-arrow is the
same kind of cue: present once the list has rested at its end, absent while
the list still has room to move.

The name sits under the portrait by `space(SP_MD)`, the gap under a cover.
It is the page heading: `text.header`, Bold, primary, centred. `space(SP_XL)`
then separates that group from the prose. The biography is a section: the
heading "About the artist" at `text.title` (one step under the name), then
the body at `text.secondary`. The album description is the next section,
same heading size, and its heading is the album's name — a subtitle, not
another page title. An edition modifier sits under that name in dim italic.
Both bodies are left-aligned in a column no wider than the track list's
reading measure, and that column is centred on the page.

**Credits** follow the album description when the release names anyone.
The heading is "Credits", at the same `text.title`. Under it the names
are grouped by the kind of work, and each kind is a heading at
`text.body`, Bold, primary, left-aligned, the same weight for every
kind: Music, Arrangement, Performance, Recording, Release. A kind with
nobody in it is absent. Inside a kind every name is the same size and
the same colour, and the role is a dim italic caption at the right of
the line (`composer`, `lyricist`), so one role is not a heading over
the next. Music is `text.body`, primary: composer, lyricist, writer.
Arrangement is that same size in the secondary colour when the release
also names an author, and primary when it does not — the arranger or
the remixer is then who made the music that is playing. Performance
(conductor, ensemble, soloist, performer, featured) is `text.secondary`
in the secondary colour. Recording (producer, engineer, mixer) and
Release (label, copyright) are `text.secondary` in dim; a release line
is italic. On a column too narrow for the caption, the role drops under
its names, still dim italic. A name that appears on several tracks of
the release is one line. The words come from the FLAC Vorbis comments
on those tracks (`credits::fromTags`, pinned by `credits_test`), plus
the downloader database's label and copyright when that row exists.
WAV files contribute nothing here. The section is Computer Modern, with
the rest of this page.

The down-arrow on the album page is a different sentence from the disc
arrow. It sits lower, in a taller band (`space(SP_XL)`) under the whole
content area, and it is centered horizontally on that area — the panel
between the bars, not the track list. In the wide layout the two centres
differ, which is the point. In the tall layout they differ whenever the
group is narrower than the panel. Centering the scene-change on
the list would say "another disc". The two arrows never share a page.
`OTHER VERSIONS` / `MORE REMIXES` stays on the last disc, above the scene cue.

The list **clamps at the end of the current disc**. The gesture that arrives
there does not also turn the page. On a touchscreen that is a lift: the next
drag does. On a mouse wheel the turn is two detents close together
(`kAlbumPageGestureMs`, 320 ms), and only after the roll that arrived has
been quiet for that same interval. Notches of the arrival roll are absorbed.
They do not restart a wait that the next roll has to sit out: once the roll
is over, the second detent turns the page at once — the next disc, or the
artist page. One detent does not. The same pair applies turning back a disc.
Leaving the artist page upward is that same stop: a continuous roll up the
biography settles on the portrait and stays there, and the up-arrow appears
for that rest. The next gesture — two detents after the roll has been quiet,
or the drag after the one that rested — returns to the last disc. Entering
the page does not show the arrow. The turn's own trailing notches are
absorbed, so one flick cannot walk through a short disc into the next page.
Nothing animates; the page flips at once.
The return leaves the album list on the last disc, at its end. `goBack()`
and Escape take one step (artist → album world); a second Escape closes the
album view to the grid. If there is no artist page and no further disc, the
list clamps and draws no triangle.
**Nothing in it takes a hover treatment** — a background that
lights up under the cursor is the wrong vocabulary for a page of prose. The
photo is the one clickable thing (it opens the art scene); its only intended
affordance is a cursor change, not a highlight. `rcAlbumText_` / `rcArtistImg_`
are live only here.

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

**The variant strip stays in album world**, after the last track, in the
scrolling column. Other copies of this record are not the artist page. When
the album belongs to a variant group — the same release also held as another
edition, at another quality, or as another remix set (see
`core/include/core/variants.h`) — a strip of tiles sits there, one per *other*
member. The album you are looking at is never in its own strip. The pinned
cover remaining visible above/beside them is allowed; album cover + artist
photo on screen together is not.

Its caption follows what the group actually is: **`OTHER VERSIONS`** for
albums, EPs and singles, **`MORE REMIXES`** on a remix page. "Other versions"
is empty words there — a remix already *is* a version, so the phrase adds
nothing; what is below is simply more of them.

They are **full-size covers** at `gridArtSize_`, wrapping to a second row when
the panel is too narrow, with the base name, the edition line and the artist
under each one. The album grid (§8.2) leaves those words off, because there
the picture is the shelf. Here the listener is choosing among copies of the
record already open, so the copy has to say which one it is. A variant *is*
an album, so the cover is drawn at grid size; a shrunken tile would say it
were a lesser thing. Drawing at grid size also puts the art at 1:1 with the
density `getGridArtTexture()` decoded it for. Two details of that caption:

- **No shrink-to-fit and no truncation.** Extra rows add to album-world
  content height and still snap by a track-row pitch — dropping a version
  defeats the whole strip.
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
like the artist photo, so they are cropped to the *list* viewport by the same
UV sub-rect trick, for the same reason — a tile scrolling under the pinned
cover must not paint over it.

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
The main Settings page fills the safe area in both orientations. Bar A and
bar B are not drawn there and do not hit-test, so the letter that opened the
page is not a way out. The header button says **Exit**. Opening this page
stops playback; leaving it does not start the music again, and play is refused
for the whole session. A sub-menu replaces that page, so Exit is gone and the
header button says **Return** (back to the main Settings page). The AutoEQ
switcher's route into the EQ catalogue is the exception: the rails stay, the
music keeps playing, and Return goes back to the player.

Exit and Return are the body Terminus strike, the same one the other Settings
buttons use. Terminus only lands on a fixed set of sizes (the 16 px strike
at an integer scale: 16, 32, 48, …). A fractional scale is grey pixels, so
it is refused. At the 1080 reference the body role is about 22 px, which
used to draw the 16 px strike and read small next to the serif on the music
page. `pickStrike` draws one step above that historical choice, so body and
secondary land on 32 and the header on 48. That is the only legal step up;
32 px of bitmap is larger than a 22 px serif em, and it is still the match.
`terminusTallerTarget` returns the height just drawn, which `pickStrike`
then takes one step further — group headings stay exactly one strike above
the names. The lift lives only inside `pickStrike`. Call sites do not add
a second one, and the music surface does not go through it at all. The
button box grows from the strike actually drawn, so the glyph is not
clipped by a box authored for a smaller label. A panel row is `space(44)`,
which still holds a 32 px cell.

Music Folders keeps **Remove Selected** on the left. Return is the way out,
and if a folder was removed it rescans — Escape and the back gesture take
that same path. There is no Done. Select Music Folder keeps **Select This
Folder** on the right, in the accent; Return leaves without choosing. There
is no Cancel. The two used to sit beside Return and do the same thing.

Themes is the page that picks the palette. The header says Themes on the
overview and the part name when one part is open. Return from a part
comes back to the four parts. Return from the overview leaves the page.

Under the header, a filled secondary button holds the mode. The label is
`STATIC MODE - CLICK TO SWITCH` or `DYNAMIC MODE - CLICK TO SWITCH`, with
`TAP` where the finger is the switch. Dynamic adds a second dim line
naming the palette on screen. The two lines are centered as one group on
their ink, inside a button at least `space(72)` tall. Static is the
default. A theme sets Static, saves `ui_theme`, and applies that palette.
The mode button saves only `ui_theme_mode` (`static` or `dynamic`;
anything else is static). Dynamic never writes `ui_theme`. It rests on
Matrix, and while audio is underway it builds a palette from the playing
cover. The page is the color that covers the most of the sleeve. The ink
is the sleeve's own light or dark pigment, hue kept. The accent is the mark the eye separates from the field. A red dot on
black qualifies. A night blue on that black does not: it stays the field.
Further marks stay recognizable as the album title, the mosaic fill, the
hairline, and the washes. On the transport bar the accent is the center
control, and previous and next stay the ink. A red in the picture becomes the
error, an amber the warning, and a quality mark takes the hue of a sleeve
color in its family. The result is kept only when the contrast rules
pass, including the album-title color on the page. A cover that cannot
carry that ladder keeps the nearest loaded palette for that listen. The built palette is stored in `cover_themes`, one row per
release, and the next play of the same cover file reads it back. The
row is keyed by folder name and artist, and it misses when the cover
file's size or mtime changes or the generator number changes. Playback
ending returns to Matrix. The Themes page names the release on the dim
line under the mode, and the stroke stays on the saved static id.
A `stroke(2)` rule in `CLR_TEXT_PRIMARY` around the swatch strip marks
the saved static id, so a live cover match does not move it.

The overview is four parts, in order: Morning, Afternoon, Evening, Night.
A part is the background's relative luminance. Above 0.93 is Morning,
above 0.45 down through 0.93 is Afternoon, above 0.03 down through 0.45
is Evening, and 0.03 and below is Night. A value sitting on a bound
belongs to the darker part. Built-in palettes are placed by that
luminance; their stored group words stay in the literals so a saved
`ui_theme` is untouched. An imported file is placed by its `group` line
when the word is one of those four, and by luminance otherwise. Inside a
part the order is A to Z by name, then id.

Each theme is its colors as squares, one per distinct palette color, page
first, up to the twenty slots. A square within `themeChannelDist` 48 of
the current page gets a hairline in `CLR_TEXT_DIM`. The square starts at
`space(22)` and shrinks toward `space(8)` so a full strip fits the width.
A narrow window shows fewer themes on a row. A wide window shows more.

The overview draws one row of strips per part, a sample chosen when
Themes opens and kept until the next visit, so Night does not push the
other three off the page. Names are hidden there. A `+` after the row,
on the next line when it does not fit beside the strip, opens that part
alone. The part page lists every theme, the name beside the strip in
primary ink, and the next theme on the same row when the pair fits.
Hover is `CLR_HOVER` behind the strip, and behind the name on the part
page. A group heading is one Terminus strike taller than the names,
primary ink, with a `CLR_TEXT_SECONDARY` rule under it. The wheel moves
one row.

At the bottom of the scroll, a secondary button says Import themes.
Choosing a folder copies the `.theme` and `.toml` files in that
directory, not its children, into `<state dir>/themes`. The copy does
not ask for a part: a file with a `group` line of morning, afternoon,
evening, or night keeps it, and any other word is placed by luminance
when the page draws. A file already there is left in place. The page
then reloads and returns to the four parts. A file that fails the
contrast rules, or reuses a built-in id, is still refused. A dim line
reads `N LEFT OUT` and `N ALREADY THERE` when either count is above
zero. Return from that folder picker lands on Themes.

`tools/theme_pack` does the same placement offline. `import` writes
`<id>.theme` into a destination folder and an `INDEX.txt` a person can
read. `export` copies the theme files and rewrites that index. A hand
edit of the group line is kept when import runs again without
`--replace`.

A `.theme` file, or an Alacritty file with `[colors.primary]`,
`[colors.normal]` and `[colors.bright]`, placed in `<state dir>/themes`
joins the list at startup and again when this page is opened. The
bundled set in `<exe dir>/themes-bundled` is loaded after that folder,
so a file the listener already has keeps its id. The minimum direct file
is `bg`, `text` and `accent` (`#RGB` or `#RRGGBB`). Optional keys
(`bg.track`, `text.secondary`, `text.dim`, `hover`, `separator`,
`error`, `warning`, the four `quality.*` marks, and the rest of the
token list) override the derived slots. `id`, `name`, `group` and
`blurb` name the row. Names stay ASCII: Terminus folds a codepoint it
cannot draw to `?`. The original eight ids are unchanged.

Full-page overlays. Shared `panels::drawHeader` (Terminus title + Return, or
Exit on the main page) and
`panels::drawButton`, both at `UI_CORNER_RADIUS`. A primary button is the
accent with the page color as its label; hover steps that accent back toward
the page. A secondary button steps the page toward the primary text (14% at
rest, 24% on hover), so a blue page stays blue and a paper page stays paper.
Row lists via
`panels::drawTerminusScrollList` and radio groups via
`panels::drawTerminusRadioRow` — Terminus, same selection family
(`matrixListStyle()` / `matrixRadioStyle()` colours). Search via
`panels::drawTerminusSearchField`. The vk_canvas `widgets::drawScrollList` /
`drawRadioRow` stay for Playlists and other music-facing lists.

**Settings is a Terminus surface, ALL CAPS** — titles, buttons, radio labels,
descriptions, file-manager rows, EQ names, search, toggles. The radio
(Audio Output, and the Bluetooth codec rows) and the Interface switches are
bitmap marks on that same grid: a 13px ring, a dot inside it when the row
is chosen, and a pill whose rounded ends are that ring. Sizes come
from the same type roles as the rest of the app (`header` / `body` /
`secondary` in `ui_metrics.hh`). Advance is optical (ink box + 1 px),
not the monospace cell, so `AAUDIO` does not open a hole around I.
`terminusFold` / `terminusWrap` / `terminusEllipsize` /
`drawTerminusText` (`gui/src/terminus_glyph.hh`). Unrepresentable
codepoints fold (em dash → `-`, middle dot → `/`) or become `?`; there
is no Computer Modern inside the Settings panels. List inset matches the
captions (`SP_LG`), not `canvas.pad()`. Terminus is one SDF-shape quad
per pixel-run; the shape VBO in `vk_canvas` grows rather than truncating
at 2048 quads (that ceiling is what sliced the last letters of a line
and hid every EQ row past the first two, and what turned the transport
bar black when a longer Audio page ate the budget). OverlayRasterizer's
32768 / 256-per-tile curve cap is the compute-path ceiling, unused while
`useShapes()` is on. `panels::drawHeader`/`panels::drawButton` take
`terminusChrome = false` by default so EqSwitcher and the signal chain
stay serif. Audio Output and EQ dock action buttons below a clipped,
scrollable body; the EQ catalogue scrolls inside the list. EQ
Every-source rows use ASCII `" / "` between name, source and rig.

**One rendering trap this exposed:** Terminus draws as raw overlapping
rectangles (run-length fills), not through the MSDF text layer plain
`canvas.text`/`textStyled` calls use. Two identical labels drawn at the exact
same position — harmless when both went through the text layer — can leave a
later character incompletely drawn when both are Terminus rects. This is how
a **pre-existing** redundant double-draw of Interface Settings' Close button
(drawn once inline, once again by the shared panel dispatcher) was found and
removed; see `drawInterfaceSettings`'s closing comment.

**The primary button is right-anchored in all four panels.** A page with one
action fills the strip, so the click lands anywhere along the dock; a page
with several keeps the primary as the rightmost of that row. EQ Settings used
to lay its row out left-to-right from `content.left`, making it the one page
of four where the green button changed sides. Mouse muscle memory is learned
per app; a primary that moves between sibling pages cannot be learned.

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
(`panels::drawScrollbar`). `themeRule()` track, `CLR_TEXT_SECONDARY` thumb —
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
   square). Settings radios and switches are bitmap marks on the Terminus
   grid, not a private radius. The tile glow is the remaining own-radius
   shape (§8.2).
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

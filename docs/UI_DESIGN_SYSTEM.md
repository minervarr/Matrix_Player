# Matrix Player — UI Design System

This document is the written map of Matrix Player's visual language. The GUI is
**fully custom-rendered** — no OS controls anywhere — through the first-party
`vk_canvas` Vulkan engine: SDF rounded rectangles, analytic triangles (for
icons), and MSDF text. Almost everything is drawn in one place,
`PlayerWindow::drawFrame()` (`gui/src/player_view.cc:745`), with the fullscreen
album-art window in `gui/src/art_view.cc` and the shared settings-panel widgets
in `gui/src/panels/settings_panels.cc` (+ the reusable
`framework/vk_canvas/core/widgets.*`).

`file:line` references throughout point at the source of truth. When you change
the look, update both the code and this doc.

---

## 1. Design principles

1. **Custom-rendered, no OS chrome.** Every pixel is drawn by the app. There are
   no native buttons, lists, scrollbars, or dialogs — settings are full-page
   overlays, not modal windows.
2. **Dark, serif-typographic, single-accent.** A near-black stack of surfaces,
   Latin Modern (a serif) for all text, and exactly one accent — a vivid green
   (`CLR_ACCENT` `rgb(0,200,83)`).
3. **Square throughout.** Artwork, structural surfaces, and interactive chrome
   (buttons, hover/selection highlights, search fields) all use square corners
   (`UI_CORNER_RADIUS = 0`). The only rounded shapes are the circular radio dot
   and the decorative tile glows. Selection/hover highlights fill the full row
   height (matching the action-button height) and, in lists/radios, hug their
   text.
4. **Accent = state, hover = neutral.** Green *only* signals state — focus,
   selected, active, playing. Merely hovering something is always a **neutral
   grey** treatment, never green. This is the rule that keeps the accent
   meaningful.
5. **Static and instant by design.** There is no animation. The UI redraws on
   dirty and flips between discrete states immediately (see §9).
6. **Resolution-robust, not pixel-pinned.** Text sizes are a percentage of
   window height with a hard legibility floor; all other geometry scales by one
   proportion factor, `uiScale_` (see §3, §4).

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
| `CLR_TEXT_PRIMARY` | 242 | Primary text |
| `CLR_TEXT_SECONDARY` | 128 | Artist, time, duration |
| `CLR_TEXT_DIM` | 140 | De-emphasized: badges, hints, placeholders |
| `CLR_TEXT_ALBUM_TITLE` | 255 | Grid album title (pure white) |
| `CLR_ACCENT` | 0,200,83 | Brand + all state (focus/selected/active/playing) |
| `CLR_HOVER` | 38 | Neutral hover pill / focus frame |
| `CLR_SEPARATOR` | 36 | 1px hairline separators, unfocused input underline |
| `CLR_INPUT_BG` | 24 | Search / text-field fill |
| `CLR_TILE_PLACEHOLDER` | 28 | Album-art placeholder fill |
| `CLR_ERROR` | 220,70,70 | Reserved for error UI (not yet drawn) |

**Quality-color tier (a second, scoped palette).** Album art borders and
track-list "auras" (§8.2/§8.4) are colored by objective audio quality, not
UI state — this is the one deliberate exception to "one palette":

| Token | RGB | Tier |
|---|---|---|
| `CLR_QUALITY_DSD` | 255,255,255 | DSD |
| `CLR_QUALITY_DXD` | 255,165,0 | >=352.8kHz (DXD) |
| `CLR_QUALITY_HIRES` | 0,255,255 | >=64kHz (hi-res PCM) |
| `CLR_QUALITY_STANDARD` | 255,255,0 | >=44.1kHz (CD quality) |

Below 44.1kHz, no border is drawn. `qualityColorFor(sampleRate, isDsd)`
(`gui/src/theme.hh`) is the single place this mapping lives.

**Contrast note:** `CLR_TEXT_DIM` was deliberately raised 80→140 so real
information (badges/hints) clears WCAG AA (~5.6:1 on the base background) — see
the comment in `theme.hh`.

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

**Model.** Every text size = `max(pct × windowContentHeight, floorPx)` via
`ResponsiveTextScale` (`framework/vk_canvas/core/responsive_text.hh`). The floor
`kMinReadableTextSizePx` (~18.29px) is generated at build time from the shipped
font's thinnest-stroke geometry (`ui_min_text_size.gen.h`) — below it, hairlines
in the serif faces disappear.

**Roles** (`gui/src/player_view.hh`; percentages are `numerator/661`):

| Role (`textSizes_.*`) | ~px @661 ref | Used for |
|---|---|---|
| `badge` | 11.5 | BITPERFECT / REF EQ / quality badges |
| `secondary` | 11.5 | artist, time, duration, search text |
| `body` | 12.5 | grid album title, track number |
| `nav` | 13.0 | nav items, settings rows, track title, brand |
| `transportTitle` | 14.0 | now-playing title |
| `trackPanelTitle` | 16.0 | album-view title |
| `header` | 17.0 | page/panel headers |

Computed each `recalcLayout()` (`player_view.cc:1302`). **`uiScale_ =
textSizes_.nav / 13`** is the single factor multiplying every fixed-pixel layout
value (§4).

**Font styles** (`FontStyle`: Roman / Bold / Math / Italic):
- **Bold** — titles and headers (brand, album titles, page headers, playing row).
- **Italic** — artist lines, placeholders, empty-state hints.
- **Math** — repurposed to Latin Modern **Mono** for numeric readouts (track #,
  duration, time, quality badge) so digits don't jitter as they change.
- **Roman** — everything else.

**Faces.** Base is Latin Modern (`fonts/lm/*`, serif). Script fallbacks match the
serif lineage: New Computer Modern for Cyrillic/Greek (baked eagerly), and
Fandol Song / Harano Aji Mincho / UnBatang for CJK/Japanese/Korean (baked lazily
from scanned metadata). The art window mirrors the same mapping.

**Overflow helpers** (`framework/vk_canvas/core/text_util.hh`, UTF-8-safe):
`truncateToWidth` (prefix + …), `splitTwoLines` (two-line grid titles),
`wrapText` (album-view prose), `fitTextSize`, `textCenteredStyled` (MSDF-correct
centering).

---

## 4. Spacing & layout

**Spacing scale** (`theme.hh`; base px, multiply by `uiScale_`):
`SP_XS 4 · SP_SM 8 · SP_MD 12 · SP_LG 20 · SP_XL 40`. Prefer these over ad-hoc
literals. (Adoption is in progress — the settings panels use them; some main-UI
pads remain literals, tracked as follow-up.)

**Regions** (`recalcLayout()`, `player_view.cc:1295`; `us = uiScale_`):
- Transport bar: full width, bottom, height `80·us`, bg `CLR_BG_TRANSPORT`.
- Sidebar: left, width `max(170, 170·us)`, bg `CLR_BG_SIDEBAR`, 1px right hairline.
- Grid / content: remaining area, bg `CLR_BG_MAIN`.
- Track panel: when an album is open it **replaces** the grid (same rect), bg
  `CLR_BG_TRACKPANEL`; settings are full-page overlays in the same area.

**Grid tiles:** columns from a ~250px target pitch (clamped 2–8),
`gridArtSize_ = max(80, gridW/cols − 30)`; square art + centered title + artist.

**Row heights** (three, by context): `kPanelRowH 44` (settings list rows),
`trackRowHeight_ 40·us` (album-view tracks), `34·us` (settings-page inline rows,
backend radios, search fields). Documented rather than merged — each suits its
density.

---

## 5. Shape & radius

- **One radius token:** `UI_CORNER_RADIUS = 0` — the whole app is square
  (buttons, hover/selection highlights, search fields, grid hover frame). This
  single token enforces it; nudge it up if a softer look is ever wanted.
- **Exceptions (own radii):** the circular radio dot (`dot.w*0.5`) and the
  decorative multi-layer now-playing tile glow (10/12px, §8.2).
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

Icons are **vector primitives** (triangles + rounded rects), not images or
glyphs — crisp at any size, zero VRAM. Defined in `drawUiIcon`
(`player_view.cc:630`) on a 36-unit box scaled into the button:

| Icon | Construction |
|---|---|
| Play | one right-pointing triangle |
| Stop | one rounded square (radius `2/36`) |
| Prev | left bar + left-pointing triangle |
| Next | right-pointing triangle + right bar |

Color is passed per button — normally `CLR_TEXT_PRIMARY`; the idle **Play** is
`CLR_ACCENT` (a call to action). Two identical button banks use them: the
transport bar and Essential mode. There are no other icons.

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
Nav items *Albums* / *Settings*: hover = grey pill; active = accent-tint pill +
left bar + accent label.

### 8.2 Album grid
Square art tiles (placeholder = flat `CLR_TILE_PLACEHOLDER`) + Bold white title
(two-line fallback) + italic secondary artist. **Hover** = neutral grey focus
frame. **Selected** = accent ring (0.80). **Now-playing** = three-layer accent
glow (0.20/0.45/1.0 at radii 12/10/8). A thin accent bottom bar marks
**last-played only** (now-playing is already unmistakable from the glow — no
double-marking). Empty states: "No albums yet…" / "No matches for …".

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

### 8.7 Fullscreen art window (`art_view.cc`)
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
6. **Spacing from the scale.** Prefer `SP_*` over new literals; keep everything
   `× uiScale_`.
7. **Text through the role scale.** Use a `textSizes_.*` role + the right
   `FontStyle`; don't hardcode a pixel size (except the floored fallbacks).

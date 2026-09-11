# Bar A: a rail you cannot mis-tap, and an EQ switcher you can read

*2026-09-11*

## What changes, in one sentence

Bar A stops separating its cells by position and shades of grey and starts
separating them by **typeface and weight** — Settings in Terminus, Find in
regular Computer Modern, the filters in bold — with the two rare cells moved
together to the near end, and the AutoEQ quick-switcher becomes a readable
list that takes over the content area instead of unfurling along the bar.

## Why the current rail has to change

Measured on the phone the reports come from (moto g06, 720x1640), bar A reads
`S | × No AutoEQ | S A E S C L R P`:

```
near [Settings][ × | profile name ] · · · gap · · · [Search][A E S C L R P] far
```

1. **Search sits in the letter row.** It is the cell immediately next to
   Albums and the same size as a filter letter, so a slightly-off tap on
   Albums opens search. Opening search collapses all seven letters, so the
   recovery is a trip to the `×` at the far end. Search is also the one cell
   the listener uses *least* of the four things on this bar (filters,
   playlists, EQ switching and search — asked, not assumed).
2. **Three cells say "S".** Settings, Search and Singles are told apart only
   by the text ladder in `bar_a.cc` — PRIMARY 242, SECONDARY 170, DIM 128 —
   and that ladder is already full: 128 is the WCAG floor.
3. **The AutoEQ list is unreadable.** It unfurls ALONG the bar, so each saved
   profile gets a segment the size of a filter letter. Rendered at the phone's
   size with one saved profile, the name draws as `S…` — and sideways, in the
   horizontal layout. The listener switches between three to five profiles,
   often; with five, each would get even less room. A 130-unit bar has no space
   to fix this in place.

## The rail

```
near [S][F] | active profile name | · · · gap · · · [A E S C L R P] far
      │  │                                            └ filters: Computer Modern BOLD
      │  └ Find: Computer Modern regular
      └ Settings: Terminus
```

- **Rare cells together at the near end, frequent cells alone at the far
  end**, with the gap between them. A mis-tap on Albums can no longer reach
  search. Settings stays pinned at the very near end and never moves — the rule
  `rail_layout.hh` already states, because opening search must not cost what
  was typed.
- **Find is the letter `F`.** No filter uses F, so it cannot be mistaken for
  one. The Search `S` disappears.
- **The cell count does not change**: two near cells plus seven letters, where
  today it is one plus eight. The ordinary vertical bar therefore shrinks no
  further than it already does.
- **An empty search closes by itself.** Tapping anywhere outside the field
  while it holds no text and no chips calls `closeSearch()`. A search with
  anything in it stays open — nothing typed is ever discarded by a stray tap.
  With search open the letters and the EQ box still hide and a close button
  still takes the far end, exactly as today.
- **The EQ box shows the active profile's name and nothing else.** Its `×`
  ("No AutoEQ") is removed; "No EQ" becomes the first row of the list below.
  In bit-perfect mode the box does not exist, as today.

### Typography, and why it replaces the grey ladder

| cell | face | weight | colour (unchanged) |
|---|---|---|---|
| Settings `S` | Terminus | bold | DIM 128 |
| Find `F` | Computer Modern | regular | SECONDARY 170 |
| filters `A E S C L R P` | Computer Modern | **bold** | PRIMARY 242 |

The filters get the heaviest weight because they are the most used, and bold
holds up better in the shrunken cells of the vertical bar. The colour ladder
stays, but it is now the *second* signal rather than the only one: the only
remaining pair of identical letters is Settings and Singles, and a 1-bit pixel
`S` beside a bold serif `S` do not resemble each other at any size.

Settings gets Terminus because it is machine chrome, not music — the same face
Economycs sets everything in.

### Terminus is a bitmap font, so it is baked, not loaded

Terminus ships as `.otb` bitmap strikes (12–32 px). It cannot enter this app's
text engine: `RasterFont` bakes from outlines, and `FontStyle` has exactly four
slots — Roman, Bold, Math, Italic — all in use. So the glyph is handled the way
Economycs handles its whole font (`gui/src/term.hh` there):

- `tools/terminus/bake_glyphs.py` reads `ter-u16b.otb` and `ter-u32b.otb` with
  Pillow and writes `gui/src/terminus_glyphs.gen.h`: one bit per pixel, the
  glyphs this app draws (today only `S`). The two `.otb` sources and Terminus's
  `LICENSE` (SIL OFL 1.1) are copied into `tools/terminus/`, so the bake does
  not depend on another repository or on `/usr/share/fonts`. **The generated
  header is committed**, exactly like `ui_icons.gen.h`, so the C++ build never
  needs Python.
- `gui/src/terminus_glyph.{hh,cc}` turns a glyph's bit rows into run-length
  rectangles and draws them with `canvas.rect()`. No atlas, no texture, no
  sampler — nothing a 1-bit glyph needs. Draws identically on all three
  platforms.
- **Integer scale only, nearest sampling**, the rule from Economycs's
  `docs/type-hierarchy.md`: a fractional scale resamples pixels into grey and
  breaks a 1-bit face. The strike and factor are chosen from the cell's target
  height -- the size the filter letters are drawn at, so the three cells
  match: the 32 px strike at `k = max(1, round(target / 32))`, or the 16 px
  strike when the target is under 24 px. The glyph is centred in its cell.

## The EQ switcher

Tapping the EQ box replaces the album grid with a list — a new
`ContentOverlay::EqSwitcher` scene, using the same "replace the content, never
float over it" mechanism the artwork and the signal chain already use (the
renderer emits every rect before every glyph, so an overlay cannot cover text
drawn earlier).

```
  No EQ
> Sony WH-1000XM4            <   active: accent tint
  Truthear Zero RED
  Sennheiser HD 650
  Moondrop Chu II
  All profiles…                  opens the EQ Settings panel
```

- **Rows are full width and full name**, with touch-sized rows
  (`space(65)`, the album view's track row).
- **Order is the existing saved-list order**: pinned, then most used, then most
  recent (`loadEqHeadphones`). The active row is marked with the accent tint —
  accent is state, never hover (`UI_DESIGN_SYSTEM.md` §1.4). A tentative profile
  (selected, not yet credited) keeps the SECONDARY colour it has today.
- **Tapping a row applies it and returns to the grid**: `selectEqProfile()` for
  a profile, `clearEqProfile()` for No EQ.
- **"All profiles…"** opens the existing EQ Settings panel, where the catalogue
  is searched and saved rows are pinned or removed. This list is for switching
  only.
- **Closing without a change**: Back / Escape (`goBack()` already closes scenes),
  or tapping the EQ box again.
- **The bars keep working while it is open**, because it is a scene and not a
  panel. Tapping a filter letter closes the list and switches the filter, the
  same way the bars already behave over the artwork.

## What is removed

- The along-the-bar list: `RailInput::eqListOpen`, `RailLayout::eqList`,
  `railListRow()`, the row drawing and hit-testing in `bar_a.cc`, and the
  `BarAItem::EqRow` / `EqNone` hit items.
- The `×` in the EQ box, and `RailLayout::eqNone`.
- The colour ladder's role as the only thing separating the three `S` cells —
  the ladder itself stays.

## Testing

- **`rail_layout_test`** — anchors updated for the new order, in all states
  (two orientations × bit-perfect × search open). It must still assert that
  horizontal is vertical rotated 90° counter-clockwise, rect by rect. New
  assertion: Settings and Find are in the near cluster, and the gap separates
  Find from the first filter letter.
- **`terminus_glyph_test`** (new, pure) — the run-length rectangles cover
  exactly the set bits of the baked glyph and nothing else, at scales 1, 2 and
  3; and the strike/scale choice is an integer factor for every target height.
- **`ui_capture`** — a new `45-eq-switcher` state. Like the grid work, it is
  checked with `--fixture 60` at `--frame 720x1640` and `--frame 1640x720`,
  because the default captures run on an empty library. The existing rail states
  will change and are checked by eye: `30-settings`, `40-search`, `41-search-suggest`,
  `42-search-chips`. `43-autoeq-unfurled` is **retired**: it opens the
  along-the-bar list, which no longer exists, and `45-eq-switcher` replaces it.
- `docs/UI_DESIGN_SYSTEM.md` and `CLAUDE.md` updated: the rail section, the
  three-`S` note in `bar_a.cc`, and `CLAUDE.md`'s rule 6 about the unfurled list.

## Not in scope

- A search magnifier or any new SVG icon — typeface and weight carry the
  distinction instead. An icon can come later without changing this layout.
- Short nicknames for saved profiles, and gesture cycling between them.
- Terminus anywhere other than the Settings cell.

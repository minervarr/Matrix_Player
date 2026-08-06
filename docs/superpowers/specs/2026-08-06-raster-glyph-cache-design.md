# Replacing MTSDF with a per-size rasterized glyph cache

## The problem, stated exactly

Most of the library renders blank. The cause is atlas capacity, not fidelity.

`MsdfFont` bakes at `sizePxEm_ = 96` with `distanceRange_ = 9.6`, which makes a
CJK cell roughly 115×104 px. A 4096² sheet therefore holds about 1,300 glyphs,
and Latin + Bold + Italic + Mono + the icon set are already in it. From the
capture run's own log:

```
MSDF baked 359 fallback codepoints from fandol/FandolSong-Regular.otf: atlas now 4096x3946
E/Msdf: MSDF atlas height 4163 exceeds the 4096 guaranteed texture limit   <- Japanese, rejected whole
E/Msdf: MSDF atlas height 5609 exceeds the 4096 guaranteed texture limit   <- Korean, rejected whole
```

Chinese fit by luck, being first in the fallback order
(`PlayerWindow::ensureFallbackGlyphs`: Chinese → Japanese → Korean). Japanese
and Korean baked **zero** glyphs. One cause explains every reported symptom:
blank track names, album titles showing only their ASCII tail, no Hangul
anywhere, gaps in Japanese.

The requirement this violates ranks above fidelity: **a text method that only
works for some languages is disqualified.** So MTSDF is replaced in Matrix
Player rather than tuned.

## Why the replacement is comfortable

Measured against the real library, not assumed.

Distinct codepoints across all album/track metadata: **485 total** — Han 166,
Hangul 158, Latin 104, Cyrillic 50, Kana 3.

Distinct text sizes: **four**. `ui_metrics.hh` derives five roles from
`kMinReadableTextSizePx × kUiTypeRatio^n`, and `caption`/`secondary`
deliberately share a size (they are separated by the color ladder and by
style, not by size). `scale = max(1, H/1080)`, so one display yields one set of
four.

485 codepoints × 4 sizes × ~2 styles ≈ **4,000 cells at ~26 px** ≈ 2.8 M
pixels — about one sixth of a single 4096² sheet, in R8 rather than RGBA. The
whole sheet is 16 MB against the 67 MB MTSDF cache file that exists today. The
capacity ceiling does not get raised; it stops being the binding constraint.

## Architecture

A new `RasterFont` **alongside** `MsdfFont` in
`framework/vk_canvas/first_party/vulkan_font_engine/core/`. Nothing is deleted:
MTSDF stays for vk_canvas's other consumers, which magnify glyphs continuously
and are the case MSDF is genuinely right for.

### 1. `glyph_raster.{hh,cc}` — the rasterizer

FreeType `FT_Render_Glyph(FT_RENDER_MODE_NORMAL)` with `FT_LOAD_NO_HINTING`, at
an exact ppem, producing 8-bit coverage. **Unhinted on purpose**: hinting moves
the outline, which is a distortion, not fidelity.

Kept dependency-light (FreeType only — no msdfgen, no Vulkan) so it can carry a
plain `assert()` test in `core/tests/`, the same way `gpos_kern.cc` does.

### 2. `RasterFont` — the drop-in

Mirrors only the `MsdfFont` surface that `Canvas` and `player_view` actually
call: `layout`, `layoutByKey`, `textWidth`, `advance`, `lineHeight`,
`emitGlyph`, `bakeCodepoints`, `hasCodepoint`, `hasStyle`. Switching is then a
type swap in `player_view.hh` and `art_view.hh` rather than a rewrite of the
drawing code.

Glyphs are keyed `(style, sizePx, codepoint)` and baked **on demand from what
is on screen**, so a library far larger than this one never bakes what it never
shows.

### 3. What carries over unchanged

- **`gpos_kern.cc`.** Pair adjustments are a table in em units, independent of
  how a glyph is drawn. The landed kerning work is reused verbatim.
- **Linear-light compositing.** `msdf_frag.slang` gains a `raster` mode where
  coverage is `tex.r` directly, feeding the *same* linear-light premultiplied
  composite and the same `srcColorBlendFactor = ONE`. The landed gamma work is
  reused verbatim.

Both were architecture-independent when they landed, and they stay that way.

### 4. Atlas

R8, shelf-packed, grown lazily. Eviction is deliberately **not** built now: at
4,000 cells of a 21,000-cell budget there is nothing to evict, and an LRU whose
pressure path never executes is an untested branch pretending to be a feature.
The on-demand bake is the mechanism that makes a bigger library safe; if a real
library ever fills the sheet, eviction is added then, with a case to test it
against.

### 5. No disk cache

At 10–50 µs/glyph, 4,000 glyphs is 40–200 ms once at startup. MTSDF's 1–10
ms/glyph is the only reason its cache file exists. Dropping it also drops the
cache version word, the icon-set fingerprint embedded in the filename, and the
entire class of stale-bake bugs — including the v9→v10 repack that made an A/B
of two installed builds look like an architectural difference when it was a
reshuffled overflowing atlas.

### 6. Positioning

Whole-pixel pen snapping. The pen stays in float so kerned advances accumulate
exactly, and rounds only at emit — which keeps `textWidth()` and the emitters in
agreement. That agreement is the invariant centered and right-aligned labels
depend on; it is the thing to assert, not to eyeball.

No MTSDF fallback inside Matrix Player: no text in this UI scales
continuously, so the case that would need one does not arise.

## Scope

Everything switches, one path: `PlayerWindow`, `ArtWindow`, and the UI icon
glyphs (`ui_icons_draw.cc` reads `Canvas::msdfFont()`). Matrix Player ends up
genuinely MTSDF-free — one atlas, one shader mode, one code path.

ArtWindow draws large text, and large cells are the expensive case for a
per-size cache. The budget above has room, but this is measured after the
switch rather than assumed: if ArtWindow's sizes prove costly, that is a number
to report, not a reason to keep two pipelines.

## Verification

The gate is `matrix_ui_capture` against the **real** database — 85 albums, 833
tracks, and the multi-script tail this design exists for.

Two capture states were added for exactly this and are already committed
(`fd95bc0`):

- **`17-grid-multiscript`** — the album grid scrolled to the end. Latin sorts
  first, so on a real library every Han/Hangul/Cyrillic title lands in the last
  8 of 47; the first screenful is all Latin and says nothing. This is how the
  bug survived a full session of captures unnoticed.
- **`21-album-view-multiscript`** — the album view of a Korean record, chosen
  by content (Hangul preferred over Kana over Han, because the fallback bake
  order means Korean is the first script to disappear and the last to return).
  Track titles are the smallest type role in the app, where a missing glyph
  draws as an empty row rather than a wrong-looking one.

The current shots are the before-baseline: `21-album-view-multiscript` shows
tracks 3, 5 and 6 as completely blank rows.

Success is those two shots rendering every script, with no `exceeds the 4096
guaranteed texture limit` line in the log.

Alongside them, unchanged:

- `ui_metrics_test`, `ui_icons_test`, and a new `glyph_raster` test.
- A centered-label check, proving `textWidth()` still matches the emitters.
- The 8K-downsample ground truth (`--frame 7680x4320` box-downsampled by 4 is
  an exact reference for the 1080p render, since `scale = max(1, H/1080)`) for
  any fidelity claim. It already killed two plausible improvements; it stays
  the arbiter.

## Deliberately out of scope

- **Removing MTSDF from vk_canvas.** Other consumers magnify glyphs
  continuously, which is what MSDF is for.
- **Subpixel-positioned glyph variants.** Whole-pixel snapping first; variants
  only if measurement asks for them.
- **`ensureFallbackGlyphs` scanning `Album::displayName` and `Track::album`.**
  It scans `Album::name`, which is the raw folder key — an opaque hash in a
  downloader-managed library. A codepoint appearing *only* in an album title
  and in no track title would never be baked. It does not bite today (track
  titles cover every script present) and the scan may not survive the rewrite,
  so it is recorded here rather than fixed in passing.

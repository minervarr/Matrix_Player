# Terminus

The Settings cell in bar A is drawn in [Terminus](https://terminus-font.sourceforge.net/),
a **bitmap** font, under the SIL Open Font License 1.1 (see `LICENSE`).

Terminus cannot go through the app's text engine: `RasterFont` bakes from
outlines, a bitmap strike has none, and `FontStyle`'s four slots are all in use.
So the handful of glyphs the UI needs are baked here, one bit per pixel, into
`gui/src/terminus_glyphs.gen.h`, and drawn by `gui/src/terminus_glyph*.cc` as
run-length rectangles at an **integer** scale — the way Economycs draws its
whole interface. A fractional scale would resample pixels into grey, which is
exactly what a 1-bit face must not have.

## Files

- `ter-u16b.otb`, `ter-u32b.otb` — the bold strikes, copied from the Terminus
  distribution so the bake depends on nothing outside this repository.
- `LICENSE` — the font's licence, which travels with it.
- `bake_glyphs.py` — the bake.

## Regenerating

```sh
python3 tools/terminus/bake_glyphs.py      # needs Pillow
```

To draw another character, add it to `GLYPHS` in the script, rerun, and commit
the regenerated header. The header is committed, so a normal C++ build never
needs Python.

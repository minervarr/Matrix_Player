# UI icon font

Icons are authored as SVGs here and packaged into `matrix-icons.otf`, which is
baked into the app's shared MTSDF atlas alongside the UI text. Drawing an icon
is then one more text quad in a pass that already runs every frame — arbitrary
curves, tinted at draw time, no extra GPU pass, no texture uploads, and none of
the per-frame Bézier cost that makes real-time vector rendering expensive.

Same idea as Apple's SF Symbols, which ships as a font for the same reasons.

## Editing an icon

1. Open the `.svg` in `icons/` with Inkscape and draw.
2. **`Path → Object to Path`** — converts shapes/text to real outlines.
3. **`Path → Stroke to Path`** — strokes are not outlines; unconverted strokes
   simply vanish from the bake.
4. **`Path → Union`** — see the warning below. This one is not optional.
5. Save (plain or Inkscape SVG, both work).
6. Rebuild:
   ```sh
   pip install fonttools svgelements
   python3 build_icon_font.py
   ```
7. Rebuild and run `matrix_player`.

Both generated outputs are committed, so a normal C++ build never needs Python.

**You do not need to clear the atlas cache.** The generator emits a fingerprint
of the baked geometry into `ui_icons.gen.h`, and that fingerprint is part of the
cache filename (`gui/src/ui_fonts.hh`), so any icon change automatically misses
the old cache and re-bakes. Superseded caches are deleted on the next launch.

This matters more than it sounds: the bake is gated on `hasCodepoint()`, which
only knows whether a codepoint *exists*, not whether its artwork changed. Before
the fingerprint, editing an icon silently kept the old glyph — changing the box
sizes once left the gear and warning rendering **3.3× oversize**, spilling out of
their rectangles, and the only cure was knowing to delete a file by hand.

## Union is mandatory

This build sets `MSDFGEN_USE_SKIA=OFF`, so msdfgen has **no overlap resolver**.
Overlapping sub-paths corrupt the multi-channel correction and show up as torn
edges at the seams. Every icon must be one clean, non-self-intersecting outline.

`build_icon_font.py` warns when two contours' bounding boxes partially overlap,
which catches the common case — but it is a heuristic, not a proof. Run
`Path → Union` and don't rely on the warning.

Holes are fine and need no special handling: a contour fully nested inside
another becomes a cut-out. msdfgen's `orientContours()` works this out by
scanline nesting parity, so the winding direction you draw in doesn't matter.
The warning icon's "!" is exactly this.

## Design rules

- **One colour per icon.** MTSDF stores coverage, not colour — the app picks
  the colour at draw time (`CLR_TEXT_DIM` idle, `CLR_ACCENT` active, …). Fills
  and strokes in the SVG are ignored; only the outline is read.
- **Square canvas.** The whole document box maps onto the glyph's em box, so
  every icon shares one coordinate frame and keeps its relative size. A
  non-square document is letterboxed and centred rather than stretched.
- **Draw at the size you want relative to the frame.** Because the box (not the
  ink) is what maps onto the button, an icon drawn small inside its canvas
  renders small. The current set uses a 36×36 canvas, inherited from the
  primitive icons it replaced.

## Adding a new icon

1. Drop `icons/<name>.svg` in.
2. Add `("<name>", "<Enum>")` to `ICONS` in `build_icon_font.py` — **append
   only**. Codepoints are assigned by position, so renumbering would point a
   stale atlas cache at the wrong artwork.
3. Add the enumerator to `UiIcon` in `gui/src/ui_icons.hh` and its `case` in
   `gui/src/ui_icons.cc`.
4. Add it to the loop in `gui/src/ui_icons_test.cc` so the mapping is covered.
5. Rebuild the font, then the app.

## How it stays sharp — the design box

Each icon is authored inside a square **design box** measured in ems, set
per-icon in the `ICONS` table. That box is the resolution knob: atlas cell size
derives from the outline's own bounds, so an N-em outline is baked at N × 96px.

**Bigger is not better.** A bake much denser than the size an icon is actually
drawn at gets heavily *minified*, and the shader takes a single bilinear tap per
pixel — it smears. This is not hypothetical: a first pass put every icon in a
4-em box (384px cells), which left the sidebar gear at **12.8×** and the warning
at **10.4×** minification, and both looked visibly blurry. `msdf.cc:407`
documents the identical trap for text.

So match the box to where the icon is really drawn (sizes from
`player_view.cc`, authored at the 1080 reference, ×4 on an 8K display):

| Icon | Drawn at | Box | Cell | Worst case |
|---|---|---|---|---|
| play / stop / prev / next | 71–284px (960 essential @8K) | 2 em | 192px | 2.7× min → 1.5× mag |
| settings | 30–120px | 1 em | 96px | 3.2× min → 1.25× mag |
| warning | 37–148px | 1 em | 96px | 2.6× min → 1.5× mag |

Aim to keep both ends inside roughly 0.5×–3×. Total cost is ~3.3 MB of atlas.
`ui_icons_test` bounds every box on **both** sides — too small softens under
magnification, too large smears under minification.

If you add an icon drawn at a very different size from these, give it its own
box rather than reusing a neighbour's.

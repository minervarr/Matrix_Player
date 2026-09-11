#pragma once

// ── One Terminus glyph, drawn as pixels ─────────────────────────────────────
//
// Terminus is a BITMAP font: strikes at fixed sizes, every pixel exactly on or
// off. It cannot enter vk_canvas's text engine, which bakes from outlines and
// whose four FontStyle slots are all taken, so the glyphs the UI needs are
// baked by tools/terminus/bake_glyphs.py into terminus_glyphs.gen.h and drawn
// here as run-length rectangles -- one rect per horizontal run of set pixels.
//
// INTEGER SCALE ONLY, nearest sampling. A fractional scale resamples pixels
// into grey, and a 1-bit face with grey in it is a blurred face. Economycs's
// docs/type-hierarchy.md states the rule; this follows it.
//
// The geometry half is pure (terminus_glyph.cc, tested by terminus_glyph_test);
// the Canvas half is terminus_glyph_draw.cc -- the ui_icons.cc split.
#include <cstdint>
#include <vector>

#include "layout_rect.hh"          // framework/app_shell, same as ui_icons.hh
#include "terminus_glyphs.gen.h"

// Forward-declared exactly as ui_icons.hh does, so the pure half and its test
// never include canvas.hh.
class Canvas;
struct Color;

namespace terminus {

struct Pick {
    const Glyph* glyph = nullptr;   // null: this character was not baked
    int          scale = 1;
};

// The strike and integer scale for `cp` drawn about `targetPx` tall: the 16 px
// strike at x1 below 24 px, otherwise the 32 px strike at round(target / 32).
Pick pickStrike(char32_t cp, float targetPx);

// Device-pixel rectangles relative to the glyph's top-left, one per run of set
// pixels in a row, each `scale` tall.
struct Run { int x, y, w, h; };
void runs(const Glyph& g, int scale, std::vector<Run>& out);

} // namespace terminus

// Draw `cp` centred in `rc`. The origin is FLOORED so every edge lands on a
// whole pixel -- the shape path antialiases a fractional edge, and a grey
// pixel is precisely what a bitmap face must not have. False if not baked.
bool drawTerminusGlyph(Canvas& c, const LayoutRect& rc, char32_t cp, float targetPx,
                       const Color& col);

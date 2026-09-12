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
#include <string>
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

// Resolves every character of `text` against the strike `targetPx` picks,
// after folding ASCII lowercase to its capital (drawTerminusText's whole
// reason for existing is that callers keep writing normal mixed-case string
// literals). Returns false on the FIRST unresolvable character -- any byte
// >= 0x80 (a UTF-8 lead or continuation byte) is never baked, which is what
// makes any non-ASCII string fall back automatically; `out`/`outScale` are
// left in an unspecified partial state on failure and must not be used.
bool resolveText(const std::string& text, float targetPx,
                 std::vector<const Glyph*>& out, int& outScale);

} // namespace terminus

// Draw `cp` centred in `rc`. The origin is FLOORED so every edge lands on a
// whole pixel -- the shape path antialiases a fractional edge, and a grey
// pixel is precisely what a bitmap face must not have. False if not baked.
bool drawTerminusGlyph(Canvas& c, const LayoutRect& rc, char32_t cp, float targetPx,
                       const Color& col);

// The advance width `text` would occupy at drawTerminusText's chosen strike
// and scale -- for centering/right-alignment, mirroring Canvas::textWidth().
// Returns -1.0f, never a real width, when the string cannot be represented
// (distinct from a legitimately empty string's width of 0). Pure -- no
// Canvas -- so terminus_glyph_test can assert on real chrome strings without
// linking the renderer.
float terminusTextWidth(const std::string& text, float targetPx);

// Draws `text` left-aligned from (x, y) -- the same (x, y) = top-left
// convention Canvas::text()/textStyled() already use. The chosen strike's
// actual cell height is rarely exactly targetPx (16 or 32 times an integer
// scale), so the vertical placement is offset internally to keep a caller's
// EXISTING centering math (built around targetPx) correct without change --
// passing the same (x, y, targetPx) as an equivalent canvas.text() call lines
// the two up. Draws nothing and returns false if any character is
// unresolvable -- never half a string.
bool drawTerminusText(Canvas& c, const std::string& text, float x, float y,
                      float targetPx, const Color& col);

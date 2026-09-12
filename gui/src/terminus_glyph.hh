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
// strike at x1 below 24 px, otherwise the 16 px strike at round(target / 16).
// The 32 px strike is still baked; running text does not use it (twice the
// rows = twice the curves, and OverlayRasterizer drops past 8192).
Pick pickStrike(char32_t cp, float targetPx);

// Device-pixel rectangles relative to the glyph's top-left, one per run of set
// pixels in a row, each `scale` tall.
struct Run { int x, y, w, h; };
void runs(const Glyph& g, int scale, std::vector<Run>& out);

// Horizontal advance of `g` at `scale`, in device pixels. Ink bounding box
// plus 1 px side bearing -- not the monospace cell -- so "AAUDIO" does not
// open a hole around I. Space (no ink) still advances a full cell. Integer.
int advance(const Glyph& g, int scale);
// Columns of empty pixels to the left of the ink. Draw by subtracting
// leftBearing * scale from the pen so the ink starts at the pen.
int leftBearing(const Glyph& g);

// Resolves every character of `text` against the strike `targetPx` picks,
// after terminusFold (ASCII lowercase to capital, dashes/dots to ASCII,
// everything else to '?'). Always succeeds for any UTF-8 input: folding made
// every string representable. `outScale` is the strike scale.
bool resolveText(const std::string& text, float targetPx,
                 std::vector<const Glyph*>& out, int& outScale);

} // namespace terminus

// Fold one Unicode scalar to a baked ASCII cell. Lowercase a-z -> A-Z.
// U+2013/U+2014 -> '-'; U+00B7/U+2022/U+2219 -> '/'; printable ASCII otherwise
// unchanged; everything else -> '?'.
char terminusFoldCp(char32_t cp);

// Decode UTF-8, fold every scalar, return the ASCII string that will be drawn.
// Invalid UTF-8 bytes become '?'. Never empty-for-failure: "" in -> "".
std::string terminusFold(const std::string& utf8);

// Longest prefix of `text` (after fold) whose width is <= maxW, plus "..."
// when truncated. Truncation lands on a cell boundary. If even "..." does
// not fit, returns "...". Pure.
std::string terminusEllipsize(const std::string& text, float targetPx, float maxW);

// Word-wrap folded text to maxW. Splits on ASCII space. A single word wider
// than maxW is broken at a cell. Pure.
std::vector<std::string> terminusWrap(const std::string& text, float targetPx, float maxW);

// Draw `cp` centred in `rc`. The origin is FLOORED so every edge lands on a
// whole pixel -- the shape path antialiases a fractional edge, and a grey
// pixel is precisely what a bitmap face must not have. False if not baked.
bool drawTerminusGlyph(Canvas& c, const LayoutRect& rc, char32_t cp, float targetPx,
                       const Color& col);

// The advance width `text` would occupy at drawTerminusText's chosen strike
// and scale -- for centering/right-alignment, mirroring Canvas::textWidth().
// Empty -> 0. Never returns -1: folding made every string representable.
// Pure -- no Canvas -- so terminus_glyph_test can assert without the renderer.
float terminusTextWidth(const std::string& text, float targetPx);

// Draws `text` left-aligned from (x, y) -- the same (x, y) = top-left
// convention Canvas::text()/textStyled() already use. The chosen strike's
// actual cell height is rarely exactly targetPx (16 or 32 times an integer
// scale), so the vertical placement is offset internally to keep a caller's
// EXISTING centering math (built around targetPx) correct without change --
// passing the same (x, y, targetPx) as an equivalent canvas.text() call lines
// the two up. Folds first; always draws (never a serif fallback).
bool drawTerminusText(Canvas& c, const std::string& text, float x, float y,
                      float targetPx, const Color& col);

// Word-wrapped Terminus paragraph. `lineH` is the advance to the next line's
// top, same units as `y`.
void drawTerminusWrapped(Canvas& c, const std::string& text, float x, float y,
                         float targetPx, float maxW, float lineH, const Color& col);

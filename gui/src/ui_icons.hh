#pragma once
#include "layout_rect.hh"
#include "ui_icons.gen.h"   // generated: kIconCp*, kIconBoxEm, kIconCodepoints

// UI icons drawn as glyphs from the shared MTSDF atlas.
//
// The artwork lives in assets/fonts/icons/matrix-icons.otf (built from
// tools/icon_font/icons/*.svg by tools/icon_font/build_icon_font.py) and is
// baked into the same atlas as the UI text by PlayerWindow::bakeIconGlyphs().
// Drawing an icon is therefore one more text quad in a pass that already runs
// every frame: arbitrary curves, tinted at draw time, no extra GPU pass and no
// per-frame Bezier work.
//
// Each icon is authored inside its own square design box whose bottom edge
// sits on the baseline and whose left edge sits at x=0. The BOX — not the
// ink — maps onto the target rect, which reproduces the 36-unit grid the old
// primitive icons shared, so icons keep their relative sizes.
//
// The box size differs PER ICON because it also sets the bake density (an
// N-em outline is baked at N x 96px). A box far denser than the size an icon
// is drawn at is not "extra quality" — it is heavy minification, which a
// single bilinear tap smears. See the ICONS table in build_icon_font.py.
//
// This header deliberately does NOT include canvas.hh: the placement math
// below is pure and links into ui_icons_test without dragging in Vulkan.
class Canvas;
struct Color;

enum class UiIcon { Play, Stop, Prev, Next, Settings, Warning, Quality };

// Private Use Area codepoint for `icon`; 0 if unmapped.
unsigned uiIconCodepoint(UiIcon icon);

// Design box of `icon`, in ems (see above). 0 if unmapped.
float uiIconBoxEm(UiIcon icon);

// Where an icon's design box lands inside `rc`: the largest centred square,
// exactly like the primitive icons' `s = min(r.w, r.h)` framing.
struct IconPlacement {
    float x;       // pen X == the design box's left edge
    float y;       // y for Canvas::text(), which adds +size to reach the baseline
    float sizePx;  // em size in pixels
};
IconPlacement uiIconPlacement(const LayoutRect& rc, UiIcon icon);

// Draw `icon` as an atlas glyph (defined in ui_icons_draw.cc, the only part
// that needs Canvas). Returns false when the glyph isn't baked — missing or
// stale icon font, or an atlas too full to take it — so the caller can fall
// back to the primitive construction rather than drawing nothing.
//
// NOTE: msdf_frag.slang reads vertex alpha < 0.9995 as a "render from the true
// SDF" tag, so a translucent `col` renders with slightly softened corners.
// Harmless — and arguably right — for a faded icon, but it is not a bug.
bool drawUiIconGlyph(Canvas& c, const LayoutRect& rc, UiIcon icon, const Color& col);

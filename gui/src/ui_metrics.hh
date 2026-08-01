#pragma once
#include "layout.hh"                 // vk_canvas: UiScale
#include "ui_min_text_size.gen.h"    // build-generated: kMinReadableTextSizePx

// ── The one scale ────────────────────────────────────────────────────────────
//
// Text sizes and geometry derive from a SINGLE factor anchored at the app's
// real render height. Complete mode force-fullscreens (os/linux_host.cc), so
// 1080 is what this app actually draws at, not a historical reference size.
//
// The smallest role is PINNED to kMinReadableTextSizePx (the font's own
// geometric legibility floor, generated at build time from the shipped fonts'
// outline geometry). Every other role is that floor times kUiTypeRatio^n.
// This is what keeps the hierarchy honest: because all roles derive from one
// clamped value, the scale clamps UNIFORMLY below the reference height
// instead of the smallest role clamping alone and squashing the scale flat —
// the defect this module replaces. The old system authored seven independent
// percentages against a 661px reference, every one of which sat below the
// floor at that reference, so the hierarchy only partly existed above ~1051px
// and collapsed entirely below ~711px.
//
// See docs/superpowers/specs/2026-07-28-ui-design-system-rigor-pass-design.md.

static constexpr float kUiReferenceHeight = 1080.0f;
static constexpr float kUiTypeRatio       = 1.18f;

struct UiTextSizes {
    float caption;    // badges, hints, placeholders, section captions
    float secondary;  // artist, time, duration, search text
    float body;       // grid titles, track titles, nav, settings rows
    float title;      // now-playing title, album-view title
    float header;     // page + panel headers
};

// caption and secondary share a size deliberately — they are separated by the
// text-color ladder (theme.hh: PRIMARY 242 > SECONDARY 170 > DIM 128) and by
// font style, not by size.

struct UiMetrics {
    float       scale = 1.0f;   // 1.0 at H <= 1080, H/1080 above
    UiTextSizes text{};

    // Spacing/size: pads, gaps, row heights, widths, icon boxes.
    float space(float authored) const { return authored * scale; }

    // Stroke weight: hairlines, borders, bars. Snapped to whole device pixels
    // so a 1px rule never lands blurred across two pixels on a taller display
    // (continuous scaling would put it at 1.33px on a 1440p screen).
    float stroke(float authored) const;
};

UiMetrics computeUiMetrics(float contentHeight);

// The album grid's TOP pad, derived from its horizontal pad and the cell's
// own centering slack — never authored independently.
//
// It lives here, beside space()/stroke(), because it is the same kind of
// thing: resolution-robust geometry derived from one place rather than
// hand-tuned per screen. Pure and header-only so ui_metrics_test can assert
// it without linking a Canvas (same reason ui_icons.cc is split from
// ui_icons_draw.cc).
//
// Clamped at 0 slack: a cell narrower than its art is degenerate, and a
// negative pad would push the first row off the top of the page.
inline int gridTopPad(int padXpx, int cellStepX, int artSize) {
    const int slack = cellStepX - artSize;
    return padXpx + (slack > 0 ? slack / 2 : 0);
}

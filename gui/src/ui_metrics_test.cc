// Asserts must stay live even though the app builds Release (NDEBUG).
#undef NDEBUG
#include <cassert>
#include <cmath>
#include <cstdio>
#include <initializer_list>

#include "ui_metrics.hh"

static bool nearlyEqual(float a, float b) { return std::fabs(a - b) < 0.001f; }

int main() {
    // ── The factor floors at 1.0 and grows only above the reference height ──
    assert(nearlyEqual(computeUiMetrics(700.0f).scale,  1.0f));
    assert(nearlyEqual(computeUiMetrics(900.0f).scale,  1.0f));
    assert(nearlyEqual(computeUiMetrics(1080.0f).scale, 1.0f));
    assert(nearlyEqual(computeUiMetrics(1440.0f).scale, 1440.0f / 1080.0f));
    assert(nearlyEqual(computeUiMetrics(2160.0f).scale, 2.0f));

    // ── Roles at the reference height: floor * ratio^n ──
    UiMetrics m = computeUiMetrics(1080.0f);
    assert(nearlyEqual(m.text.caption,   kMinReadableTextSizePx));
    assert(nearlyEqual(m.text.secondary, kMinReadableTextSizePx));
    assert(nearlyEqual(m.text.body,      kMinReadableTextSizePx * 1.18f));
    assert(nearlyEqual(m.text.title,     kMinReadableTextSizePx * 1.18f * 1.18f));
    assert(nearlyEqual(m.text.header,    kMinReadableTextSizePx * 1.18f * 1.18f * 1.18f));

    // ── The invariant the OLD system violated: adjacent roles keep an exact
    //    ratio at EVERY height, so the hierarchy never distorts. Under the old
    //    seven-percentage scale the smallest role clamped alone below ~1051px
    //    and these ratios collapsed toward 1.0. ──
    for (float h : { 400.0f, 700.0f, 900.0f, 1080.0f, 1440.0f, 2160.0f, 4320.0f }) {
        UiMetrics k = computeUiMetrics(h);
        assert(nearlyEqual(k.text.body   / k.text.caption, kUiTypeRatio));
        assert(nearlyEqual(k.text.title  / k.text.body,    kUiTypeRatio));
        assert(nearlyEqual(k.text.header / k.text.title,   kUiTypeRatio));
        // caption is the smallest role and never drops under the legibility floor
        assert(k.text.caption >= kMinReadableTextSizePx - 0.001f);
        // hairlines never vanish
        assert(k.stroke(1.0f) >= 1.0f);
        // caption and secondary are one size by design
        assert(nearlyEqual(k.text.caption, k.text.secondary));
    }

    // ── space() is linear in the factor; stroke() snaps to whole pixels ──
    assert(nearlyEqual(m.space(65.0f), 65.0f));
    assert(nearlyEqual(computeUiMetrics(2160.0f).space(65.0f), 130.0f));
    assert(nearlyEqual(m.stroke(1.0f), 1.0f));
    assert(nearlyEqual(m.stroke(3.0f), 3.0f));
    assert(nearlyEqual(computeUiMetrics(1440.0f).stroke(1.0f), 1.0f));  // round(1.333)
    assert(nearlyEqual(computeUiMetrics(1440.0f).stroke(2.0f), 3.0f));  // round(2.667)
    assert(nearlyEqual(computeUiMetrics(1440.0f).stroke(3.0f), 4.0f));  // round(4.0)
    assert(nearlyEqual(computeUiMetrics(2160.0f).stroke(1.0f), 2.0f));
    assert(nearlyEqual(computeUiMetrics(2160.0f).stroke(3.0f), 6.0f));

    // ── gridTopPad: the grid's two margins are equal BY CONSTRUCTION ────────
    // A tile is centered in its cell, so the visible left margin is the pad
    // plus half the cell's leftover slack. The top has no such slack, so the
    // top pad must absorb it or the first row bleeds against the window edge
    // while the sidebar beside it has air. That is the whole property.
    {
        // 32px pad, 354px cell stride, 314px art -> 20px slack per side.
        assert(gridTopPad(32, 354, 314) == 52);

        // No slack (art exactly fills the cell): the pads coincide.
        assert(gridTopPad(32, 314, 314) == 32);

        // Odd slack truncates like the integer cell math it mirrors, and
        // never exceeds the visible left margin by rounding up.
        assert(gridTopPad(32, 355, 314) == 52);

        // Degenerate: a cell narrower than its art cannot push the row off
        // the top of the page.
        assert(gridTopPad(32, 300, 314) == 32);
    }

    // ── Grid scroll survives a resize ───────────────────────────────────────
    //
    // The reported bug: narrow the window, scroll the album grid to the
    // bottom, then widen it — the grid drew NOTHING. More columns pack the
    // same albums into fewer rows, so the content got shorter while the pixel
    // offset stayed put, landing past the end. drawFrame()'s firstRow then
    // started beyond the last tile and emitted no quads, and because
    // gridIndices_ was not empty the empty-state message did not draw either.
    // Just background.
    {
        const int stepY = 400;   // tile + row gap

        // Same geometry in and out: the anchor round-trips exactly.
        assert(gridScrollForAnchor(gridAnchorTile(1200, stepY, 4), stepY, 4) == 1200);

        // A partial row scrolls back to that row's start, never forward past
        // it — the anchor tile must be fully visible, not clipped at the top.
        assert(gridScrollForAnchor(gridAnchorTile(1250, stepY, 4), stepY, 4) == 1200);

        // THE REPORTED CASE, and why BOTH steps are needed. 47 albums at 3
        // columns (a half-width window) is 16 rows; scrolled near the bottom.
        // Widening to 6 columns halves the row count to 8.
        {
            const int viewH       = 1080;
            const int narrowScroll = 5200;                  // near the bottom of 16 rows
            const int wideContentH = 8 * stepY;             // 47 over 6 cols -> 8 rows
            const int wideMax      = wideContentH - viewH;  // 2120

            const int anchor = gridAnchorTile(narrowScroll, stepY, 3);
            assert(anchor == 39);                           // row 13 * 3 columns

            const int reScroll = gridScrollForAnchor(anchor, stepY, 6);
            assert(reScroll == 2400);                       // tile 39 -> row 6

            // Anchoring alone is NOT enough: row 6 of 8 is still past the end
            // of a viewport 1080 tall. Preserving what the listener was
            // looking at says nothing about whether it still fits.
            assert(reScroll > wideMax);

            // Which is why the clamp follows it. Together they land at the
            // bottom of the new grid — the closest legal position to the
            // albums that were on screen.
            const int finalScroll =
                (int)clampScroll((float)reScroll, (float)wideContentH, (float)viewH);
            assert(finalScroll == wideMax);
            assert(finalScroll >= 0 && finalScroll <= wideMax);

            // The bug, stated as a number: without any of this the offset
            // stays at 5200, which is 3080px past the last row. firstRow then
            // starts beyond the last tile and the grid draws nothing at all.
            assert(narrowScroll > wideMax);
            assert(narrowScroll / stepY > (wideContentH / stepY) - 1);
        }

        // Narrowing (the other direction) pushes the anchor further down, and
        // stays a legal offset for the taller grid it produces.
        {
            const int anchor = gridAnchorTile(2400, stepY, 6);    // tile 36
            assert(gridScrollForAnchor(anchor, stepY, 3) == 4800); // row 12
        }

        // Degenerate inputs: recalcLayout() runs once before any real layout
        // exists, with zero columns and a zero step.
        assert(gridAnchorTile(0, 0, 0) == 0);
        assert(gridAnchorTile(500, 0, 4) == 0);
        assert(gridAnchorTile(500, stepY, 0) == 0);
        assert(gridScrollForAnchor(0, stepY, 4) == 0);
        assert(gridScrollForAnchor(40, 0, 4) == 0);
        assert(gridScrollForAnchor(40, stepY, 0) == 0);

        // Top of the grid stays the top at any width — the case that never
        // reproduced the bug, and must not start moving now.
        for (int cols = 1; cols <= 8; cols++)
            assert(gridScrollForAnchor(gridAnchorTile(0, stepY, 3), stepY, cols) == 0);
    }

    std::printf("ui_metrics_test: all assertions passed\n");
    return 0;
}

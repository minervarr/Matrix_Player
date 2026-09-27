// scroll_test — the two rules every scrollable surface in the app shares.
//
// PlayerWindow::scrollDelta() and ::scrollDiscrete() replaced eight
// pixel-smooth clamps. Direction is still subtractive; motion is now the
// grid's row snap, so a delta smaller than the threshold does not change the
// row and a desktop notch of 120 is one row. The bodies are restated here
// rather than linked, because they are members of a class whose header drags
// in Canvas, Host, sqlite and the audio backends. scrollRows() itself is the
// REAL one (grid_layout.cc), so this cannot drift from the snap rule.
//
// Convention matches framework/vk_canvas/core/tests/*.cc: plain assert(), no
// framework, and NDEBUG undefined so the asserts survive an optimized build.
#undef NDEBUG
#include <cassert>
#include <algorithm>
#include <cstdio>

#include "grid_layout.hh"

static int scrollDelta(int rawDelta, bool invert) {
    return invert ? -rawDelta : rawDelta;
}

// Restated from PlayerWindow::snapScroll.
static int snapScroll(int offset, int contentH, int viewH, int pitch) {
    if (pitch <= 0) return 0;
    const int maxRow = std::max(0, (contentH - viewH) / pitch);
    int row = offset / pitch;
    if (row < 0) row = 0;
    if (row > maxRow) row = maxRow;
    return row * pitch;
}

// Restated from PlayerWindow::scrollDiscrete, minus invalidate().
static bool scrollDiscrete(grid::RowScroll& s, int& offsetPx, int delta,
                           int pitch, int contentH, int viewH, bool touch) {
    if (pitch <= 0) return false;
    const int maxRow = std::max(0, (contentH - viewH) / pitch);
    if (offsetPx != s.row * pitch) {
        s      = grid::RowScroll{};
        s.row  = offsetPx / pitch;
        s.free = (float)(s.row * pitch);
    }
    const float perUnit = touch ? 1.0f : (float)pitch / 120.0f;
    if (!grid::scrollRows(s, -(float)delta * perUnit, pitch, maxRow))
        return false;
    offsetPx = s.row * pitch;
    return true;
}

int main() {
    // ── Direction ───────────────────────────────────────────────────────────
    // The base sense is subtractive: a positive delta LOWERS the offset.
    {
        grid::RowScroll s;
        int off = 300;                          // row 1 of pitch 300
        s.row = 1; s.free = 300.0f;
        assert(scrollDiscrete(s, off, +120, 300, 2000, 500, false) && off == 0);
        assert(s.row == 0);
    }
    {
        grid::RowScroll s;
        int off = 0;
        assert(scrollDiscrete(s, off, -120, 300, 2000, 500, false) && off == 300);
        assert(s.row == 1);
    }

    // Inverting flips the sign and does nothing else — same magnitude, same
    // bounds, opposite direction.
    assert(scrollDelta(+120, false) == +120);
    assert(scrollDelta(+120, true)  == -120);
    assert(scrollDelta(0, true) == 0);          // no motion stays no motion
    for (int d = -300; d <= 300; d += 7)
        assert(scrollDelta(scrollDelta(d, true), true) == d);   // an involution

    // ── Discrete steps ──────────────────────────────────────────────────────
    const int P = 300, contentH = 2000, viewH = 500;   // maxRow = 5

    // A delta smaller than the threshold does not change the row.
    {
        grid::RowScroll s;
        int off = 0;
        assert(!scrollDiscrete(s, off, -59, P, contentH, viewH, true));
        assert(off == 0 && s.row == 0);
        assert( scrollDiscrete(s, off, -2, P, contentH, viewH, true));
        assert(off == P && s.row == 1);         // 61 px: jump
    }

    // One desktop notch of 120 is one row, from rest, every time.
    {
        grid::RowScroll s;
        int off = 0;
        for (int r = 0; r < 5; ++r) {
            assert(scrollDiscrete(s, off, -120, P, contentH, viewH, false));
            assert(off == (r + 1) * P && s.row == r + 1);
        }
        for (int r = 5; r > 0; --r) {
            assert(scrollDiscrete(s, off, +120, P, contentH, viewH, false));
            assert(off == (r - 1) * P && s.row == r - 1);
        }
    }

    // Content shorter than the view cannot scroll, however hard it is pushed.
    {
        grid::RowScroll s;
        int off = 0;
        assert(!scrollDiscrete(s, off, -9999, P, 300, 600, true) && off == 0);
        assert(!scrollDiscrete(s, off, +9999, P, 300, 600, true) && off == 0);
        assert(!scrollDiscrete(s, off, -9999, P, 600, 600, false) && off == 0);
        assert(snapScroll(0, 300, 600, P) == 0);
        assert(snapScroll(0, 600, 600, P) == 0);
    }

    // Never above the top, never past the last whole step.
    {
        grid::RowScroll s;
        int off = 0;
        assert(!scrollDiscrete(s, off, +50, P, contentH, viewH, true) && off == 0);
        assert(scrollDiscrete(s, off, -100000, P, contentH, viewH, true));
        assert(off == 5 * P && s.row == 5);
        assert(!scrollDiscrete(s, off, -1, P, contentH, viewH, true));
        assert(off == 5 * P);
    }

    // The bottom is reachable by the DRAW's viewport, never a taller one.
    // Same numbers the signal chain used to get wrong: using the wheel's
    // rcGrid_ height (720) as the view left no room at all, while the draw's
    // 629 did. Discrete maxRow = (content - view) / pitch.
    {
        const int pitch = 30;
        const int pageH = 663;
        const int drawView = 629, wheelView = 720;
        assert(snapScroll(9999, pageH, drawView, pitch) == 30);  // (663-629)/30 = 1
        assert(snapScroll(9999, pageH, wheelView, pitch) == 0);  // no room
        assert(snapScroll(9999, pageH, drawView, pitch) > 0);
    }

    // Snapping is idempotent: re-snapping a settled offset never moves it.
    for (int off = -50; off <= 2050; off += 37) {
        const int a = snapScroll(off, 2000, 500, P);
        assert(snapScroll(a, 2000, 500, P) == a);
        assert(a >= 0 && a <= 5 * P);
        assert(a % P == 0);
    }

    // A shrinking list must not strand the offset above its new end — the case
    // that renders as an EMPTY panel rather than a short one, because
    // widgets::drawScrollList clips instead of clamping. And the heal lands
    // on a whole step, never a leftover pixel.
    assert(snapScroll(900, 400, 300, 44) == 88);   // maxRow = 100/44 = 2
    assert(snapScroll(900, 200, 300, 44) == 0);

    printf("scroll_test: all assertions passed\n");
    return 0;
}

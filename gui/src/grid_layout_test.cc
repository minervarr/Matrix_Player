// grid_layout_test — the album grid's shape and its row-snapped scroll.
//
// Links src/grid_layout.cc and nothing else. The numbers below are the real
// ones for a 720x1640 phone at scale 1.0 (bar thickness 130, side pad 24,
// artMargin 30, text band 112, target pitch 250, min art 80), in both
// orientations, because that is the device the two layout defects were
// reported from.
//
// Convention matches framework/vk_canvas/core/tests/*.cc: plain assert(), no
// framework, NDEBUG undefined so the asserts survive an optimized build.
#undef NDEBUG
#include <cassert>
#include <cstdio>

#include "grid_layout.hh"

using grid::computeShape;
using grid::RowScroll;
using grid::scrollRows;

// Everything a shape must satisfy, whatever the numbers.
static void checkInvariants(const grid::Shape& s, int usableW, int usableH,
                            int artMargin, int textBand) {
    assert(s.cols >= 1 && s.rows >= 1);
    // Whole rows fill the height: at most rows-1 px of integer remainder.
    assert(s.rows * s.pitch <= usableH);
    assert(usableH - s.rows * s.pitch < s.rows);
    // The art fits its column...
    assert(s.art <= s.cellW - artMargin || s.cols == 1);
    assert(s.cols * s.cellW <= usableW);
    // ...and the art PLUS its text fits its row. This is the property the
    // phone lacked: the artist and year line is inside the row that owns it,
    // so a whole row is never a row with its text cut off.
    assert(s.artOffsetY + s.art + textBand <= s.pitch);
    assert(s.artOffsetY >= 0);
}

// Feed `n` equal moves of `step` px and return how many row changes fired.
static int feed(RowScroll& s, int n, float step, int P, int maxRow) {
    int changes = 0;
    for (int i = 0; i < n; ++i) changes += scrollRows(s, step, P, maxRow) ? 1 : 0;
    return changes;
}

int main() {
    const int pitch250 = 250, margin = 30, band = 112, minArt = 80;

    // ── Vertical: rcGrid_ 720 x 1380, minus a 24 px pad on every side ──────
    {
        const int w = 672, h = 1332;
        const grid::Shape s = computeShape(w, h, pitch250, margin, band, minArt);
        checkInvariants(s, w, h, margin, band);
        // 672 / 250 = 2.69. floor() used to make that 2 columns and 6 tiles;
        // nearest makes it 3 columns and 4 whole rows: 12 tiles.
        assert(s.cols == 3);
        assert(s.rows == 4);
        assert(s.cols * s.rows == 12);
        assert(s.pitch == 333);
        assert(s.art == 191);
    }

    // ── Horizontal: rcGrid_ 1380 x 720 ───────────────────────────────────────
    {
        const int w = 1332, h = 672;
        const grid::Shape s = computeShape(w, h, pitch250, margin, band, minArt);
        checkInvariants(s, w, h, margin, band);
        assert(s.cols == 5);
        // Two WHOLE rows. Before, row 2's byline sat 30 px below the bottom of
        // the grid at scroll 0 -- every launch opened on a clipped row.
        assert(s.rows == 2);
        assert(s.cols * s.rows == 10);
        assert(s.art == 194);
    }

    // The same area turned sideways gets the same SIZE of artwork, which is
    // what "use the area the same way in both orientations" comes down to:
    // 191 against 194 px, where it used to be 306 against 236.
    {
        const grid::Shape v = computeShape(672, 1332, pitch250, margin, band, minArt);
        const grid::Shape h = computeShape(1332, 672, pitch250, margin, band, minArt);
        assert(v.art - h.art <= 5 && h.art - v.art <= 5);
    }

    // A short viewport gives a row back rather than shrink art below minArt.
    {
        const grid::Shape s = computeShape(1332, 250, pitch250, margin, band, minArt);
        checkInvariants(s, 1332, 250, margin, band);
        assert(s.rows == 1);
    }

    // Degenerate input must not divide by zero or go negative.
    {
        const grid::Shape s = computeShape(0, 0, pitch250, margin, band, minArt);
        assert(s.pitch >= 1 && s.cellW >= 1 && s.art >= 1);
    }

    // ── Row snapping ────────────────────────────────────────────────────────
    const int P = 300, maxRow = 10;          // threshold = 0.2 * 300 = 60 px

    // A deliberate drag answers after 0.2 of a row, not half of one.
    {
        RowScroll s;
        assert(!scrollRows(s, 59.0f, P, maxRow) && s.row == 0);
        assert( scrollRows(s,  2.0f, P, maxRow) && s.row == 1);   // 61 px: jump
        // ...and then once per WHOLE row of further travel, never faster.
        assert(feed(s, 1, 298.0f, P, maxRow) == 0 && s.row == 1); // 359 px
        assert(feed(s, 1,   2.0f, P, maxRow) == 1 && s.row == 2); // 361 px
    }

    // The regression the first version of this rule had: right after an early
    // jump, one pixel of BACKWARD travel must not jump straight back. Turning
    // around costs the same 0.2 of a row as going forward did.
    {
        RowScroll s;
        feed(s, 1, 61.0f, P, maxRow);                 // row 1
        assert(s.row == 1);
        assert(!scrollRows(s, -1.0f, P, maxRow) && s.row == 1);
        assert(!scrollRows(s, -58.0f, P, maxRow) && s.row == 1); // 59 back
        assert( scrollRows(s, -2.0f, P, maxRow) && s.row == 0);  // 61 back
    }

    // A finger resting on the glass jitters by a pixel or two. It must never
    // make the grid strobe between rows.
    {
        RowScroll s;
        s.row = 4; s.free = 4.0f * P;
        for (int i = 0; i < 200; ++i) {
            assert(!scrollRows(s, (i % 2) ? 1.5f : -1.5f, P, maxRow));
            assert(s.row == 4);
        }
    }

    // Continuous travel in one direction is never re-anchored, so its
    // distance in rows is travel / pitch. 1500 px: rows change at 60, 360,
    // 660, 960 and 1260 -> row 5.
    {
        RowScroll s;
        assert(feed(s, 30, 50.0f, P, maxRow) == 5);
        assert(s.row == 5);
    }

    // One wheel notch is scaled to one pitch, so each notch steps exactly one
    // row, from rest, every time -- no notch is swallowed, none counts twice.
    {
        RowScroll s;
        for (int r = 0; r < maxRow; ++r) {
            assert(scrollRows(s, (float)P, P, maxRow));
            assert(s.row == r + 1);
        }
        // ...and the same coming back.
        for (int r = maxRow; r > 0; --r) {
            assert(scrollRows(s, -(float)P, P, maxRow));
            assert(s.row == r - 1);
        }
    }

    // A throw that overshoots stops on the last legal row, and never below 0.
    {
        RowScroll s;
        scrollRows(s, 1.0e6f, P, maxRow);
        assert(s.row == maxRow);
        assert(scrollRows(s, -61.0f, P, maxRow) && s.row == maxRow - 1);
        scrollRows(s, -1.0e6f, P, maxRow);
        assert(s.row == 0);
    }

    // Content that fits never scrolls, and a zero move is not a move.
    {
        RowScroll s;
        assert(!scrollRows(s, 500.0f, P, 0) && s.row == 0);
        assert(!scrollRows(s, 0.0f, P, maxRow));
    }

    printf("grid_layout_test: all assertions passed\n");
    return 0;
}

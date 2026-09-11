#pragma once

// ── The album grid's shape, and how it scrolls ──────────────────────────────
//
// Two decisions, both pure arithmetic, both pinned by grid_layout_test.
//
// SHAPE. The grid used to take its column count as floor(width / pitch) and
// then stack rows of a fixed natural height until the viewport ran out, so
// the last row on screen was whatever fraction happened to fit. Two defects
// fell out of that, and both were reported from the same phone:
//
//   * FLOOR made 2.69 columns into 2. On a 720 px-wide portrait screen the
//     grid showed 2 x 3 = 6 tiles while the very same area turned sideways
//     showed 5 x 2 = 10 -- equal area, 60% of the tiles, only because a
//     rounding direction differs between 2.69 and 5.33.
//   * A fractional last row is a row whose TEXT is off the bottom. The art
//     fits, the album name fits, and the artist and year are clipped away.
//     In the horizontal layout that happened at scroll 0, on every launch.
//
// So the column count is the NEAREST whole number of target pitches, and the
// row count is the nearest whole number of natural row heights -- after which
// the row pitch is stretched (or trimmed) so that whole rows fill the height
// exactly. The artwork is bounded by BOTH its column's width and its row's
// height, so the text band under it always fits inside its own row. Nothing
// is ever cut at the bottom of the grid, by construction rather than by luck.
//
// SCROLL. The grid scrolls by whole rows and never shows a partial one. There
// is no animation: the offset is either one row or the next, and the next
// frame simply draws it. The input is still tracked continuously -- a finger's
// own pixels, or a wheel notch scaled to one row -- so the host's kinetic throw
// works unchanged and still decides how FAR a flick carries. Only where the
// grid SNAPS is quantised. See scrollRows().
namespace grid {

struct Shape {
    int cols       = 1;
    int rows       = 1;   // whole rows filling the usable height
    int cellW      = 1;   // horizontal stride of one column
    int art        = 1;   // square artwork side
    int pitch      = 1;   // vertical stride of one row; rows * pitch <= usableH
    int artOffsetY = 0;   // artwork top within its row (art + text centred)
};

// usableW/usableH: the grid rect minus its side pads. tilePitch: the target
// column stride. artMargin: air reserved around the art in its cell.
// textBand: height of the text block under the art (title lines + byline).
// minArt: the smallest legible artwork; a column or a row is dropped rather
// than going below it.
Shape computeShape(int usableW, int usableH, int tilePitch, int artMargin,
                   int textBand, int minArt);

// How far the input must travel before the grid jumps a row, as a fraction of
// one row. Small enough that a deliberate drag answers at once; large enough
// that a finger resting on the glass cannot flicker between two rows.
inline constexpr float kRowSnapThreshold = 0.2f;

// The row-snapped scroll's whole state. `free` is the input's continuous
// position in grid pixels, `row` is the row on screen, `dir` the direction of
// the last movement (+1 toward the end, -1 toward the start, 0 at rest).
struct RowScroll {
    float free = 0.0f;
    int   row  = 0;
    int   dir  = 0;
};

// Move by `dpx` grid pixels (positive = toward the end). Returns true when the
// row on screen changed, which is the only time a frame is worth drawing.
//
// The rule, in the listener's terms: a row changes after kRowSnapThreshold of
// a row's travel in either direction, and then once per whole row after that.
//
// It takes the direction into account, and that is load-bearing. Jumping
// EARLY means that right after a jump the input still sits most of a row short
// of the new row -- so a rule that only looked at the position would see the
// very next pixel of backward jitter as a reason to jump straight back, and a
// resting finger would strobe between two rows. Instead a REVERSAL re-anchors
// the input to the row on screen, so turning back costs the same small
// threshold as going forward did. Continuous travel in one direction is never
// re-anchored, which is what keeps a throw's distance exactly what the host's
// kinetic scroller computed: travel / pitch rows, no more and no less.
bool scrollRows(RowScroll& s, float dpx, int pitch, int maxRow);

} // namespace grid

#include "rail_layout.hh"

namespace {

// One interval along the bar's long axis, measured from the SETTINGS end.
struct Span { int a = 0; int b = 0; };

// The only place the two orientations differ. Maps a span on the long axis to
// a window rect, filling the bar's full thickness on the cross axis.
//
// Vertical  (top bar):  near = left,   far = right   -> +x
// Horizontal (left bar): near = bottom, far = top    -> -y
//
// Those two ARE one 90-degree counter-clockwise rotation of each other; see
// rail_layout.hh, and rail_layout_test, which asserts it rather than trusting
// this comment.
LayoutRect place(const RailInput& in, Span s) {
    if (s.b <= s.a) return {};   // degenerate: nothing to draw, nothing to hit
    if (in.orient == UiOrientation::Vertical)
        return { in.bar.left + s.a, in.bar.top, in.bar.left + s.b, in.bar.bottom };
    return { in.bar.left, in.bar.bottom - s.b, in.bar.right, in.bar.bottom - s.a };
}

// The bar's extent along its long axis.
int longExtent(const RailInput& in) {
    return (in.orient == UiOrientation::Vertical) ? (in.bar.right - in.bar.left)
                                                  : (in.bar.bottom - in.bar.top);
}

}  // namespace

RailLayout computeRailLayout(const RailInput& in) {
    RailLayout out;

    const int L = longExtent(in);
    if (L <= 0 || in.cell <= 0) return out;   // no bar yet, or no metrics yet

    const bool showEqBox = !in.bitPerfect && !in.searchOpen;
    const int  eqLen     = (showEqBox && in.eqBoxExtent > 0) ? in.eqBoxExtent : 0;

    // ── The cell shrinks to fit, and this is load-bearing ───────────────────
    //
    // A square cell at the bar's own thickness is the intent, but it does not
    // always fit: nine cells (Settings + Search + seven filters) plus the
    // AutoEQ box plus the insets can exceed the bar's long extent. That is not
    // a corner case -- it is the ordinary VERTICAL bar, whose long extent is
    // the window's WIDTH, on any 1080-wide screen. At the reference scale that
    // is 1080 against 9x130 + 300 + gaps, which does not fit.
    //
    // Shrinking uniformly is the only option that keeps the rail readable:
    // dropping items would hide a filter, and letting them overflow would put
    // letters underneath the AutoEQ box, where they would still hit-test.
    // Settings uses the same cell as the rest, so the row stays one family.
    int cell = in.cell;
    {
        const int cells = kRailLetterCount + 2;         // + search + settings
        const int fixed = in.pad * 2 + eqLen + in.gap;
        if ((long long)cells * cell + fixed > L) {
            cell = (L - fixed) / cells;
            if (cell < 1) return out;                   // nothing legible fits
        }
    }

    // Settings: pinned at the near end, in every state. See rail_layout.hh —
    // search must not be able to push it.
    Span settings{ in.pad, in.pad + cell };
    out.settings = place(in, settings);

    // The AutoEQ box hides in bit-perfect (nothing to pick a profile for) and
    // while searching (the field takes the bar). Those two are the same code
    // path on purpose: open-search then looks identical in both EQ modes.
    int nearEnd = settings.b;                 // where the near cluster stops
    if (eqLen > 0) {
        Span box{ nearEnd, nearEnd + eqLen };
        out.eqBox = place(in, box);

        // The box's two halves: a SQUARE × at its near end (no profile), the
        // active name taking the rest. Computed HERE rather than during the
        // draw, so the hit-test and the drawing read one source instead of the
        // drawing writing what the hit-test will read next frame.
        //
        // Square means the bar's THICKNESS, not `cell`. Those are the same
        // number until the cells shrink, and they shrink on exactly the bar
        // whose long extent is the window's height -- the ordinary horizontal
        // one. Using `cell` there made the × a 82x130 rectangle and shifted
        // both it and the name 24 px toward the near end.
        const int thick = (in.orient == UiOrientation::Vertical)
                              ? (in.bar.bottom - in.bar.top)
                              : (in.bar.right - in.bar.left);
        const int sq = (thick < eqLen) ? thick : eqLen;
        out.eqNone = place(in, Span{ box.a, box.a + sq });
        out.eqName = place(in, Span{ box.a + sq, box.b });

        // The unfurled list runs from the box to the far end of the bar. It
        // takes the letter group's space -- see RailInput::eqListOpen.
        if (in.eqListOpen) out.eqList = place(in, Span{ box.b, L });

        nearEnd = box.b;
    }

    if (in.searchOpen) {
        // Close at the far end, field spanning everything between it and the
        // near cluster. The filter letters are gone -- collapsing them is what
        // makes room for a field wide enough to hold chips.
        Span close{ L - in.pad - cell, L - in.pad };
        out.close = place(in, close);
        out.search = place(in, Span{ nearEnd + in.gap, close.a - in.gap });
        return out;
    }

    // The unfurled AutoEQ list takes the letter group's space, for the same
    // reason open search does -- see RailInput::eqListOpen. Settings and the
    // box itself stay exactly where they were.
    if (in.eqListOpen) return out;

    // Search letter + the seven filter letters travel together as one group of
    // eight cells, with search on the NEAR side of the group: reading outward
    // from Settings it goes search, then Albums..Playlists.
    const int groupCells = kRailLetterCount + 1;
    const int groupLen   = groupCells * cell;

    int groupStart;
    if (showEqBox) {
        // Reference EQ: pegged to the far end, because the box occupies the
        // near end and centring would leave the group visibly off-axis.
        groupStart = L - in.pad - groupLen;
    } else {
        // Bit-perfect: centred in the space that is actually free -- between
        // the near cluster and the far inset -- recomputed from the live bar,
        // never from a stored constant.
        const int freeA = nearEnd + in.gap;
        const int freeB = L - in.pad;
        groupStart = freeA + ((freeB - freeA) - groupLen) / 2;
    }
    // Never overlap the near cluster, however narrow the window gets. A letter
    // drawn under the AutoEQ box would still be hit-testable there.
    if (groupStart < nearEnd + in.gap) groupStart = nearEnd + in.gap;

    out.search = place(in, Span{ groupStart, groupStart + cell });
    for (int i = 0; i < kRailLetterCount; i++) {
        const int a = groupStart + (i + 1) * cell;
        out.letters[i] = place(in, Span{ a, a + cell });
    }
    return out;
}

// ── Rows inside an unfurled list ─────────────────────────────────────────────
// `list` is already a window rect, so these need no RailInput: the long axis is
// implied by the orientation, and rows run from the list's NEAR end (its left
// in Vertical, its bottom in Horizontal) outward.

int railListCapacity(const LayoutRect& list, UiOrientation orient, int rowExtent) {
    if (rowExtent <= 0) return 0;
    const int room = (orient == UiOrientation::Vertical) ? (list.right - list.left)
                                                         : (list.bottom - list.top);
    return room > 0 ? room / rowExtent : 0;
}

LayoutRect railListRow(const LayoutRect& list, UiOrientation orient,
                       int rowExtent, int i) {
    if (rowExtent <= 0 || i < 0) return {};
    if (i >= railListCapacity(list, orient, rowExtent)) return {};
    if (orient == UiOrientation::Vertical)
        return { list.left + i * rowExtent, list.top,
                 list.left + (i + 1) * rowExtent, list.bottom };
    return { list.left, list.bottom - (i + 1) * rowExtent,
             list.right, list.bottom - i * rowExtent };
}

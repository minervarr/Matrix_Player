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
    // always fit: nine cells (Settings + Find + seven filters) plus the AutoEQ
    // box plus the insets can exceed the bar's long extent. That is not a
    // corner case -- it is the ordinary VERTICAL bar, whose long extent is the
    // window's WIDTH, on any 1080-wide screen. At the reference scale that is
    // 1080 against 9x130 + 300 + gaps, which does not fit.
    //
    // Shrinking uniformly is the only option that keeps the rail readable:
    // dropping items would hide a filter, and letting them overflow would put
    // letters underneath the AutoEQ box, where they would still hit-test.
    // Settings uses the same cell as the rest, so the row stays one family.
    int cell = in.cell;
    {
        const int cells = kRailLetterCount + 2;         // + Find + Settings
        const int fixed = in.pad * 2 + eqLen + in.gap;
        if ((long long)cells * cell + fixed > L) {
            cell = (L - fixed) / cells;
            if (cell < 1) return out;                   // nothing legible fits
        }
    }

    // Settings: pinned at the near end, in every state. See rail_layout.hh —
    // search must not be able to push it.
    const Span settings{ in.pad, in.pad + cell };
    out.settings = place(in, settings);

    if (in.searchOpen) {
        // Close at the far end, the field spanning everything between it and
        // Settings. The letters and the box are gone -- collapsing them is what
        // makes room for a field wide enough to hold chips.
        const Span close{ L - in.pad - cell, L - in.pad };
        out.close  = place(in, close);
        out.search = place(in, Span{ settings.b + in.gap, close.a - in.gap });
        return out;
    }

    // Find sits beside Settings: the two RARE cells together, where a tap
    // meant for a filter cannot reach them. See rail_layout.hh.
    const Span find{ settings.b, settings.b + cell };
    out.search = place(in, find);
    int nearEnd = find.b;                     // where the near cluster stops

    // The AutoEQ box hides in bit-perfect (nothing to pick a profile for) and
    // while searching (the field takes the bar). Those two are the same code
    // path on purpose: open search then looks identical in both EQ modes.
    if (eqLen > 0) {
        const Span box{ nearEnd, nearEnd + eqLen };
        out.eqBox = place(in, box);
        nearEnd = box.b;
    }

    const int groupLen = kRailLetterCount * cell;
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

    for (int i = 0; i < kRailLetterCount; i++) {
        const int a = groupStart + i * cell;
        out.letters[i] = place(in, Span{ a, a + cell });
    }
    return out;
}

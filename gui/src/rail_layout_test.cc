// Asserts must stay live even though the app builds Release (NDEBUG).
#undef NDEBUG
#include <cassert>
#include <cstdio>

#include "rail_layout.hh"

namespace {

// A 1920x1080 screen at the reference scale: space(130) is 130 px, and a cell
// is square at that thickness. These are the app's real numbers, not round
// ones -- a layout test on invented sizes proves nothing about the layout the
// app actually draws.
constexpr int kW    = 1920;   // the bar's long extent
constexpr int kT    = 130;    // its thickness  (space(130), scale 1.0)
constexpr int kCell = 130;    // one cell: square at this thickness
constexpr int kEq   = 300;
constexpr int kPad  = 16;
constexpr int kGap  = 24;

bool empty(const LayoutRect& r) {
    return r.left == 0 && r.top == 0 && r.right == 0 && r.bottom == 0;
}
int  wide(const LayoutRect& r) { return r.right - r.left; }
int  tall(const LayoutRect& r) { return r.bottom - r.top; }
bool same(const LayoutRect& a, const LayoutRect& b) {
    return a.left == b.left && a.top == b.top &&
           a.right == b.right && a.bottom == b.bottom;
}

RailInput vertical(bool bitPerfect, bool searchOpen) {
    RailInput in;
    in.bar = { 0, 0, kW, kT };          // top bar: full width, thickness tall
    in.orient = UiOrientation::Vertical;
    in.bitPerfect = bitPerfect;
    in.searchOpen = searchOpen;
    in.cell = kCell; in.eqBoxExtent = kEq; in.pad = kPad; in.gap = kGap;
    return in;
}

RailInput horizontal(bool bitPerfect, bool searchOpen) {
    RailInput in = vertical(bitPerfect, searchOpen);
    in.bar = { 0, 0, kT, kW };          // left bar: thickness wide, full height
    in.orient = UiOrientation::Horizontal;
    return in;
}

// The 90-degree COUNTER-CLOCKWISE rotation, stated geometrically and NOT
// copied from rail_layout.cc's place(). In screen coordinates (y down),
// rotating a W-by-T bar counter-clockwise sends (x, y) to (y, W - x): the left
// end goes to the bottom, the right end to the top, the top edge to the left
// edge.
LayoutRect rotateCcw(const LayoutRect& r) {
    return { r.top, kW - r.right, r.bottom, kW - r.left };
}

// Every rect of a layout, in one order, so the rotation check can walk both.
void collect(const RailLayout& l, const LayoutRect* (&out)[kRailLetterCount + 7]) {
    int n = 0;
    for (int i = 0; i < kRailLetterCount; i++) out[n++] = &l.letters[i];
    out[n++] = &l.search;
    out[n++] = &l.settings;
    out[n++] = &l.close;
    out[n++] = &l.eqBox;
    out[n++] = &l.eqNone;
    out[n++] = &l.eqName;
    out[n++] = &l.eqList;
}

// ── Rule 3: the two orientations are ONE layout and a rotation ──────────────
// This is the assertion that makes it impossible for the two to drift apart.
// If someone later special-cases an anchor for one orientation, this fails.
void assertRotationHolds(bool bitPerfect, bool searchOpen, bool eqListOpen = false) {
    RailInput vi = vertical(bitPerfect, searchOpen);   vi.eqListOpen = eqListOpen;
    RailInput hi = horizontal(bitPerfect, searchOpen); hi.eqListOpen = eqListOpen;
    const RailLayout v = computeRailLayout(vi);
    const RailLayout h = computeRailLayout(hi);

    const LayoutRect* vr[kRailLetterCount + 7];
    const LayoutRect* hr[kRailLetterCount + 7];
    collect(v, vr);
    collect(h, hr);

    for (int i = 0; i < kRailLetterCount + 7; i++) {
        // Hidden in one orientation must mean hidden in the other -- rotating
        // an empty rect would otherwise produce a plausible-looking rectangle.
        assert(empty(*vr[i]) == empty(*hr[i]));
        if (empty(*vr[i])) continue;
        assert(same(rotateCcw(*vr[i]), *hr[i]));
    }
}

}  // namespace

int main() {
    // ── Rule 1: bit-perfect CENTRES the letter group, Reference EQ PEGS it ───
    {
        const RailLayout ref = computeRailLayout(vertical(/*bitPerfect=*/false, false));
        const RailLayout bp  = computeRailLayout(vertical(/*bitPerfect=*/true,  false));

        // Reference EQ: the group ends at the far inset.
        assert(ref.letters[kRailPlaylists].right == kW - kPad);

        // Bit-perfect: centred in the space between the near cluster and the
        // far inset -- so it does NOT reach the far end, and it sits further
        // out than the near cluster.
        assert(bp.letters[kRailPlaylists].right < kW - kPad);
        assert(bp.search.left > bp.settings.right);

        // Centred means centred: the slack left over on each side matches.
        const int freeA  = kPad + kCell + kGap;          // settings, then the gap
        const int freeB  = kW - kPad;
        const int slackA = bp.search.left - freeA;
        const int slackB = freeB - bp.letters[kRailPlaylists].right;
        assert(slackA == slackB || slackA == slackB + 1);  // odd remainder

        // And the group is the SAME SIZE in both -- centring moves it, never
        // resizes it.
        assert(ref.letters[kRailPlaylists].right - ref.search.left ==
               bp.letters[kRailPlaylists].right  - bp.search.left);
    }

    // ── Rule 2a: the absolute sides, Vertical ───────────────────────────────
    // Settings at the LEFT, AutoEQ box also on the left just inside it,
    // letters running toward the RIGHT.
    {
        const RailLayout v = computeRailLayout(vertical(false, false));
        assert(v.settings.left == kPad);
        assert(!empty(v.eqBox));
        assert(v.eqBox.left == v.settings.right);        // same end, just inside
        assert(v.search.left > v.eqBox.right);
        for (int i = 1; i < kRailLetterCount; i++)
            assert(v.letters[i].left > v.letters[i - 1].left);   // left to right
        assert(v.letters[kRailAlbums].left > v.search.left);     // search is inner

        // Every cell fills the bar's full thickness on the cross axis.
        assert(tall(v.settings) == kT && tall(v.search) == kT);
        assert(tall(v.letters[kRailAlbums]) == kT);
        assert(wide(v.letters[kRailAlbums]) == kCell);
    }

    // ── Rule 2b: the absolute sides, Horizontal ─────────────────────────────
    // Settings at the BOTTOM, AutoEQ box above it on the same end, letters
    // running toward the TOP. This is 2a rotated, and is asserted directly so
    // a reader can check the design against the spec without doing the
    // rotation in their head.
    {
        const RailLayout h = computeRailLayout(horizontal(false, false));
        assert(h.settings.bottom == kW - kPad);          // pinned to the bottom
        assert(!empty(h.eqBox));
        assert(h.eqBox.bottom == h.settings.top);        // same end, just above
        assert(h.search.bottom < h.eqBox.top);
        for (int i = 1; i < kRailLetterCount; i++)
            assert(h.letters[i].top < h.letters[i - 1].top);     // bottom to top
        assert(h.letters[kRailPlaylists].top == kPad);   // reaches the far end

        assert(wide(h.settings) == kT);
        assert(tall(h.letters[kRailAlbums]) == kCell);
    }

    // ── Rule 4: open search ─────────────────────────────────────────────────
    {
        const RailLayout v = computeRailLayout(vertical(false, /*searchOpen=*/true));

        // The filter letters collapse, and so does the AutoEQ box.
        for (int i = 0; i < kRailLetterCount; i++) assert(empty(v.letters[i]));
        assert(empty(v.eqBox));

        // Settings does NOT move -- that is the whole point: interrupting a
        // filter to change a setting must not cost what was typed.
        const RailLayout closed = computeRailLayout(vertical(false, false));
        assert(same(v.settings, closed.settings));

        // Close takes the far end; the field spans between the two anchors.
        assert(!empty(v.close));
        assert(v.close.right == kW - kPad);
        assert(v.search.left  > v.settings.right);
        assert(v.search.right < v.close.left);
        assert(wide(v.search) > kCell * 4);   // a field, not a letter

        // Bit-perfect changes NOTHING while search is open -- because the box
        // hides either way, this state is drawn once, not twice.
        const RailLayout bpOpen = computeRailLayout(vertical(true, true));
        assert(same(bpOpen.search,   v.search));
        assert(same(bpOpen.settings, v.settings));
        assert(same(bpOpen.close,    v.close));
    }

    // ── The unfurled AutoEQ list HIDES the letters, it does not float ──────
    // Forced by the renderer, not chosen: every rect is emitted before every
    // glyph, so an overlay background can never cover text drawn earlier --
    // the letters would show straight through it. Hiding them also stops them
    // hit-testing underneath the list.
    {
        RailInput in = vertical(false, false);
        in.eqListOpen = true;
        const RailLayout l = computeRailLayout(in);

        for (int i = 0; i < kRailLetterCount; i++) assert(empty(l.letters[i]));
        assert(empty(l.search));
        assert(empty(l.close));

        // The box and Settings do NOT move: the list unfurls FROM the box, so
        // its anchor has to stay where the listener just touched it.
        const RailLayout closed = computeRailLayout(vertical(false, false));
        assert(same(l.eqBox,    closed.eqBox));
        assert(same(l.settings, closed.settings));
    }

    // ── The AutoEQ box's two halves come from the LAYOUT, not the draw ──────
    // They used to be written by drawEqBox() and read by the hit-test a frame
    // later. Computing them here is what lets both read one source.
    {
        const RailLayout v = computeRailLayout(vertical(false, false));
        assert(!empty(v.eqNone) && !empty(v.eqName));

        // The × takes a SQUARE at the box's near end; the name takes the rest.
        assert(v.eqNone.left  == v.eqBox.left);
        assert(wide(v.eqNone) == tall(v.eqNone));        // square, by definition
        assert(wide(v.eqNone) == kT);                    // ...of the bar's thickness
        assert(v.eqName.left  == v.eqNone.right);
        assert(v.eqName.right == v.eqBox.right);
        assert(tall(v.eqNone) == kT && tall(v.eqName) == kT);

        // Rotated, the × is BELOW the name: the near end of a left bar is its
        // bottom, so the symbol sits under the label the same way it sits
        // before it when the bar is on top.
        const RailLayout h = computeRailLayout(horizontal(false, false));
        assert(h.eqNone.bottom == h.eqBox.bottom);
        assert(h.eqName.bottom == h.eqNone.top);

        // Hidden with the box, in both the states that hide it.
        assert(empty(computeRailLayout(vertical(true,  false)).eqName));
        assert(empty(computeRailLayout(vertical(false, true )).eqName));

        // The square follows the THICKNESS, never the shrunken cell. On a
        // narrow bar the two diverge, and using the cell made the × an
        // 82x130 rectangle -- caught by comparing captures, not by reasoning.
        RailInput narrow = vertical(false, false);
        narrow.bar = { 0, 0, 1080, kT };
        const RailLayout n = computeRailLayout(narrow);
        assert(wide(n.settings) < kCell);                // cells really did shrink
        assert(wide(n.eqNone) == tall(n.eqNone));        // ...and the square did not
        assert(wide(n.eqNone) == kT);
    }

    // ── The unfurled list: one span, split by a pure function ───────────────
    {
        RailInput in = vertical(false, false);
        in.eqListOpen = true;
        const RailLayout l = computeRailLayout(in);

        // The span runs from the far edge of the box to the far end of the bar.
        assert(!empty(l.eqList));
        assert(l.eqList.left  == l.eqBox.right);
        assert(l.eqList.right == kW);

        // Closed, there is no span at all.
        assert(empty(computeRailLayout(vertical(false, false)).eqList));

        // Rows run from the list's near end outward, flush, no gaps.
        const int rowExtent = 52;
        const int cap = railListCapacity(l.eqList, UiOrientation::Vertical, rowExtent);
        assert(cap == wide(l.eqList) / rowExtent);
        assert(cap > 1);
        for (int i = 1; i < cap; i++) {
            const LayoutRect prev = railListRow(l.eqList, UiOrientation::Vertical, rowExtent, i - 1);
            const LayoutRect cur  = railListRow(l.eqList, UiOrientation::Vertical, rowExtent, i);
            assert(cur.left == prev.right);
            assert(wide(cur) == rowExtent);
            assert(tall(cur) == kT);
        }
        // Past capacity is empty, never a rect off the end of the bar.
        assert(empty(railListRow(l.eqList, UiOrientation::Vertical, rowExtent, cap)));
        assert(empty(railListRow(l.eqList, UiOrientation::Vertical, rowExtent, -1)));
        assert(empty(railListRow(l.eqList, UiOrientation::Vertical, 0, 0)));

        // Horizontal: rows stack UPWARD from the bottom, the same rotation.
        RailInput hin = horizontal(false, false);
        hin.eqListOpen = true;
        const RailLayout hl = computeRailLayout(hin);
        const LayoutRect h0 = railListRow(hl.eqList, UiOrientation::Horizontal, rowExtent, 0);
        const LayoutRect h1 = railListRow(hl.eqList, UiOrientation::Horizontal, rowExtent, 1);
        assert(h0.bottom == hl.eqList.bottom);
        assert(h1.bottom == h0.top);
    }

    // ── The vertical bar is NARROW, and the cells shrink to fit ─────────────
    // Not a corner case: a vertical bar's long extent is the window's WIDTH.
    // On a 1080-wide screen nine 130px cells plus a 300px AutoEQ box plus the
    // insets need 1494 -- 414 more than there is. Overflowing would put filter
    // letters underneath the AutoEQ box, where they would still hit-test.
    {
        RailInput in = vertical(false, false);
        in.bar = { 0, 0, 1080, kT };
        const RailLayout n = computeRailLayout(in);

        const int cell = wide(n.settings);
        assert(cell > 0 && cell < kCell);                 // shrunk, not dropped

        // Every cell is the SAME size -- Settings included, so the row reads as
        // one family rather than one odd button beside eight others.
        assert(wide(n.search) == cell);
        for (int i = 0; i < kRailLetterCount; i++) assert(wide(n.letters[i]) == cell);

        // Nothing is dropped and nothing overlaps: the near cluster ends before
        // the group starts, and the group still reaches the far inset.
        assert(n.eqBox.left == n.settings.right);
        assert(n.search.left >= n.eqBox.right);
        assert(n.letters[kRailPlaylists].right == 1080 - kPad);
        for (int i = 1; i < kRailLetterCount; i++)
            assert(n.letters[i].left == n.letters[i - 1].right);   // flush, no gaps
    }

    // ── Rule 3, over every state ────────────────────────────────────────────
    assertRotationHolds(/*bitPerfect=*/false, /*searchOpen=*/false);
    assertRotationHolds(/*bitPerfect=*/true,  /*searchOpen=*/false);
    assertRotationHolds(/*bitPerfect=*/false, /*searchOpen=*/true);
    assertRotationHolds(/*bitPerfect=*/true,  /*searchOpen=*/true);
    assertRotationHolds(/*bitPerfect=*/false, /*searchOpen=*/false, /*eqListOpen=*/true);

    // ── Degenerate bars produce nothing, and never a negative rect ──────────
    // A layout pass can run before the first configure lands (see
    // recalcLayout()'s own renderer_ guard), and a narrow window must not
    // produce cells that overlap the near cluster.
    {
        RailInput z = vertical(false, false);
        z.bar = {};
        const RailLayout l = computeRailLayout(z);
        assert(empty(l.settings) && empty(l.search) && empty(l.eqBox));

        RailInput narrow = vertical(false, false);
        narrow.bar = { 0, 0, 300, kT };       // narrower than the near cluster
        const RailLayout n = computeRailLayout(narrow);
        assert(n.search.left >= n.settings.right);
        for (int i = 0; i < kRailLetterCount; i++)
            assert(n.letters[i].right >= n.letters[i].left);
    }

    printf("rail_layout_test: all assertions passed\n");
    return 0;
}

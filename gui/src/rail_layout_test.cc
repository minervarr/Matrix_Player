// Asserts must stay live even though the app builds Release (NDEBUG).
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <initializer_list>

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
// rotating a W-by-T bar counter-clockwise sends (x, y) to (y, W - x).
LayoutRect rotateCcw(const LayoutRect& r) {
    return { r.top, kW - r.right, r.bottom, kW - r.left };
}

constexpr int kRects = kRailLetterCount + 4;   // letters + search, settings, close, eqBox
void collect(const RailLayout& l, const LayoutRect* (&out)[kRects]) {
    int n = 0;
    for (int i = 0; i < kRailLetterCount; i++) out[n++] = &l.letters[i];
    out[n++] = &l.search;
    out[n++] = &l.settings;
    out[n++] = &l.close;
    out[n++] = &l.eqBox;
}

// ── The two orientations are ONE layout and a rotation ─────────────────────
// If someone later special-cases an anchor for one orientation, this fails.
void assertRotationHolds(bool bitPerfect, bool searchOpen) {
    const RailLayout v = computeRailLayout(vertical(bitPerfect, searchOpen));
    const RailLayout h = computeRailLayout(horizontal(bitPerfect, searchOpen));
    const LayoutRect* vr[kRects];
    const LayoutRect* hr[kRects];
    collect(v, vr);
    collect(h, hr);
    for (int i = 0; i < kRects; i++) {
        // Hidden in one orientation must mean hidden in the other -- rotating
        // an empty rect would otherwise produce a plausible-looking rectangle.
        assert(empty(*vr[i]) == empty(*hr[i]));
        if (empty(*vr[i])) continue;
        assert(same(rotateCcw(*vr[i]), *hr[i]));
    }
}

}  // namespace

int main() {
    // ── The rare cells sit together at the near end: Settings, then Find ───
    {
        const RailLayout v = computeRailLayout(vertical(false, false));
        assert(v.settings.left == kPad);
        assert(v.search.left == v.settings.right);          // Find beside Settings
        assert(wide(v.search) == kCell && tall(v.search) == kT);
        assert(v.eqBox.left == v.search.right);             // then the EQ box
        assert(wide(v.eqBox) == kEq);                        // the box IS the name now
        for (int i = 1; i < kRailLetterCount; i++)
            assert(v.letters[i].left > v.letters[i - 1].left);
        assert(tall(v.settings) == kT && tall(v.letters[kRailAlbums]) == kT);
        assert(wide(v.letters[kRailAlbums]) == kCell);
    }

    // ── The whole point: Find is never next to a filter letter ─────────────
    // A slightly-off tap on Albums used to open search, because search was the
    // cell immediately beside it. Now the gap always stands between them.
    for (bool bp : { false, true }) {
        const RailLayout v = computeRailLayout(vertical(bp, false));
        assert(v.letters[kRailAlbums].left - v.search.right >= kGap);
        if (!bp) assert(v.letters[kRailAlbums].left - v.eqBox.right >= kGap);
    }

    // ── Bit-perfect CENTRES the letter group, Reference EQ PEGS it ─────────
    {
        const RailLayout ref = computeRailLayout(vertical(false, false));
        const RailLayout bp  = computeRailLayout(vertical(true,  false));
        assert(ref.letters[kRailPlaylists].right == kW - kPad);
        assert(bp.letters[kRailPlaylists].right < kW - kPad);
        assert(empty(bp.eqBox));

        // Centred in the space after the near cluster (Settings, Find, gap).
        // The old test asserted `slackA == slackB || slackA == slackB + 1`,
        // whose second branch can never hold: floor() centring leaves any odd
        // pixel on the FAR side, so it is slackB that may be one larger.
        const int freeA  = kPad + 2 * kCell + kGap;
        const int freeB  = kW - kPad;
        const int slackA = bp.letters[kRailAlbums].left - freeA;
        const int slackB = freeB - bp.letters[kRailPlaylists].right;
        assert(slackB - slackA == 0 || slackB - slackA == 1);

        // Centring moves the group, never resizes it.
        assert(ref.letters[kRailPlaylists].right - ref.letters[kRailAlbums].left ==
               bp.letters[kRailPlaylists].right  - bp.letters[kRailAlbums].left);
    }

    // ── Horizontal, asserted directly: the near end is the BOTTOM ──────────
    // This is the rotation of the block above, written out so a reader can
    // check it against the spec without doing the rotation in their head.
    {
        const RailLayout h = computeRailLayout(horizontal(false, false));
        assert(h.settings.bottom == kW - kPad);
        assert(h.search.bottom == h.settings.top);
        assert(h.eqBox.bottom == h.search.top);
        assert(h.eqBox.top - h.letters[kRailAlbums].bottom >= kGap);
        for (int i = 1; i < kRailLetterCount; i++)
            assert(h.letters[i].top < h.letters[i - 1].top);
        assert(h.letters[kRailPlaylists].top == kPad);
        assert(wide(h.settings) == kT && tall(h.letters[kRailAlbums]) == kCell);
    }

    // ── Open search: letters and box hide, Settings stays, field in between ─
    {
        const RailLayout v      = computeRailLayout(vertical(false, true));
        const RailLayout closed = computeRailLayout(vertical(false, false));
        for (int i = 0; i < kRailLetterCount; i++) assert(empty(v.letters[i]));
        assert(empty(v.eqBox));
        // Settings does NOT move: interrupting a filter to change a setting
        // must not cost what was typed.
        assert(same(v.settings, closed.settings));
        assert(v.close.right == kW - kPad && wide(v.close) == kCell);
        assert(v.search.left  == v.settings.right + kGap);
        assert(v.search.right == v.close.left - kGap);
        assert(wide(v.search) > kCell * 4);                  // a field, not a letter

        // Bit-perfect changes nothing while search is open: one state, not two.
        const RailLayout bpOpen = computeRailLayout(vertical(true, true));
        assert(same(bpOpen.search, v.search) && same(bpOpen.close, v.close));
    }

    // ── A narrow vertical bar shrinks every cell uniformly ─────────────────
    // A phone held upright: nine cells plus the box do not fit at 130.
    {
        RailInput in = vertical(false, false);
        in.bar = { 0, 0, 1080, kT };
        const RailLayout n = computeRailLayout(in);
        const int cell = wide(n.settings);
        assert(cell > 0 && cell < kCell);                    // shrunk, not dropped
        assert(wide(n.search) == cell);
        for (int i = 0; i < kRailLetterCount; i++) assert(wide(n.letters[i]) == cell);
        assert(n.search.left == n.settings.right && n.eqBox.left == n.search.right);
        assert(n.letters[kRailAlbums].left - n.eqBox.right >= kGap);
        assert(n.letters[kRailPlaylists].right == 1080 - kPad);
        for (int i = 1; i < kRailLetterCount; i++)
            assert(n.letters[i].left == n.letters[i - 1].right);   // flush, no gaps
    }

    // ── Every state, both orientations: one layout and a rotation ──────────
    // bit-perfect x search-open is the whole state space now; orientation is
    // the pair compared inside each call.
    for (bool bp : { false, true })
        for (bool open : { false, true })
            assertRotationHolds(bp, open);

    // ── Degenerate bars produce nothing, and never a negative rect ─────────
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

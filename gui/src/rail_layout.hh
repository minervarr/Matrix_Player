#pragma once
#include "layout_rect.hh"
#include "ui_orientation.hh"

// ── Where everything in bar A goes ───────────────────────────────────────────
//
// Bar A is the navigation half of the two-bar frame: the six release-type
// filters plus Playlists as single letters, the search letter, Settings, and
// the AutoEQ quick-switcher. Bar B (the transport) is laid out elsewhere.
//
// PURE. No Canvas, no Host, no theme, no metrics — just rectangles from
// rectangles, so rail_layout_test can assert every position without a window.
// This is the part of the design most likely to break in silence: a wrong
// anchor in one of the eight states (2 orientations x bit-perfect x search
// open) does not crash, it just looks wrong in a case nobody happened to open.
// Same split, and same reason, as ui_icons.cc vs ui_icons_draw.cc.
//
// ── The one axis ─────────────────────────────────────────────────────────────
//
// Horizontal is Vertical rotated 90 degrees COUNTER-CLOCKWISE, so everything
// below is computed once along the bar's long axis and mapped at the end. The
// axis runs from the SETTINGS end to the FAR end:
//
//   Vertical (top bar):    near = left,   far = right
//   Horizontal (left bar): near = bottom, far = top
//
// which is exactly what that rotation gives (the top bar's left end becomes
// the left bar's bottom end). rail_layout_test asserts the two orientations
// are that rotation of each other, rect by rect, so they cannot drift apart.
//
// ── The order along it ───────────────────────────────────────────────────────
//
//   near [ Settings ][ AutoEQ box ] · · · gap · · · [ Search ][ letters ] far
//
// Settings is pinned at the very near end and NEVER moves. That is not a
// stylistic choice: opening search must not cost what the listener already
// typed, so Settings has to stay reachable and in place while the filter
// letters collapse around it. Anything else on that end would push Settings
// when it appeared or vanished.
//
// The AutoEQ box sits just inside Settings, on the same end (left in Vertical,
// bottom in Horizontal). In bit-perfect there is nothing to pick a profile FOR,
// so the box does not exist and the letter group CENTRES in the space that is
// left; in Reference EQ the box exists and the group pegs to the far end.
// The jump between those two is instant, deliberately — no animation.
//
// With search open the filter letters and the AutoEQ box both hide, a close
// button takes the far end, and the field spans the middle. Because the box
// hides either way, open-search looks identical in bit-perfect and in
// Reference EQ — one state to draw, not two.

// The seven filter letters, in the order they are laid out from the near end
// outward. Deliberately the READING order, which is also the sidebar row order
// this replaces — not the Album::ReleaseType enum's order, which is frozen by
// the albums table and means nothing on screen.
enum RailLetter {
    kRailAlbums = 0,
    kRailEps,
    kRailSingles,
    kRailCompilations,
    kRailLive,
    kRailRemixes,
    kRailPlaylists,
    kRailLetterCount
};

struct RailInput {
    LayoutRect    bar{};                                // bar A, in window coords
    UiOrientation orient    = UiOrientation::Horizontal;
    bool          bitPerfect = false;                   // no AutoEQ box
    bool          searchOpen = false;
    // The AutoEQ list is unfurled over the letter group. It HIDES the letters
    // rather than floating above them, and that is forced by the renderer, not
    // chosen: every rect is emitted before every glyph (see the note on
    // rcChips_ in recalcLayout()), so an overlay's background can never cover
    // text drawn earlier -- the letters would show straight through it.
    bool          eqListOpen = false;

    // The DESIRED extent of one cell along the long axis; the cross-axis
    // extent is always the bar's full thickness, so a cell is square when this
    // equals it. Every cell SHRINKS uniformly when nine of them plus the
    // AutoEQ box cannot fit — which is the ordinary vertical bar, not a corner
    // case, since its long extent is the window's width. See the comment on
    // the clamp in rail_layout.cc.
    int cell = 0;
    // The AutoEQ box's extent along the long axis. Ignored when it is hidden.
    int eqBoxExtent = 0;
    // Outer inset at both ends, and the gap between the near cluster and the
    // letter group.
    int pad = 0;
    int gap = 0;
};

// Every rect is in window coordinates. A hidden element is returned as {} —
// an empty rect, which every hit-test in this codebase already misses.
struct RailLayout {
    LayoutRect letters[kRailLetterCount]{};  // empty while search or the EQ list is open
    LayoutRect search{};    // the search letter, or the text field while open
    LayoutRect settings{};
    LayoutRect close{};     // only while search is open
    LayoutRect eqBox{};     // empty when bit-perfect, or while search is open
};

RailLayout computeRailLayout(const RailInput& in);

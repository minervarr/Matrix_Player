#pragma once
#include "layout_rect.hh"
#include "ui_orientation.hh"

// ── Where everything in bar A goes ───────────────────────────────────────────
//
// Bar A is the navigation half of the two-bar frame: the six release-type
// filters plus Playlists as single letters, the Find cell, Settings, and the
// AutoEQ box. Bar B (the transport) is laid out elsewhere.
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
//   near [ Settings ][ Find ][ AutoEQ box ] · · · gap · · · [ letters ] far
//
// RARE cells together at the near end, FREQUENT cells alone at the far end,
// with the gap between them. Find used to be the cell right beside Albums, the
// same size as a filter letter, so a slightly-off tap on Albums opened search
// -- and opening search collapses every letter. It is now two cells and a gap
// away from the nearest filter.
//
// Settings is pinned at the very near end and NEVER moves: opening search must
// not cost what the listener already typed, so Settings stays reachable and in
// place while the letters collapse around it.
//
// The AutoEQ box follows Find. It shows the active profile's NAME and nothing
// else; touching it opens the EqSwitcher scene (player_view). In bit-perfect
// there is nothing to pick a profile FOR, so the box does not exist and the
// letter group CENTRES in the space that is left; in Reference EQ the group
// pegs to the far end. The jump between those two is instant -- no animation.
//
// With search open the letters and the box hide, a close button takes the far
// end, and the field spans the middle, starting after Settings. Open search
// looks identical in bit-perfect and in Reference EQ -- one state, not two.

// The seven filter letters, in the order they are laid out from the near end
// outward. Deliberately the READING order, not Album::ReleaseType's order,
// which is frozen by the albums table and means nothing on screen.
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
    LayoutRect letters[kRailLetterCount]{};  // empty while search is open
    LayoutRect search{};    // the Find cell, or the text field while open
    LayoutRect settings{};
    LayoutRect close{};     // only while search is open
    LayoutRect eqBox{};     // the active profile's name; empty when bit-perfect or searching
};

RailLayout computeRailLayout(const RailInput& in);

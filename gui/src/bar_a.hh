#pragma once
#include <string>
#include <vector>

#include "layout_rect.hh"
#include "rail_layout.hh"
#include "theme.hh"
#include "ui_metrics.hh"
#include "ui_orientation.hh"

class Canvas;

// ── Bar A, drawn and hit-tested from plain data ──────────────────────────────
//
// The navigation half of the two-bar frame. rail_layout.hh already decides
// WHERE everything goes; this decides what it LOOKS like and what a point in it
// means. Together they are the whole bar, and neither knows what application is
// drawing it.
//
// SHARED. It takes a Canvas and a struct of values and touches nothing else --
// no PlayerWindow, no Host, no Db, no AlbumTypeFilter, no OS. That is what lets
// the desktop and the Android slice draw the SAME bar rather than two bars that
// resemble each other. Before this file existed the drawing lived in
// player_view.cc as two methods reading fifteen members directly, and the phone
// showed a flat track list instead.
//
// The rule that keeps it shared, and it is checkable:
//
//     grep -n '^#include' bar_a.* | grep -E 'player_view|host|core/db|audio_output'
//
// must stay empty. If something here needs one of those, it is app state and
// belongs in the caller, arriving through BarAModel as a value.

// What a point in bar A means. Deliberately NOT the desktop's integer hit
// vocabulary (kSidebar*Hit, which folds AlbumTypeFilter's own values into its
// low end): that enum is frozen by the albums table and means nothing to a
// layer that draws letters. The caller translates at its own edge.
enum class BarAItem {
    None = 0,
    Filter,       // index = a RailLetter (kRailAlbums .. kRailRemixes)
    Playlists,
    Search,       // the Find cell that OPENS search -- never the open field
    SearchClose,
    Settings,
    EqBox,        // the active profile's name; touching it opens the EqSwitcher
};

struct BarAPick {
    BarAItem item  = BarAItem::None;
    int      index = -1;
};

inline bool operator==(const BarAPick& a, const BarAPick& b) {
    return a.item == b.item && a.index == b.index;
}
inline bool operator!=(const BarAPick& a, const BarAPick& b) { return !(a == b); }

// Everything bar A needs to draw itself, as values. Nothing here is a pointer
// into application state that could change between the layout pass and the
// paint, which is the whole point.
struct BarAModel {
    LayoutRect    bar{};
    UiOrientation orient = UiOrientation::Horizontal;
    RailLayout    rail{};
    UiMetrics     metrics{};

    bool searchOpen     = false;
    bool searchFocused  = false;
    bool settingsActive = false;
    bool eqSwitcherOpen = false;   // the box reads as selected while its scene is up

    std::string searchQuery;

    // A RailLetter, or -1 for none. A rail index rather than a filter value:
    // the reading order on screen is not the enum's order, and this layer is
    // the one that draws the screen.
    int  activeLetter    = -1;
    bool playlistsActive = false;

    // The AutoEQ box. Its presence is decided by the geometry (rail.eqBox is
    // empty in bit-perfect and while searching), so there is no second flag.
    bool        eqNone      = true;   // no profile is selected
    bool        eqTentative = false;  // selected, not yet credited
    std::string eqName;

    // What the pointer is over. Android leaves this None and nothing else
    // changes: the shared code never asks which platform it is on.
    BarAPick hovered;
};

void     drawBarA(Canvas& canvas, const BarAModel& m);
BarAPick barAHitTest(const BarAModel& m, int x, int y);

// One text-input field, shared by bar A's search and the EQ panel's profile
// search. It lived as a file-static in player_view.cc; it is declared here
// because bar A needs it and a second copy is how two search boxes start
// looking different.
void drawSearchField(Canvas& canvas, const LayoutRect& rc, const std::string& text,
                     bool focused, const char* placeholder, float textSize);

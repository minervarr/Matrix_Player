#pragma once
#include <string>
#include <vector>
#include "layout_rect.hh"
#include "color.hh"

class Canvas;
enum class FontStyle : unsigned char;

// vk_canvas-native replacements for the four native Win32 dialogs (Manage
// Folders / Audio Settings / EQ Settings / SHBrowseForFolderW). Built once,
// used identically on both platforms — see CLAUDE.md's Phase 7 decision:
// the app is fully custom-rendered everywhere else, so these shouldn't be
// generic OS chrome bolted on. All four reuse PlayerWindow's existing
// full-page-view pattern (like the album view) rather than a modal popup —
// Wayland has no child/owned-window primitive matching Win32's modal dialogs.

enum class SettingsPanel { None, ManageFolders, AudioSettings, EqSettings, FolderPicker };

namespace panels {

// One row's screen rect within a scrollable list area, given the list's
// current scroll offset — the same "compute during draw, hit-test reads it
// back" split PlayerWindow's trackPanelHitTest() already uses.
LayoutRect rowRect(const LayoutRect& area, int index, int rowH, int scrollY);

// Row index at (x,y), or -1 if outside the area or past the last row.
// rowCount bounds the result so a click below the last real row misses.
int hitTestRows(const LayoutRect& area, int rowH, int scrollY, int rowCount, int x, int y);

// Draws a vertically-scrolled list of single-line text rows: hover highlight,
// top/bottom separators, optional accent border + text color on the selected
// row. Clips to `area` so rows scrolled partway out don't bleed past it.
void drawRowList(Canvas& canvas, const LayoutRect& area,
                  const std::vector<std::string>& labels,
                  int rowH, int scrollY, int hoverIdx, int selectedIdx,
                  float textSize, float uiScale);

// A small rectangular action button (Done/Cancel/Remove/Assign/Select),
// right-aligned text inside a border, matching the settings-page row style.
void drawButton(Canvas& canvas, const LayoutRect& rc, const std::string& label,
                 bool hover, float textSize, bool primary = false);

// Panel chrome: title bar + "Close" affordance. Returns the content area
// below the header (what the panel's own drawing should treat as its rect).
// closeRc receives the close button's hit-test rect (top-right corner).
LayoutRect drawHeader(Canvas& canvas, const LayoutRect& area, const std::string& title,
                      float uiScale, float headerTextSize, LayoutRect& closeRc);

} // namespace panels

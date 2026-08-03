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

// Playlists used to be a sixth value here, borrowing the overlay to draw in.
// It is NOT one any more, and must not become one again: the same four input
// dispatchers (onPanelClick / onPanelMouseMove / onPanelWheel /
// onPanelKeyDown) that make this enum cheap to extend also divert every event
// to the panel BEFORE the sidebar or the transport bar is hit-tested — which
// is exactly right for a settings dialog and exactly wrong for a way of
// browsing music. While Playlists was in here you could not click Singles,
// could not click Settings, and could not press Space to stop the music. It is
// a top-level section now (PlayerWindow::NavSection).
enum class SettingsPanel { None, ManageFolders, AudioSettings, EqSettings, FolderPicker };

// The scrollable/selectable text row list these panels used to draw with a
// local panels::drawRowList (+ rowRect/hitTestRows) now comes from the
// framework: widgets::drawScrollList (framework/vk_canvas/core/widgets.hh),
// styled per-app via PlayerWindow::matrixListStyle(). Its returned ListRow
// rects are cached and hit-tested by PlayerWindow::hitTestListRows().

namespace panels {

// A small rectangular action button (Done/Cancel/Remove/Assign/Select),
// right-aligned text inside a border, matching the settings-page row style.
void drawButton(Canvas& canvas, const LayoutRect& rc, const std::string& label,
                 bool hover, float textSize, bool primary = false);

// Panel chrome: title bar + "Close" affordance. Returns the content area
// below the header (what the panel's own drawing should treat as its rect).
// closeRc receives the close button's hit-test rect (top-right corner).
LayoutRect drawHeader(Canvas& canvas, const LayoutRect& area, const std::string& title,
                      float scale, float headerTextSize, LayoutRect& closeRc);

// Overflow indicator for a widgets::drawScrollList viewport: a thin track +
// proportional thumb docked inside the list's right edge. Draws nothing when
// the content fits (contentH <= listArea height), so callers can call it
// unconditionally. Purely an affordance — it is not hit-tested or draggable;
// scrolling stays on the wheel (PlayerWindow::onPanelWheel).
//
// drawScrollList clips overflowing rows away silently, with no visual hint
// they exist — a USB DAC sitting in row 7 of a 6-row viewport was invisible
// and unreachable-looking. Every panel that scrolls should draw this.
void drawScrollbar(Canvas& canvas, const LayoutRect& listArea,
                   int contentH, int scrollY, float scale);

} // namespace panels

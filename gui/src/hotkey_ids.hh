#pragma once
// Edge-snap/mode-toggle hotkey IDs — shared between PlayerWindow::onHotkey()
// and both Host backends. Windows: real system-wide hotkeys (RegisterHotKey).
// Linux: no cross-compositor global-hotkey equivalent, so these become
// focused-window-only Alt+key checks in LinuxHost — a deliberate, documented
// behavior narrowing (works only while the window has focus), not a silent
// drop. See host.hh's class comment.
enum HotkeyId {
    kHotkeySnapLeft = 1,
    kHotkeySnapRight,
    kHotkeySnapBottom,
    kHotkeySnapTop,
    kHotkeySnapCenterG,  // Alt+G and Alt+H both center — two ids, same action
    kHotkeySnapCenterH,
    kHotkeyToggleOrientation,  // Alt+L: Horizontal <-> Vertical
};

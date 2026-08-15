#pragma once

// ── The one orientation ──────────────────────────────────────────────────────
//
// Which of the two layouts the UI draws. NOT a window size: this is derived
// from the shape the window ALREADY has, never asked of the OS. That is the
// whole reason it works identically on Wayland (where a client cannot position
// or size itself), on Windows, and on Android (where a device rotation arrives
// as an ordinary resize, since the manifest declares configChanges=orientation
// and locks no screenOrientation).
//
// Replaces UiMode{Essential, Complete}, which was a window rectangle wearing a
// mode's name — both hosts used it only to pick a RECT, and asking the
// compositor for one is why the old toggle felt slow on Wayland and why
// snapToEdge()/adaptToCurrentMonitor() were dead no-ops there. See
// docs/superpowers/specs/2026-08-12-orientation-modes-design.md.
//
// Horizontal is Vertical rotated 90 degrees counter-clockwise. There are not
// two layouts; there is one layout and a rotation.
//
// NAMED ui_orientation, not orientation, on purpose: vk_canvas's Android
// platform already ships framework/vk_canvas/platform/android/orientation.hh
// (vce::platform::Orientation — PHYSICAL device orientation from the
// accelerometer), and android/CMakeLists.txt compiles it. Two headers of one
// basename on one include path is a trap, and the two concepts are genuinely
// different. This also matches the ui_metrics/ui_icons/ui_fonts family in this
// directory.
enum class UiOrientation { Horizontal, Vertical };

// The automatic answer for a window of this size.
//
// A tie (w == h) resolves to Horizontal deliberately: something has to win, and
// Horizontal is what every existing monitor, capture and screenshot already
// assumes. Degenerate sizes (a Wayland surface configured 0x0 before its first
// real configure) fall out of the same comparison rather than needing a guard —
// the answer is defined, not accidental.
UiOrientation autoOrientationFor(int windowW, int windowH);

// Automatic by default, with a manual override that sticks.
//
// `manual` is meaningless while `autoEnabled` is true, and toggleManual() seeds
// it from what auto WOULD have said. Without that seeding the first key press
// on a vertical monitor would appear to do nothing.
struct UiOrientationState {
    bool          autoEnabled = true;
    UiOrientation manual      = UiOrientation::Horizontal;

    UiOrientation resolve(int windowW, int windowH) const;

    // Leaves automatic mode (if it was on) and flips to the other layout.
    void toggleManual(int windowW, int windowH);
};

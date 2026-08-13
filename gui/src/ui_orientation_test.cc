// Asserts must stay live even though the app builds Release (NDEBUG).
#undef NDEBUG
#include <cassert>
#include <cstdio>

#include "ui_orientation.hh"

int main() {
    // ── Wider than tall is Horizontal; taller than wide is Vertical ──────────
    assert(autoOrientationFor(1920, 1080) == UiOrientation::Horizontal);
    assert(autoOrientationFor(1080, 1920) == UiOrientation::Vertical);
    assert(autoOrientationFor(2560, 1440) == UiOrientation::Horizontal);

    // A phone in portrait and the same phone in landscape are the two cases
    // Android delivers as ordinary resizes — no sensor is ever consulted.
    assert(autoOrientationFor(1080, 2400) == UiOrientation::Vertical);
    assert(autoOrientationFor(2400, 1080) == UiOrientation::Horizontal);

    // ── An exactly square window is Horizontal, deliberately ────────────────
    // Something must win, and Horizontal is the layout every existing monitor
    // and every existing capture already assumes. Ties never flip.
    assert(autoOrientationFor(1000, 1000) == UiOrientation::Horizontal);

    // ── Degenerate sizes never crash and never flip ─────────────────────────
    // A Wayland surface can be configured 0x0 before its first real configure
    // arrives; the answer must be defined rather than accidental.
    assert(autoOrientationFor(0, 0)   == UiOrientation::Horizontal);
    assert(autoOrientationFor(0, 500) == UiOrientation::Vertical);
    assert(autoOrientationFor(500, 0) == UiOrientation::Horizontal);

    // ── Auto state follows the window ───────────────────────────────────────
    UiOrientationState st;                       // auto is the default
    assert(st.autoEnabled);
    assert(st.resolve(1920, 1080) == UiOrientation::Horizontal);
    assert(st.resolve(1080, 1920) == UiOrientation::Vertical);

    // ── Toggling takes manual control, and STICKS ───────────────────────────
    // The listener asked for the other layout; a later resize must not silently
    // take it back, which is the whole point of the override.
    st.toggleManual(1920, 1080);
    assert(!st.autoEnabled);
    assert(st.resolve(1920, 1080) == UiOrientation::Vertical);
    assert(st.resolve(1080, 1920) == UiOrientation::Vertical);

    // ── Toggling again flips the manual choice, still manual ────────────────
    st.toggleManual(1920, 1080);
    assert(!st.autoEnabled);
    assert(st.resolve(1080, 1920) == UiOrientation::Horizontal);

    // ── The first toggle flips away from what auto WOULD have said ──────────
    // Not from a stored default: pressing the key on a vertical monitor must
    // give Horizontal, otherwise the first press appears to do nothing.
    UiOrientationState st2;
    st2.toggleManual(1080, 1920);                // auto here would say Vertical
    assert(st2.resolve(1080, 1920) == UiOrientation::Horizontal);

    // ── Re-enabling auto discards the manual choice ─────────────────────────
    st2.autoEnabled = true;
    assert(st2.resolve(1080, 1920) == UiOrientation::Vertical);

    printf("ui_orientation_test: all assertions passed\n");
    return 0;
}

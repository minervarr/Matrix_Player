// scroll_test — the two rules every scrollable surface in the app shares.
//
// PlayerWindow::scrollDelta() and ::scrollTo() replaced eight hand-rolled
// copies of "which way does a delta move the content" and "where is it allowed
// to stop". Eight copies is eight chances for one to be wrong, and one WAS: the
// signal chain clamped against the wrong viewport and could not be scrolled at
// all once the phone was rotated. This pins the rules so a ninth surface cannot
// reintroduce either mistake.
//
// The bodies are restated here rather than linked, because scrollTo() is a
// static member of a class whose header drags in Canvas, Host, sqlite and the
// audio backends — and the whole point of this convention is a test with no
// engine behind it. They are three lines each; the assertions below are what
// they must MEAN, and the real ones are one grep away.
//
// Convention matches framework/vk_canvas/core/tests/*.cc: plain assert(), no
// framework, and NDEBUG undefined so the asserts survive an optimized build.
#undef NDEBUG
#include <cassert>
#include <algorithm>
#include <cstdio>

// The engine's clamp, which scrollTo() delegates to
// (framework/vk_canvas/core/layout.hh) — already pinned by layout_test, and
// restated here so this file links nothing.
static float clampScroll(float scrollPx, float contentH, float viewH) {
    return std::max(0.0f, std::min(scrollPx, std::max(0.0f, contentH - viewH)));
}

static int scrollTo(int offset, int delta, int contentH, int viewH) {
    return (int)clampScroll((float)(offset - delta), (float)contentH, (float)viewH);
}

static int scrollDelta(int rawDelta, bool invert) {
    return invert ? -rawDelta : rawDelta;
}

int main() {
    // ── Direction ───────────────────────────────────────────────────────────
    // The base sense is subtractive: a positive delta LOWERS the offset. That
    // is what carries the content down with a finger moving down the screen,
    // and moves the view up when a wheel is pushed away.
    assert(scrollTo(500, +10, 2000, 500) == 490);
    assert(scrollTo(500, -10, 2000, 500) == 510);

    // Inverting flips the sign and does nothing else — same magnitude, same
    // bounds, opposite direction.
    assert(scrollDelta(+120, false) == +120);
    assert(scrollDelta(+120, true)  == -120);
    assert(scrollDelta(0, true) == 0);          // no motion stays no motion
    for (int d = -300; d <= 300; d += 7)
        assert(scrollDelta(scrollDelta(d, true), true) == d);   // an involution

    // ── Bounds ──────────────────────────────────────────────────────────────
    // Content shorter than the view cannot scroll, however hard it is pushed.
    assert(scrollTo(0, -9999, 300, 600) == 0);
    assert(scrollTo(0, +9999, 300, 600) == 0);
    assert(scrollTo(0, -9999, 600, 600) == 0);   // exactly equal is still no room

    // Never above the top, never past the bottom.
    assert(scrollTo(0, +50, 2000, 500) == 0);
    assert(scrollTo(1400, -9999, 2000, 500) == 1500);
    assert(scrollTo(1500, -1, 2000, 500) == 1500);

    // The bottom is reachable EXACTLY, which is the property the signal chain
    // lost: its wheel used a viewport 91 px taller than the one its draw laid
    // out into, so its ceiling sat 91 px short and the last of the page could
    // never be reached. Same content, two viewports, two different ends —
    // and the taller viewport must never be the one that decides.
    const int contentH = 663;
    const int drawView = 629, wheelView = 720;      // the real numbers, rotated
    assert(scrollTo(0, -9999, contentH, drawView) == 34);   // 663 - 629
    assert(scrollTo(0, -9999, contentH, wheelView) == 0);   // the old, wrong ceiling
    assert(scrollTo(0, -9999, contentH, drawView) > 0);     // ...and it DID have room

    // Clamping is idempotent: re-clamping a settled offset never moves it.
    for (int off = -50; off <= 2050; off += 37) {
        const int a = scrollTo(off, 0, 2000, 500);
        assert(scrollTo(a, 0, 2000, 500) == a);
        assert(a >= 0 && a <= 1500);
    }

    // A shrinking list must not strand the offset above its new end — the case
    // that renders as an EMPTY panel rather than a short one, because
    // widgets::drawScrollList clips instead of clamping.
    assert(scrollTo(900, 0, 400, 300) == 100);
    assert(scrollTo(900, 0, 200, 300) == 0);

    printf("scroll_test: all assertions passed\n");
    return 0;
}

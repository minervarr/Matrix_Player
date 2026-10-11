#pragma once

// Wheel paging for the album view. Pure: no clock, no window, no Canvas.
//
// One disc is one page, and the artist text is the page after the last disc.
// The roll that is moving the track list must not also turn that page — a
// notch that merely arrives at the end belongs to the roll. The turn is a
// different gesture: two wheel detents, close together, after that roll has
// gone quiet. The second detent turns the page. It does not start another wait.
//
// The quiet and the "close together" window are the same length on purpose.
// A steady roll then never satisfies the pair (each notch is still the roll),
// and a pair that is not fast never satisfies it either. Touch does not use
// this. A finger has a lift, and that lift is the gap.

#include <cstdint>

// Win32 WHEEL_DELTA. Wayland's axis value is scaled into these units before
// it reaches the app, so one physical notch is 120 on both desktops.
inline constexpr int kAlbumWheelDetent = 120;

// Notches closer than this are one gesture. A pair counts only when both
// detents land inside one window, and the first of them lands after the
// previous gesture has been quiet for the same length.
inline constexpr int kAlbumPageGestureMs = 320;

struct AlbumPager {
    // While `arriving` and now < tailUntilMs, notches belong to the roll
    // that reached the boundary. They are not half of the pair.
    int64_t tailUntilMs = 0;
    bool    arriving    = false;
    // Wheel units collected toward the pair. A new window starts this over.
    int64_t pairStartMs = 0;
    int     accumulated = 0;
};

enum class AlbumPagerResult { Stay, Turn };

// `delta` is in wheel units, sign ignored: the caller has already decided
// this notch points past the boundary. `arrivedNow` is the notch that
// reached the boundary by scrolling the list. It arms the tail and does
// not count.
inline AlbumPagerResult albumPagerWheel(AlbumPager& p, int delta, int64_t nowMs,
                                        bool arrivedNow) {
    if (arrivedNow) {
        p.arriving    = true;
        p.tailUntilMs = nowMs + kAlbumPageGestureMs;
        p.pairStartMs = 0;
        p.accumulated = 0;
        return AlbumPagerResult::Stay;
    }
    if (p.arriving && nowMs < p.tailUntilMs) {
        p.tailUntilMs = nowMs + kAlbumPageGestureMs;
        p.pairStartMs = 0;
        p.accumulated = 0;
        return AlbumPagerResult::Stay;
    }
    p.arriving = false;
    if (p.pairStartMs == 0 || nowMs - p.pairStartMs > kAlbumPageGestureMs) {
        p.pairStartMs = nowMs;
        p.accumulated = 0;
    }
    const int mag = delta < 0 ? -delta : delta;
    p.accumulated += mag;
    if (p.accumulated >= 2 * kAlbumWheelDetent) {
        // The rest of this roll is not a second turn. The next pair has to
        // be its own gesture, or one flick would walk every disc.
        p.arriving    = true;
        p.tailUntilMs = nowMs + kAlbumPageGestureMs;
        p.pairStartMs = 0;
        p.accumulated = 0;
        return AlbumPagerResult::Turn;
    }
    return AlbumPagerResult::Stay;
}

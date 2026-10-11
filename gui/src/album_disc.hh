#pragma once

#include <algorithm>

// One disc page's scroll extent. Pure: no Canvas, no window, no clock.
//
// Height is a function of the disc alone. It used to be measured as
// (cursor - scroll) + scroll while a 1<<30 stand-in meant "the end".
// At that magnitude a 40 px row adds 64, so the measured page is not
// this one and the clamp stops past the last row. The frame that
// measured it had already painted every row above the clip, and the
// repaint it asked for was discarded. Walking back a disc showed the
// new label over an empty list until a later event redraws the
// settled offset.

inline int albumDiscContentH(float headerH, int trackCount, int pitch,
                            float extraH, float pad) {
    if (trackCount < 0) trackCount = 0;
    if (pitch < 0) pitch = 0;
    const float h = headerH + (float)trackCount * (float)pitch + extraH + pad;
    if (!(h > 0.f)) return 0;
    if (h >= 2147483647.f) return 2147483647;
    return (int)h;
}

// The offset that puts the page on its last whole row. 0 when the page
// fits, so a short disc is both its start and its end.
inline int albumDiscEndScroll(int contentH, int viewH, int pitch) {
    if (pitch <= 0 || contentH <= viewH) return 0;
    return ((contentH - viewH) / pitch) * pitch;
}

// What the OTHER VERSIONS / MORE REMIXES strip adds to the page. The last
// disc is the only one that carries it. `captionBlock` is the caption's
// own advance (the draw adds caption * 2.2 before the tiles).
inline float albumVariantStripH(int count, float listW, float artW,
                               float gapX, float tileH, float rowGap,
                               float captionBlock) {
    if (count <= 0) return 0.f;
    const float stepX = artW + gapX;
    int perRow = 1;
    if (stepX > 0.f)
        perRow = std::max(1, (int)((listW + gapX) / stepX));
    const int rows = (count + perRow - 1) / perRow;
    return captionBlock + (float)rows * (tileH + rowGap);
}

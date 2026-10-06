#pragma once

// Settings radios and switches, as pixels of the Terminus face beside them.
//
// A vector circle (canvas.rect with radius = half the side) is an analytic
// curve. Terminus is a 1-bit bitmap, drawn at an integer scale and never
// resampled. The two resolutions cannot sit on one row: the letter is a
// staircase and the control is a perfectly smooth disc. These marks are
// authored in the face's own pixels — the 16px strike, then multiplied by
// the same scale drawTerminusText picked — so a step in the circle is the
// same size as a step in the letter, on every window.
//
// They stay hard pixels because shape_frag caps a radius-0 box's coverage
// band at one pixel. The derivative quad overestimates a corner (it reports
// ~2) and that extra width blurs a 1px step back into a smooth circle,
// which is the whole failure these masks exist to avoid.
//
// The circle is the roundest 13px disc whose outline steps one pixel at a
// time (no single-pixel spikes, four-fold symmetric). The ring is that disc
// minus a centered 9px disc, which leaves the face's 2px bold stroke. The
// switch ends are that same ring, extruded. Pure: no Canvas.

#include <cstdint>
#include <cmath>
#include <vector>

#include "terminus_glyph.hh"

namespace panels {

struct PxMark {
    int w;
    int h;
    const std::uint32_t* rows;   // bit 0 = leftmost pixel
};

struct PxRun { int x, y, w, h; };

// One Terminus cell, the gap between a mark and the word it belongs to.
constexpr int kPxCell = 8;
// The knob sits this many font-pixels in from the track's square edge.
// 2 is the stroke, so the knob rests in the hole and the outer silhouette
// of the pill stays visible.
constexpr int kToggleKnobInset = 2;

inline const PxMark& radioDisc() {
    // ....#####....
    // ...#######...
    // ..#########..
    // .###########.
    // #############
    static constexpr std::uint32_t kRows[] = {
        0x000001F0, 0x000003F8, 0x000007FC, 0x00000FFE, 0x00001FFF,
        0x00001FFF, 0x00001FFF, 0x00001FFF, 0x00001FFF, 0x00000FFE,
        0x000007FC, 0x000003F8, 0x000001F0,
    };
    static constexpr PxMark m{13, 13, kRows};
    return m;
}

inline const PxMark& radioRing() {
    static constexpr std::uint32_t kRows[] = {
        0x000001F0, 0x000003F8, 0x0000060C, 0x00000C06, 0x00001803,
        0x00001803, 0x00001803, 0x00001803, 0x00001803, 0x00000C06,
        0x0000060C, 0x000003F8, 0x000001F0,
    };
    static constexpr PxMark m{13, 13, kRows};
    return m;
}

inline const PxMark& radioDot() {
    static constexpr std::uint32_t kRows[] = {
        0x0000001C, 0x0000003E, 0x0000007F, 0x0000007F,
        0x0000007F, 0x0000003E, 0x0000001C,
    };
    static constexpr PxMark m{7, 7, kRows};
    return m;
}

inline const PxMark& toggleFill() {
    static constexpr std::uint32_t kRows[] = {
        0x003FFFF0, 0x007FFFF8, 0x00FFFFFC, 0x01FFFFFE, 0x03FFFFFF,
        0x03FFFFFF, 0x03FFFFFF, 0x03FFFFFF, 0x03FFFFFF, 0x01FFFFFE,
        0x00FFFFFC, 0x007FFFF8, 0x003FFFF0,
    };
    static constexpr PxMark m{26, 13, kRows};
    return m;
}

inline const PxMark& toggleOutline() {
    static constexpr std::uint32_t kRows[] = {
        0x003FFFF0, 0x007FFFF8, 0x00C0000C, 0x01800006, 0x03000003,
        0x03000003, 0x03000003, 0x03000003, 0x03000003, 0x01800006,
        0x00C0000C, 0x007FFFF8, 0x003FFFF0,
    };
    static constexpr PxMark m{26, 13, kRows};
    return m;
}

inline const PxMark& toggleKnob() {
    static constexpr std::uint32_t kRows[] = {
        0x0000007C, 0x000000FE, 0x000001FF, 0x000001FF, 0x000001FF,
        0x000001FF, 0x000001FF, 0x000000FE, 0x0000007C,
    };
    static constexpr PxMark m{9, 9, kRows};
    return m;
}

inline int radioDotOffset() {
    return (radioDisc().w - radioDot().w) / 2;
}

inline int toggleKnobRow() {
    return (toggleFill().h - toggleKnob().h) / 2;
}

inline int toggleKnobColumn(bool on) {
    const int kw = toggleKnob().w;
    return on ? toggleFill().w - kToggleKnobInset - kw : kToggleKnobInset;
}

inline void pxRuns(const PxMark& m, int scale, std::vector<PxRun>& out) {
    out.clear();
    const int s = scale > 0 ? scale : 1;
    for (int y = 0; y < m.h; ++y) {
        const std::uint32_t bits = m.rows[y];
        int x0 = -1;
        for (int x = 0; x <= m.w; ++x) {
            const bool on = x < m.w && ((bits >> x) & 1u);
            if (on && x0 < 0) {
                x0 = x;
            } else if (!on && x0 >= 0) {
                out.push_back(PxRun{ x0 * s, y * s, (x - x0) * s, s });
                x0 = -1;
            }
        }
    }
}

struct PxPlace { float x, y; int scale; };

// `prefX` is the desired left edge. It is snapped onto the grid
// terminus::origin(gridOriginX, textY) defines, and the mark is centered on
// the capital band (glyph rows 2..12). An odd leftover goes above the
// capitals, into the blank rows the face already leaves there.
inline PxPlace placePxMark(float gridOriginX, float textY, float targetPx,
                           float prefX, int markH) {
    const terminus::Origin o = terminus::origin(gridOriginX, textY, targetPx);
    const int scale = o.scale > 0 ? o.scale : 1;
    const int spare = 10 - markH;
    const int topRow = 2 + (spare >= 0 ? spare / 2 : (spare - 1) / 2);
    const int k = (int)std::lround((prefX - o.penX) / (float)scale);
    PxPlace p;
    p.x = o.penX + (float)(k * scale);
    p.y = o.top + (float)(topRow * scale);
    p.scale = scale;
    return p;
}

} // namespace panels

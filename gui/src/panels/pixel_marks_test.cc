// pixel_marks_test -- the settings radio and switch bitmaps.
// Header-only, plus terminus::origin so the grid is the face's grid.
// Convention matches terminus_glyph_test: plain assert(), NDEBUG undefined.
#undef NDEBUG
#include <cassert>
#include <cmath>
#include <vector>

#include "panels/pixel_marks.hh"

static bool bit(const panels::PxMark& m, int x, int y) {
    return x >= 0 && y >= 0 && x < m.w && y < m.h && ((m.rows[y] >> x) & 1u);
}

static void expectSymmetric(const panels::PxMark& m) {
    for (int y = 0; y < m.h; ++y) {
        for (int x = 0; x < m.w; ++x) {
            assert(bit(m, x, y) == bit(m, m.w - 1 - x, y));
            assert(bit(m, x, y) == bit(m, x, m.h - 1 - y));
            if (m.w == m.h)
                assert(bit(m, x, y) == bit(m, y, x));
        }
    }
}

static int minRun(const panels::PxMark& m) {
    int mr = 99;
    for (int y = 0; y < m.h; ++y) {
        int run = 0;
        for (int x = 0; x <= m.w; ++x) {
            if (bit(m, x, y)) run++;
            else if (run) { if (run < mr) mr = run; run = 0; }
        }
    }
    for (int x = 0; x < m.w; ++x) {
        int run = 0;
        for (int y = 0; y <= m.h; ++y) {
            if (bit(m, x, y)) run++;
            else if (run) { if (run < mr) mr = run; run = 0; }
        }
    }
    return mr;
}

static void expectRuns(const panels::PxMark& m, int scale) {
    std::vector<panels::PxRun> rs;
    panels::pxRuns(m, scale, rs);
    const int W = m.w * scale, H = m.h * scale;
    std::vector<int> hit((size_t)W * H, 0);
    for (const panels::PxRun& r : rs) {
        assert(r.w > 0 && r.h > 0);
        assert(r.x >= 0 && r.y >= 0 && r.x + r.w <= W && r.y + r.h <= H);
        for (int y = r.y; y < r.y + r.h; ++y)
            for (int x = r.x; x < r.x + r.w; ++x)
                hit[(size_t)y * W + x]++;
    }
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const bool on = bit(m, x / scale, y / scale);
            assert(hit[(size_t)y * W + x] == (on ? 1 : 0));
        }
}

int main() {
    const panels::PxMark& disc = panels::radioDisc();
    const panels::PxMark& ring = panels::radioRing();
    const panels::PxMark& dot  = panels::radioDot();
    const panels::PxMark& fill = panels::toggleFill();
    const panels::PxMark& out  = panels::toggleOutline();
    const panels::PxMark& knob = panels::toggleKnob();

    assert(disc.w == 13 && disc.h == 13);
    assert(ring.w == 13 && ring.h == 13);
    assert(dot.w == 7 && dot.h == 7);
    assert(fill.w == 26 && fill.h == 13);
    assert(out.w == 26 && out.h == 13);
    assert(knob.w == 9 && knob.h == 9);

    // A square with the corners filled is not a circle, and a one-pixel
    // spike is not this face: Terminus Bold's thinnest mark is 2px.
    for (const panels::PxMark* m : { &disc, &ring, &dot, &fill, &out, &knob }) {
        expectSymmetric(*m);
        assert(minRun(*m) >= 2);
    }
    assert(!bit(disc, 0, 0));
    assert(bit(disc, disc.w / 2, 0));

    // The ring is the disc's stroke, and the dot sits in the hole.
    const int off = panels::radioDotOffset();
    assert(off == 3);
    for (int y = 0; y < ring.h; ++y)
        for (int x = 0; x < ring.w; ++x)
            if (bit(ring, x, y)) assert(bit(disc, x, y));
    for (int y = 0; y < dot.h; ++y)
        for (int x = 0; x < dot.w; ++x)
            if (bit(dot, x, y)) {
                assert(bit(disc, x + off, y + off));
                assert(!bit(ring, x + off, y + off));
            }

    // The pill's ends are the radio's ends: same inset on every row.
    for (int y = 0; y < disc.h; ++y) {
        int discInset = 0;
        while (discInset < disc.w && !bit(disc, discInset, y)) discInset++;
        int fillInset = 0;
        while (fillInset < fill.w && !bit(fill, fillInset, y)) fillInset++;
        assert(discInset == fillInset);
    }

    // Outline is the fill's stroke. The knob, off and on, stays inside the
    // fill and does not eat the outline's outer silhouette.
    for (int y = 0; y < out.h; ++y)
        for (int x = 0; x < out.w; ++x)
            if (bit(out, x, y)) assert(bit(fill, x, y));
    const int ky = panels::toggleKnobRow();
    assert(ky == 2);
    for (bool on : { false, true }) {
        const int kx = panels::toggleKnobColumn(on);
        for (int y = 0; y < knob.h; ++y)
            for (int x = 0; x < knob.w; ++x)
                if (bit(knob, x, y))
                    assert(bit(fill, x + kx, y + ky));
        for (int y = 0; y < out.h; ++y) {
            int lo = 0;
            while (lo < out.w && !bit(out, lo, y)) lo++;
            int hi = out.w - 1;
            while (hi >= 0 && !bit(out, hi, y)) hi--;
            if (lo > hi) continue;
            for (int x : { lo, hi }) {
                const int dx = x - kx, dy = y - ky;
                if (dx >= 0 && dy >= 0 && dx < knob.w && dy < knob.h)
                    assert(!bit(knob, dx, dy));
            }
        }
    }

    for (const panels::PxMark* m : { &disc, &ring, &dot, &fill, &out, &knob })
        for (int s = 1; s <= 3; ++s)
            expectRuns(*m, s);

    // The mark hangs on the same grid as the letters, centered on the capitals.
    {
        const terminus::Origin o = terminus::origin(10.7f, 100.2f, 20.0f);
        assert(o.scale == 1);
        assert(o.penX == 10.0f);
        assert(o.top == std::floor(100.2f + (20.0f - 16.0f) * 0.5f));
        const panels::PxPlace p = panels::placePxMark(10.7f, 100.2f, 20.0f, 40.0f, 13);
        assert(p.scale == 1);
        assert(p.y == o.top);                 // 13 rows -> font row 0
        assert(p.x == 40.0f);
        const panels::PxPlace odd = panels::placePxMark(10.7f, 100.2f, 20.0f, 40.0f, 11);
        assert(odd.y == o.top + 1.0f);        // leftover pixel above the caps
    }
    {
        const terminus::Origin o = terminus::origin(10.4f, 100.5f, 30.0f);
        assert(o.scale == 2);
        const panels::PxPlace p = panels::placePxMark(10.4f, 100.5f, 30.0f, 51.0f, 13);
        assert(p.scale == 2);
        assert(p.y == o.top);
        // 51 is halfway between grid lines; lround ties away from zero.
        assert(p.x == 52.0f);
        assert(std::fmod(p.x - o.penX, (float)p.scale) == 0.0f);
    }
    return 0;
}

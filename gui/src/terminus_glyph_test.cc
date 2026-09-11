// terminus_glyph_test -- the baked Terminus glyph and its run-length rects.
// Links src/terminus_glyph.cc and nothing else. Convention matches
// framework/vk_canvas/core/tests/*.cc: plain assert(), NDEBUG undefined.
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <vector>

#include "terminus_glyph.hh"

// Paint runs into a grid and compare with the glyph's bits expanded by
// `scale`: every set bit covered exactly once, no unset bit covered at all.
static void checkExact(const terminus::Glyph& g, int scale) {
    std::vector<terminus::Run> rs;
    terminus::runs(g, scale, rs);
    const int W = g.cellW * scale, H = g.cellH * scale;
    std::vector<int> hit((size_t)W * H, 0);
    for (const terminus::Run& r : rs) {
        assert(r.w > 0 && r.h > 0);
        assert(r.x >= 0 && r.y >= 0 && r.x + r.w <= W && r.y + r.h <= H);
        for (int y = r.y; y < r.y + r.h; ++y)
            for (int x = r.x; x < r.x + r.w; ++x) hit[(size_t)y * W + x]++;
    }
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const bool on = (g.rows[y / scale] >> (x / scale)) & 1u;
            assert(hit[(size_t)y * W + x] == (on ? 1 : 0));
        }
}

int main() {
    // The one glyph bar A draws exists in both strikes and is not blank.
    const terminus::Pick small = terminus::pickStrike(U'S', 20.0f);
    const terminus::Pick big   = terminus::pickStrike(U'S', 30.0f);
    assert(small.glyph && small.glyph->cellW == 8  && small.glyph->cellH == 16);
    assert(big.glyph   && big.glyph->cellW   == 16 && big.glyph->cellH   == 32);

    // Integer scale only, chosen from the target height.
    assert(small.scale == 1);                                   // < 24 -> 16 px x1
    assert(big.scale == 1);                                     // 30 -> 32 px x1
    assert(terminus::pickStrike(U'S', 48.0f).scale == 2);       // round(1.5)
    assert(terminus::pickStrike(U'S', 100.0f).scale == 3);      // round(3.125)
    assert(terminus::pickStrike(U'S', 1.0f).scale >= 1);        // never 0

    // A glyph that was not baked is reported, not drawn as garbage.
    assert(terminus::pickStrike(U'Q', 30.0f).glyph == nullptr);

    // The runs reproduce the bitmap exactly, at every scale we might use.
    for (int s = 1; s <= 3; ++s) {
        checkExact(*small.glyph, s);
        checkExact(*big.glyph, s);
    }

    printf("terminus_glyph_test: all assertions passed\n");
    return 0;
}

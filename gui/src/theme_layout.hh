#pragma once

#include <cstdint>
#include <utility>
#include <vector>

// How a row of theme previews wraps. Pure arithmetic: the Themes page and
// theme_test both call it, and neither one owns a second copy.
//
// A row fills left to right. The next preview starts a new row when it does
// not fit in what is left. A preview wider than the row still takes the row
// by itself — the caller ellipsizes the name — so a long name never shares
// its row and never gets dropped.

struct ThemeFlowSlot {
    int   index = 0;
    float x = 0;
    float y = 0;
    float w = 0;
};

inline float themeStripWidth(int count, float sq, float gap) {
    if (count <= 0 || sq <= 0.0f) return 0.0f;
    if (count == 1) return sq;
    return (float)count * sq + (float)(count - 1) * gap;
}

inline float themeFlowPack(const float* widths, int n, float maxW, float rowH,
                           float gap, std::vector<ThemeFlowSlot>& out) {
    out.clear();
    if (n <= 0 || !widths || rowH <= 0.0f) return 0.0f;
    if (maxW < 1.0f) maxW = 1.0f;
    if (gap < 0.0f) gap = 0.0f;
    float x = 0.0f;
    float y = 0.0f;
    for (int i = 0; i < n; ++i) {
        float w = widths[i];
        if (w < 0.0f) w = 0.0f;
        if (!out.empty() && x > 0.0f && x + w > maxW) {
            x = 0.0f;
            y += rowH + gap;
        }
        out.push_back(ThemeFlowSlot{ i, x, y, w });
        x += w + gap;
    }
    return y + rowH;
}

// One row of an already-shuffled group. `shown` receives positions into
// `widths`. `plus` is set when a later preview did not fit, and the tail is
// dropped until a `plusW` cell fits after the ones that stay. The first
// preview is kept even when it is wider than the row: the page still has to
// show a color, and the + then sits on the next line.
inline void themeOverviewRow(const float* widths, int n, float maxW, float gap,
                             float plusW, std::vector<int>& shown, bool& plus) {
    shown.clear();
    plus = false;
    if (n <= 0 || !widths) return;
    if (maxW < 1.0f) maxW = 1.0f;
    if (gap < 0.0f) gap = 0.0f;
    if (plusW < 0.0f) plusW = 0.0f;
    float x = 0.0f;
    for (int i = 0; i < n; ++i) {
        float w = widths[i];
        if (w < 0.0f) w = 0.0f;
        const float need = shown.empty() ? w : gap + w;
        if (!shown.empty() && x + need > maxW) break;
        shown.push_back(i);
        x += need;
    }
    if ((int)shown.size() >= n) return;
    plus = true;
    while ((int)shown.size() > 1) {
        float row = 0.0f;
        for (int k = 0; k < (int)shown.size(); ++k)
            row += (k ? gap : 0.0f) + widths[shown[(size_t)k]];
        if (row + gap + plusW <= maxW) break;
        shown.pop_back();
    }
}

// Same seed, same order. The page keeps the seed for as long as Themes is
// open, so Return from a group shows the same previews.
inline void themeSampleShuffle(std::vector<int>& ids, uint32_t seed) {
    uint32_t s = seed ? seed : 1u;
    for (int i = (int)ids.size() - 1; i > 0; --i) {
        s = s * 1664525u + 1013904223u;
        const int j = (int)(s % (uint32_t)(i + 1));
        std::swap(ids[(size_t)i], ids[(size_t)j]);
    }
}

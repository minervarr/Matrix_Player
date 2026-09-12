#include "terminus_glyph.hh"

#include <algorithm>
#include <cmath>

namespace terminus {

namespace {
template <int N>
const Glyph* find(const Glyph (&table)[N], char32_t cp) {
    for (int i = 0; i < N; ++i)
        if (table[i].cp == cp) return &table[i];
    return nullptr;
}
} // namespace

Pick pickStrike(char32_t cp, float targetPx) {
    Pick p;
    if (targetPx < 24.0f) {
        p.glyph = find(kBold16, cp);
        p.scale = 1;
    } else {
        p.glyph = find(kBold32, cp);
        p.scale = std::max(1, (int)std::lround(targetPx / 32.0f));
    }
    return p;
}

void runs(const Glyph& g, int scale, std::vector<Run>& out) {
    out.clear();
    const int s = std::max(1, scale);
    for (int row = 0; row < g.cellH; ++row) {
        const std::uint32_t bits = g.rows[row];
        if (bits == 0) continue;
        int x0 = -1;
        // `bit == cellW` is a sentinel that is always off: it flushes a run
        // that reaches the right edge.
        for (int bit = 0; bit <= g.cellW; ++bit) {
            const bool on = bit < g.cellW && ((bits >> bit) & 1u);
            if (on && x0 < 0) {
                x0 = bit;
            } else if (!on && x0 >= 0) {
                out.push_back({ x0 * s, row * s, (bit - x0) * s, s });
                x0 = -1;
            }
        }
    }
}

bool resolveText(const std::string& text, float targetPx,
                 std::vector<const Glyph*>& out, int& outScale) {
    out.clear();
    out.reserve(text.size());
    outScale = 1;
    for (unsigned char ch : text) {
        const char32_t up = (ch >= 'a' && ch <= 'z') ? (char32_t)(ch - 'a' + 'A')
                                                     : (char32_t)ch;
        const Pick p = pickStrike(up, targetPx);
        if (!p.glyph) return false;
        out.push_back(p.glyph);
        outScale = p.scale;   // identical for every glyph at this targetPx
    }
    return true;
}

} // namespace terminus

float terminusTextWidth(const std::string& text, float targetPx) {
    if (text.empty()) return 0.0f;
    std::vector<const terminus::Glyph*> glyphs;
    int scale = 1;
    if (!terminus::resolveText(text, targetPx, glyphs, scale)) return -1.0f;
    float total = 0.0f;
    for (const terminus::Glyph* g : glyphs) total += (float)(g->cellW * scale);
    return total;
}

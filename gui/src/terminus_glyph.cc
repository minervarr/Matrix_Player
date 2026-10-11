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
    // Always the 16 px strike, integer scale. The 32 px strike has twice the
    // rows and therefore twice the run-length rects for the same on-screen
    // size; OverlayRasterizer silently drops curves past 8192, which is how
    // Settings lists ate Close/Apply. 16 px at x2 is 32 px tall and half the
    // curves. Integer scale is the bitmap rule -- never a fractional one.
    //
    // One step above the strike the type role would have picked. At the
    // 1080 reference the body role is ~22 px, which lands on the 16 px
    // strike, and that read as small next to the serif on the music page.
    // The next legal size is 32. The +1 is that step, applied once, here,
    // so a call site that also asks for "a bit taller" does not take two.
    Pick p;
    p.glyph = find(kBold16, cp);
    int historical = 1;
    if (targetPx >= 24.0f)
        historical = std::max(1, (int)std::lround(targetPx / 16.0f));
    p.scale = historical + 1;
    return p;
}

Origin origin(float x, float y, float targetPx) {
    const Pick p = pickStrike(U'M', targetPx);
    Origin o;
    o.scale = std::max(1, p.scale);
    const int cellH = p.glyph ? p.glyph->cellH * o.scale : 16 * o.scale;
    o.top = std::floor(y + (targetPx - (float)cellH) * 0.5f);
    o.penX = std::floor(x);
    return o;
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

int leftBearing(const Glyph& g) {
    int lo = g.cellW;
    for (int row = 0; row < g.cellH; ++row) {
        const std::uint32_t bits = g.rows[row];
        if (!bits) continue;
        for (int b = 0; b < g.cellW; ++b)
            if ((bits >> b) & 1u) { if (b < lo) lo = b; break; }
    }
    if (lo >= g.cellW) return 0;   // empty (space)
    return lo;
}

int advance(const Glyph& g, int scale) {
    const int s = std::max(1, scale);
    int lo = g.cellW, hi = -1;
    for (int row = 0; row < g.cellH; ++row) {
        const std::uint32_t bits = g.rows[row];
        if (!bits) continue;
        for (int b = 0; b < g.cellW; ++b) {
            if ((bits >> b) & 1u) {
                if (b < lo) lo = b;
                if (b > hi) hi = b;
            }
        }
    }
    if (hi < 0) return g.cellW * s;                 // space: full cell
    return std::max(1, (hi - lo + 1) + 1) * s;      // ink + 1 px bearing
}

bool resolveText(const std::string& text, float targetPx,
                 std::vector<const Glyph*>& out, int& outScale) {
    const std::string folded = terminusFold(text);
    out.clear();
    out.reserve(folded.size());
    outScale = 1;
    for (unsigned char ch : folded) {
        Pick p = pickStrike((char32_t)ch, targetPx);
        if (!p.glyph) p = pickStrike(U'?', targetPx);
        if (!p.glyph) return false;
        out.push_back(p.glyph);
        outScale = p.scale;   // identical for every glyph at this targetPx
    }
    return true;
}

} // namespace terminus

float terminusDrawnHeight(float targetPx) {
    const terminus::Pick p = terminus::pickStrike(U'M', targetPx);
    if (!p.glyph) return targetPx;
    return (float)(p.glyph->cellH * p.scale);
}

TerminusInk terminusInk(const std::string& text, float targetPx) {
    std::vector<const terminus::Glyph*> glyphs;
    int scale = 1;
    TerminusInk ink;
    if (!terminus::resolveText(text, targetPx, glyphs, scale) || glyphs.empty())
        return ink;
    int lo = 0x7fffffff, hi = -1;
    for (const terminus::Glyph* g : glyphs) {
        for (int row = 0; row < g->cellH; ++row) {
            if (!g->rows[row]) continue;
            if (row < lo) lo = row;
            if (row > hi) hi = row;
        }
    }
    if (hi < 0) return ink;
    const float s = (float)scale;
    ink.top = (float)lo * s;
    ink.bottom = (float)(hi + 1) * s;
    return ink;
}

float terminusTallerTarget(float targetPx) {
    // The target that pickStrike draws one strike above `targetPx`.
    // pickStrike already adds one step, so handing it the height it just
    // drew asks for exactly one more (32 -> 48, 48 -> 64). Returning
    // drawn+8 used to do that when the bands started at 16; after the
    // step it would skip two.
    const int scale = terminus::pickStrike(U'M', targetPx).scale;
    return (float)(scale * 16);
}

namespace {
char32_t nextCp(const std::string& s, size_t& i) {
    if (i >= s.size()) return 0;
    const unsigned char b0 = (unsigned char)s[i];
    auto cont = [&](size_t n) -> char32_t {
        if (i + n > s.size()) { ++i; return 0xFFFD; }
        for (size_t k = 1; k < n; ++k) {
            const unsigned char c = (unsigned char)s[i + k];
            if ((c & 0xC0) != 0x80) { ++i; return 0xFFFD; }
        }
        char32_t cp = 0;
        if (n == 2) cp = ((b0 & 0x1F) << 6) | (s[i + 1] & 0x3F);
        else if (n == 3) cp = ((b0 & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F);
        else cp = ((b0 & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) |
                  ((s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F);
        i += n;
        return cp;
    };
    if (b0 < 0x80) { ++i; return b0; }
    if ((b0 & 0xE0) == 0xC0) return cont(2);
    if ((b0 & 0xF0) == 0xE0) return cont(3);
    if ((b0 & 0xF8) == 0xF0) return cont(4);
    ++i;
    return 0xFFFD;
}
} // namespace

char terminusFoldCp(char32_t cp) {
    if (cp >= U'a' && cp <= U'z') return (char)(cp - U'a' + U'A');
    if (cp == 0x2013 || cp == 0x2014) return '-';
    if (cp == 0x00B7 || cp == 0x2022 || cp == 0x2219) return '/';
    if (cp >= 0x20 && cp <= 0x7E) return (char)cp;
    return '?';
}

std::string terminusFold(const std::string& utf8) {
    std::string out;
    out.reserve(utf8.size());
    size_t i = 0;
    while (i < utf8.size())
        out.push_back(terminusFoldCp(nextCp(utf8, i)));
    return out;
}

float terminusTextWidth(const std::string& text, float targetPx) {
    if (text.empty()) return 0.0f;
    std::vector<const terminus::Glyph*> glyphs;
    int scale = 1;
    if (!terminus::resolveText(text, targetPx, glyphs, scale)) return 0.0f;
    float total = 0.0f;
    for (const terminus::Glyph* g : glyphs) total += (float)terminus::advance(*g, scale);
    return total;
}

std::string terminusEllipsize(const std::string& text, float targetPx, float maxW) {
    const std::string s = terminusFold(text);
    if (terminusTextWidth(s, targetPx) <= maxW) return s;
    const std::string dots = "...";
    if (terminusTextWidth(dots, targetPx) > maxW) return dots;
    std::string prefix = s;
    while (!prefix.empty() && terminusTextWidth(prefix + dots, targetPx) > maxW)
        prefix.pop_back();
    return prefix + dots;
}

std::vector<std::string> terminusWrap(const std::string& text, float targetPx, float maxW) {
    const std::string s = terminusFold(text);
    std::vector<std::string> out;
    if (s.empty()) return out;

    auto fits = [&](const std::string& t) {
        return terminusTextWidth(t, targetPx) <= maxW + 1e-4f;
    };
    auto takePrefix = [&](const std::string& word) -> size_t {
        size_t n = 0;
        while (n < word.size() && fits(word.substr(0, n + 1))) ++n;
        return std::max<size_t>(n, 1);
    };

    std::vector<std::string> words;
    std::string w;
    for (char ch : s) {
        if (ch == ' ') {
            if (!w.empty()) { words.push_back(w); w.clear(); }
        } else {
            w.push_back(ch);
        }
    }
    if (!w.empty()) words.push_back(w);

    std::string line;
    auto flush = [&]() {
        if (!line.empty()) { out.push_back(line); line.clear(); }
    };
    for (const std::string& word : words) {
        if (!fits(word)) {
            flush();
            size_t p = 0;
            while (p < word.size()) {
                const size_t n = takePrefix(word.substr(p));
                if (p + n < word.size()) {
                    out.push_back(word.substr(p, n));
                    p += n;
                } else {
                    line = word.substr(p, n);
                    p += n;
                }
            }
            continue;
        }
        if (line.empty()) { line = word; continue; }
        if (fits(line + " " + word)) {
            line += ' ';
            line += word;
        } else {
            out.push_back(line);
            line = word;
        }
    }
    flush();
    return out;
}

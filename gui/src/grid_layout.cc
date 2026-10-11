#include "grid_layout.hh"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace grid {

Shape computeShape(int usableW, int usableH, int tilePitch, int artMargin,
                   int textBand, int minArt) {
    Shape s;
    if (usableW <= 0 || usableH <= 0 || tilePitch <= 0) {
        s.cellW = std::max(1, usableW);
        s.pitch = std::max(1, usableH);
        s.art   = std::max(1, minArt);
        return s;
    }

    // NEAREST, not floor. See the header: floor turned 2.69 columns into 2.
    int cols = std::clamp((usableW + tilePitch / 2) / tilePitch, 2, 8);
    while (cols > 1 && usableW / cols - artMargin < minArt) cols--;
    const int cellW = usableW / cols;
    const int artW  = std::max(minArt, cellW - artMargin);

    // Rows at their natural height, rounded to the nearest whole count; then
    // the pitch is whatever makes that many rows fill the height exactly.
    const int natural = artW + artMargin + textBand;
    int rows  = std::max(1, (usableH + natural / 2) / natural);
    int pitch = usableH / rows;
    int art   = std::min(artW, pitch - artMargin - textBand);
    // Rounding UP a row can squeeze the art below legibility on a short
    // viewport; give that row back rather than draw a thumbnail.
    while (art < minArt && rows > 1) {
        rows--;
        pitch = usableH / rows;
        art   = std::min(artW, pitch - artMargin - textBand);
    }

    s.cols       = cols;
    s.rows       = rows;
    s.cellW      = cellW;
    s.art        = std::max(1, art);
    s.pitch      = std::max(1, pitch);
    s.artOffsetY = std::max(0, (s.pitch - s.art - textBand) / 2);
    return s;
}

bool scrollRows(RowScroll& s, float dpx, int pitch, int maxRow) {
    if (pitch <= 0) return false;
    maxRow = std::max(0, maxRow);
    const int d = dpx > 0.0f ? 1 : (dpx < 0.0f ? -1 : 0);
    if (d == 0) return false;

    const float P  = (float)pitch;
    const float th = kRowSnapThreshold * P;

    // A reversal (or the first movement) re-anchors to the row on screen, so
    // the threshold is measured from here -- see the header for why.
    if (d != s.dir) {
        s.free = (float)s.row * P;
        s.dir  = d;
    }
    s.free = std::clamp(s.free + dpx, 0.0f, (float)maxRow * P);

    // Rows change at row*P + th going forward, row*P - th coming back.
    int target = d > 0 ? (int)std::floor((s.free - th) / P) + 1
                       : (int)std::ceil ((s.free + th) / P) - 1;
    target = std::clamp(target, 0, maxRow);
    // Never let a forward move step backward, or the reverse.
    target = d > 0 ? std::max(target, s.row) : std::min(target, s.row);

    const bool changed = target != s.row;
    s.row = target;
    return changed;
}

namespace {

std::string foldArtist(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    std::string o;
    o.reserve(e - b);
    for (size_t i = b; i < e; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
        o.push_back((char)c);
    }
    return o;
}

} // namespace

void orderByArtist(std::vector<int>& indices, const std::vector<ReleaseKey>& keys) {
    std::unordered_map<std::string, int> count;
    count.reserve(indices.size());
    for (int i : indices) {
        if (i < 0 || (size_t)i >= keys.size()) continue;
        count[foldArtist(keys[(size_t)i].artist)]++;
    }

    std::stable_sort(indices.begin(), indices.end(), [&](int a, int b) {
        const bool aOk = a >= 0 && (size_t)a < keys.size();
        const bool bOk = b >= 0 && (size_t)b < keys.size();
        const std::string aa = aOk ? foldArtist(keys[(size_t)a].artist) : std::string();
        const std::string bb = bOk ? foldArtist(keys[(size_t)b].artist) : std::string();
        const auto ia = count.find(aa);
        const auto ib = count.find(bb);
        const int ca = ia == count.end() ? 0 : ia->second;
        const int cb = ib == count.end() ? 0 : ib->second;
        if (ca != cb) return ca > cb;
        if (aa != bb) return aa < bb;
        const int ya = aOk ? keys[(size_t)a].year : 0;
        const int yb = bOk ? keys[(size_t)b].year : 0;
        const bool ua = ya <= 0, ub = yb <= 0;
        if (ua != ub) return !ua;
        if (ya != yb) return ya < yb;
        return false;
    });
}

} // namespace grid

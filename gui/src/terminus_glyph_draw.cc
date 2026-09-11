#include "terminus_glyph.hh"

#include <cmath>

#include "canvas.hh"

bool drawTerminusGlyph(Canvas& c, const LayoutRect& rc, char32_t cp, float targetPx,
                       const Color& col) {
    const terminus::Pick p = terminus::pickStrike(cp, targetPx);
    if (!p.glyph) return false;
    const float w  = (float)(p.glyph->cellW * p.scale);
    const float h  = (float)(p.glyph->cellH * p.scale);
    const float ox = std::floor(rc.left + ((rc.right - rc.left) - w) * 0.5f);
    const float oy = std::floor(rc.top  + ((rc.bottom - rc.top) - h) * 0.5f);
    std::vector<terminus::Run> rs;
    terminus::runs(*p.glyph, p.scale, rs);
    for (const terminus::Run& r : rs)
        c.rect(ox + (float)r.x, oy + (float)r.y, (float)r.w, (float)r.h, col);
    return true;
}

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

bool drawTerminusText(Canvas& c, const std::string& text, float x, float y,
                      float targetPx, const Color& col) {
    std::vector<const terminus::Glyph*> glyphs;
    int scale = 1;
    if (!terminus::resolveText(text, targetPx, glyphs, scale)) return false;
    if (glyphs.empty()) return true;   // nothing to draw; not a failure
    // origin() is the grid the settings radio and switch also hang from.
    // Flooring a second time here would let the two drift apart. The scale
    // has one source (pickStrike); a mismatch means the glyphs and the grid
    // were chosen independently, and drawing would smear one of them.
    const terminus::Origin o = terminus::origin(x, y, targetPx);
    if (scale != o.scale) return false;
    const float oy = o.top;
    float pen = o.penX;
    std::vector<terminus::Run> rs;
    for (const terminus::Glyph* g : glyphs) {
        terminus::runs(*g, o.scale, rs);
        const float ox = pen - (float)(terminus::leftBearing(*g) * o.scale);
        for (const terminus::Run& r : rs)
            c.rect(ox + (float)r.x, oy + (float)r.y, (float)r.w, (float)r.h, col);
        pen += (float)terminus::advance(*g, o.scale);
    }
    return true;
}

void drawTerminusWrapped(Canvas& c, const std::string& text, float x, float y,
                         float targetPx, float maxW, float lineH, const Color& col) {
    const std::vector<std::string> lines = terminusWrap(text, targetPx, maxW);
    for (size_t i = 0; i < lines.size(); ++i)
        drawTerminusText(c, lines[i], x, y + (float)i * lineH, targetPx, col);
}

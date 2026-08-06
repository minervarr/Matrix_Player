// The Canvas-dependent half of ui_icons.hh. Split out so the placement math in
// ui_icons.cc stays linkable from a pure-logic test.
#include "ui_icons.hh"

#include <string_view>

#include "canvas.hh"
#include "msdf.hh"

namespace {
// Minimal UTF-8 encoder — the icon codepoints are BMP Private Use Area (3
// bytes today), but this stays correct if they ever move to a
// supplementary-plane PUA.
int encodeUtf8(unsigned cp, char out[4]) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}
}  // namespace

bool drawUiIconGlyph(Canvas& c, const LayoutRect& rc, UiIcon icon, const Color& col) {
    // The Canvas already carries the atlas font, so this needs no extra
    // plumbing through the call sites.
    const TextFont* font = c.msdfFont();
    if (!font) return false;

    const unsigned cp = uiIconCodepoint(icon);
    if (cp == 0 || !font->hasCodepoint(cp)) return false;

    const IconPlacement p = uiIconPlacement(rc, icon);
    if (p.sizePx <= 0.0f) return false;

    char utf8[4];
    const int n = encodeUtf8(cp, utf8);
    c.text(std::string_view(utf8, (size_t)n), p.x, p.y, p.sizePx, col);
    return true;
}

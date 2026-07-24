#include "settings_panels.hh"
#include "theme.hh"
#include "canvas.hh"
#include "msdf.hh"

namespace {
Rect toRect(const LayoutRect& r) {
    return { (float)r.left, (float)r.top, (float)(r.right - r.left), (float)(r.bottom - r.top) };
}
Color toColor(ColorRef c, float a = 1.0f) {
    return { GetRValue(c) / 255.0f, GetGValue(c) / 255.0f, GetBValue(c) / 255.0f, a };
}
} // namespace

namespace panels {

LayoutRect rowRect(const LayoutRect& area, int index, int rowH, int scrollY) {
    int top = area.top + index * rowH - scrollY;
    return { area.left, top, area.right, top + rowH };
}

int hitTestRows(const LayoutRect& area, int rowH, int scrollY, int rowCount, int x, int y) {
    if (x < area.left || x >= area.right || y < area.top || y >= area.bottom) return -1;
    int rel = y - area.top + scrollY;
    if (rel < 0) return -1;
    int row = rel / rowH;
    if (row < 0 || row >= rowCount) return -1;
    return row;
}

void drawRowList(Canvas& canvas, const LayoutRect& area,
                  const std::vector<std::string>& labels,
                  int rowH, int scrollY, int hoverIdx, int selectedIdx,
                  float textSize, float uiScale) {
    canvas.setClip(area.left, area.top, area.right - area.left, area.bottom - area.top);
    float pad = 14.0f * uiScale;
    for (int i = 0; i < (int)labels.size(); ++i) {
        LayoutRect rc = rowRect(area, i, rowH, scrollY);
        if (rc.bottom < area.top || rc.top > area.bottom) continue;
        Rect r = toRect(rc);
        if (i == hoverIdx)
            canvas.rect(r.x, r.y, r.w, r.h, toColor(CLR_HOVER));
        bool sel = (i == selectedIdx);
        ColorRef borderClr = sel ? CLR_ACCENT : CLR_SEPARATOR;
        float borderThick = sel ? 2.0f : 1.0f;
        canvas.rect(r.x, r.y + r.h - borderThick, r.w, borderThick, toColor(borderClr));
        ColorRef textClr = sel ? CLR_ACCENT : CLR_TEXT_PRIMARY;
        canvas.textStyled(labels[i], r.x + pad, r.y + r.h * 0.5f - textSize * 0.5f,
                          textSize, toColor(textClr), FontStyle::Roman);
    }
    canvas.clearClip();
}

void drawButton(Canvas& canvas, const LayoutRect& rc, const std::string& label,
                 bool hover, float textSize, bool primary) {
    Rect r = toRect(rc);
    ColorRef borderClr = primary ? CLR_ACCENT : CLR_SEPARATOR;
    if (hover)
        canvas.rect(r.x, r.y, r.w, r.h, toColor(CLR_HOVER), 6.0f);
    canvas.rect(r.x, r.y, r.w, 1.0f, toColor(borderClr));
    canvas.rect(r.x, r.y + r.h - 1.0f, r.w, 1.0f, toColor(borderClr));
    canvas.rect(r.x, r.y, 1.0f, r.h, toColor(borderClr));
    canvas.rect(r.x + r.w - 1.0f, r.y, 1.0f, r.h, toColor(borderClr));
    ColorRef textClr = primary ? CLR_ACCENT : CLR_TEXT_PRIMARY;
    float w = canvas.textWidthStyled(label, textSize, FontStyle::Roman);
    canvas.textStyled(label, r.x + (r.w - w) * 0.5f, r.y + r.h * 0.5f - textSize * 0.5f,
                      textSize, toColor(textClr), FontStyle::Roman);
}

LayoutRect drawHeader(Canvas& canvas, const LayoutRect& area, const std::string& title,
                      float uiScale, float headerTextSize, LayoutRect& closeRc) {
    Rect a = toRect(area);
    canvas.rect(a.x, a.y, a.w, a.h, toColor(CLR_BG_MAIN));

    float headerH = 56.0f * uiScale;
    canvas.textStyled(title, a.x + 24.0f * uiScale, a.y + headerH * 0.5f - headerTextSize * 0.5f,
                      headerTextSize, toColor(CLR_TEXT_PRIMARY), FontStyle::Bold);
    canvas.rect(a.x, a.y + headerH, a.w, 1.0f, toColor(CLR_SEPARATOR));

    float closeW = 90.0f * uiScale, closeH = 32.0f * uiScale;
    closeRc = { (int)(area.right - closeW - 20.0f * uiScale), (int)(area.top + (headerH - closeH) * 0.5f),
                (int)(area.right - 20.0f * uiScale), (int)(area.top + (headerH + closeH) * 0.5f) };

    return { area.left, (int)(area.top + headerH), area.right, area.bottom };
}

} // namespace panels

#include "settings_panels.hh"
#include "theme.hh"
#include "canvas.hh"
#include "widgets.hh"
#include "msdf.hh"

#include <algorithm>

namespace {
Rect toRect(const LayoutRect& r) {
    return { (float)r.left, (float)r.top, (float)(r.right - r.left), (float)(r.bottom - r.top) };
}
Color toColor(ColorRef c, float a = 1.0f) {
    return { GetRValue(c) / 255.0f, GetGValue(c) / 255.0f, GetBValue(c) / 255.0f, a };
}
// A theme color lifted toward white by `amt` (0-255 per channel) — used to
// synthesize hover/elevated button fills from the base palette.
Color lift(ColorRef c, int amt) {
    auto cl = [](int v) { return std::clamp(v, 0, 255); };
    return { cl(GetRValue(c) + amt) / 255.0f,
             cl(GetGValue(c) + amt) / 255.0f,
             cl(GetBValue(c) + amt) / 255.0f, 1.0f };
}
} // namespace

namespace panels {

void drawButton(Canvas& canvas, const LayoutRect& rc, const std::string& label,
                 bool hover, float textSize, bool primary) {
    (void)textSize;   // drawFitButton sizes the label to the button proportionally
    Rect r = toRect(rc);
    float radius = UI_CORNER_RADIUS;   // uniform rounding — reads as a real button
    Color bg, fg;
    if (primary) {
        // High-emphasis action: solid accent fill, dark label for contrast.
        bg = hover ? lift(CLR_ACCENT, 28) : toColor(CLR_ACCENT);
        fg = toColor(CLR_BG_MAIN);
    } else {
        // Secondary action: subtle elevated fill above the page background.
        bg = lift(CLR_BG_MAIN, hover ? 56 : 34);
        fg = toColor(CLR_TEXT_PRIMARY);
    }
    // Single line: shrink-then-ellipsis rather than wrapping a button label.
    widgets::drawFitButton(canvas, r, label, bg, fg, radius, widgets::kTextFit, false);
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

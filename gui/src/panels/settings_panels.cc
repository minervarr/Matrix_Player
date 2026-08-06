#include "settings_panels.hh"
#include "theme.hh"
#include "canvas.hh"
#include "widgets.hh"
#include "msdf.hh"

#include <algorithm>
#include <cmath>

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
                      float scale, float headerTextSize, LayoutRect& closeRc) {
    Rect a = toRect(area);
    canvas.rect(a.x, a.y, a.w, a.h, toColor(CLR_BG_MAIN));

    // Values authored at the 1080 reference height (see gui/src/ui_metrics.hh);
    // `scale` is UiMetrics::scale, 1.0 there.
    float headerH = 91.0f * scale;
    canvas.textStyled(title, a.x + 39.0f * scale, a.y + headerH * 0.5f - headerTextSize * 0.5f,
                      headerTextSize, toColor(CLR_TEXT_PRIMARY), FontStyle::Bold);
    canvas.rect(a.x, a.y + headerH, a.w, std::max(1.0f, std::round(scale)),
                toColor(CLR_SEPARATOR));

    float closeW = 147.0f * scale, closeH = 52.0f * scale;
    float closeMargin = 32.0f * scale;
    closeRc = { (int)(area.right - closeW - closeMargin), (int)(area.top + (headerH - closeH) * 0.5f),
                (int)(area.right - closeMargin),          (int)(area.top + (headerH + closeH) * 0.5f) };

    return { area.left, (int)(area.top + headerH), area.right, area.bottom };
}

void drawScrollbar(Canvas& canvas, const LayoutRect& listArea,
                   int contentH, int scrollY, float scale) {
    Rect a = toRect(listArea);
    if (a.h <= 0.0f || contentH <= (int)a.h) return;   // everything already visible

    float barW = 9.0f * scale;
    float x    = a.x + a.w - barW - SP_XS * scale;

    canvas.rect(x, a.y, barW, a.h, toColor(CLR_SEPARATOR));

    // Thumb length is the visible fraction of the content, floored so it stays
    // grabbable-looking on very long lists.
    float thumbH = std::max(a.h * (a.h / (float)contentH), 39.0f * scale);
    float maxScroll = (float)contentH - a.h;
    float t = std::clamp((float)scrollY / maxScroll, 0.0f, 1.0f);

    // Chrome, not state — CLR_ACCENT stays reserved for selection/state
    // (docs/UI_DESIGN_SYSTEM.md principle #4).
    canvas.rect(x, a.y + t * (a.h - thumbH), barW, thumbH, toColor(CLR_TEXT_SECONDARY));
}

std::vector<LayoutRect> layoutButtonRow(const LayoutRect& content, float pad,
                                        int count, float idealBtnW, float gap,
                                        float minBtnW, int by, int height,
                                        bool alignRight) {
    std::vector<LayoutRect> out(std::max(0, count));
    if (count <= 0) return out;
    float x0 = content.left + pad, x1 = content.right - pad;
    float avail = std::max(0.0f, x1 - x0);

    float btnW = idealBtnW, useGap = gap;
    float total = count * btnW + (count - 1) * useGap;
    if (total > avail) {
        btnW = std::max(minBtnW, (avail - (count - 1) * useGap) / count);
        total = count * btnW + (count - 1) * useGap;
        if (total > avail) {
            useGap = (count > 1) ? std::max(0.0f, (avail - count * minBtnW) / (count - 1)) : 0.0f;
            btnW = std::max(0.0f, (avail - (count - 1) * useGap) / count);
        }
    }

    for (int slot = 0; slot < count; slot++) {
        float left, right;
        if (alignRight) { right = x1 - slot * (btnW + useGap); left = right - btnW; }
        else            { left  = x0 + slot * (btnW + useGap); right = left + btnW; }
        out[(size_t)slot] = { (int)left, by, (int)right, by + height };
    }
    return out;
}

std::pair<LayoutRect, LayoutRect> layoutEdgePair(
    const LayoutRect& content, float pad,
    float leftIdealW, float rightIdealW, float minBtnW, float minGap,
    int by, int height) {
    float x0 = content.left + pad, x1 = content.right - pad;
    float avail = std::max(0.0f, x1 - x0);
    float leftW = leftIdealW, rightW = rightIdealW;

    if (leftW + rightW + minGap > avail) {
        float idealSum = leftIdealW + rightIdealW;
        float scale = idealSum > 0.0f ? (avail - minGap) / idealSum : 0.0f;
        leftW  = std::max(minBtnW, leftIdealW  * scale);
        rightW = std::max(minBtnW, rightIdealW * scale);
        if (leftW + rightW + minGap > avail)
            leftW = rightW = std::max(0.0f, (avail - minGap) * 0.5f);
    }

    LayoutRect leftRc  = { (int)x0, by, (int)(x0 + leftW), by + height };
    LayoutRect rightRc = { (int)(x1 - rightW), by, (int)x1, by + height };
    return { leftRc, rightRc };
}

} // namespace panels

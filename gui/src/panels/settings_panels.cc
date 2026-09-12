#include "settings_panels.hh"
#include "theme.hh"
#include "canvas.hh"
#include "widgets.hh"
#include "msdf.hh"
#include "terminus_glyph.hh"

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
                 bool hover, float textSize, bool primary, bool terminusChrome) {
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

    if (terminusChrome) {
        // Terminus is monospace: no shrink-to-fit. Ellipsize at a cell if the
        // label is wider than the button -- never fall back to Computer Modern.
        const float s    = r.h * 0.34f;
        const float maxW = std::max(0.0f, r.w - r.h * 0.35f);
        const std::string shown = terminusEllipsize(label, s, maxW);
        const float tw = terminusTextWidth(shown, s);
        canvas.rect(r.x, r.y, r.w, r.h, bg, radius);
        if (tw > 0.0f)
            drawTerminusText(canvas, shown, r.x + (r.w - tw) * 0.5f,
                             r.y + (r.h - s) * 0.5f, s, fg);
        return;
    }
    // Single line: shrink-then-ellipsis rather than wrapping a button label.
    widgets::drawFitButton(canvas, r, label, bg, fg, radius, widgets::kTextFit, false);
}

LayoutRect drawHeader(Canvas& canvas, const LayoutRect& area, const std::string& title,
                      float scale, float headerTextSize, LayoutRect& closeRc,
                      bool terminusChrome) {
    Rect a = toRect(area);
    canvas.rect(a.x, a.y, a.w, a.h, toColor(CLR_BG_MAIN));

    // Values authored at the 1080 reference height (see gui/src/ui_metrics.hh);
    // `scale` is UiMetrics::scale, 1.0 there.
    float headerH = 91.0f * scale;
    float closeW = 147.0f * scale, closeH = 52.0f * scale;
    float closeMargin = 32.0f * scale;
    const float tx = a.x + 39.0f * scale, ty = a.y + headerH * 0.5f - headerTextSize * 0.5f;
    const Color primaryCol = toColor(CLR_TEXT_PRIMARY);
    if (terminusChrome) {
        const float maxW = std::max(0.0f, a.w - 39.0f * scale - closeW - closeMargin - 8.0f * scale);
        const std::string shown = terminusEllipsize(title, headerTextSize, maxW);
        drawTerminusText(canvas, shown, tx, ty, headerTextSize, primaryCol);
    } else {
        canvas.textStyled(title, tx, ty, headerTextSize, primaryCol, FontStyle::Bold);
    }
    canvas.rect(a.x, a.y + headerH, a.w, std::max(1.0f, std::round(scale)),
                toColor(CLR_SEPARATOR));

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

LayoutRect drawTerminusRadioRow(Canvas& canvas, const LayoutRect& row,
                                bool selected, bool hovered, const std::string& label,
                                float textSize, const Color& dotOn, const Color& dotOff,
                                const Color& textOn, const Color& textOff,
                                const Color& hoverBg, const Color& selBg, const Color& selBar) {
    Rect r = toRect(row);
    const float dotD = r.h * 0.40f;
    const float pad  = r.h * 0.34f;
    const float gap  = r.h * 0.32f;
    Rect dot{ r.x + pad, r.y + (r.h - dotD) * 0.5f, dotD, dotD };
    const float labelX = dot.x + dot.w + gap;
    const float maxW = std::max(0.0f, r.x + r.w - pad - labelX);
    const std::string shown = terminusEllipsize(label, textSize, maxW);
    const float tw = std::max(0.0f, terminusTextWidth(shown, textSize));

    Rect hit{ r.x, r.y, (labelX + tw + pad) - r.x, r.h };
    const float rad = 0.0f;   // UI_CORNER_RADIUS -- Settings is square
    if (selected && selBg.a > 0.0f)
        canvas.rect(hit.x, hit.y, hit.w, hit.h, selBg, rad);
    else if (hovered)
        canvas.rect(hit.x, hit.y, hit.w, hit.h, hoverBg, rad);
    if (selected && selBar.a > 0.0f)
        canvas.rect(hit.x, hit.y, 3.0f, hit.h, selBar, rad);

    canvas.rect(dot.x, dot.y, dot.w, dot.h, selected ? dotOn : dotOff, dot.w * 0.5f);
    drawTerminusText(canvas, shown, labelX, r.y + (r.h - textSize) * 0.5f, textSize,
                     selected ? textOn : textOff);
    return { (int)hit.x, (int)hit.y, (int)(hit.x + hit.w), (int)(hit.y + hit.h) };
}

std::vector<TerminusListRow> drawTerminusScrollList(
    Canvas& canvas, const LayoutRect& area, const std::vector<std::string>& items,
    int selected, float scrollPx, float rowH, int hoverIndex, float textSize,
    const Color& rowText, const Color& hoverBg, const Color& pillColor,
    const Color& pillText, const Color& selectedBar) {
    Rect a = toRect(area);
    canvas.rect(a.x, a.y, a.w, a.h, toColor(CLR_BG_MAIN));
    const auto saved = canvas.saveClip();
    canvas.setClip(a.x, a.y, a.w, a.h);
    const float inset = canvas.pad();
    std::vector<TerminusListRow> visible;
    for (int i = 0; i < (int)items.size(); i++) {
        const float ry = a.y + (float)i * rowH - scrollPx;
        if (ry + rowH < a.y || ry > a.y + a.h) continue;
        Rect r{ a.x, ry, a.w, rowH };
        const float textX = r.x + inset;
        const std::string shown = terminusEllipsize(items[(size_t)i], textSize,
                                                    std::max(0.0f, r.w - inset * 2.0f));
        const bool sel = (i == selected);
        const float rad = 0.0f;
        if (sel)
            canvas.rect(r.x + inset * 0.3f, r.y, r.w - inset * 0.6f, rowH, pillColor, rad);
        else if (i == hoverIndex)
            canvas.rect(r.x + inset * 0.3f, r.y, r.w - inset * 0.6f, rowH, hoverBg, rad);
        if (sel && selectedBar.a > 0.0f)
            canvas.rect(r.x + inset * 0.3f, r.y, 3.0f, rowH, selectedBar, rad);
        drawTerminusText(canvas, shown, textX, r.y + (rowH - textSize) * 0.5f, textSize,
                         sel ? pillText : rowText);
        visible.push_back({ { (int)r.x, (int)r.y, (int)(r.x + r.w), (int)(r.y + r.h) }, i });
    }
    canvas.restoreClip(saved);
    return visible;
}

void drawTerminusToggle(Canvas& canvas, const LayoutRect& row, bool on,
                        const std::string& label, float textSize,
                        const Color& onColor, const Color& offColor, const Color& knobColor,
                        const Color& labelColor) {
    Rect r = toRect(row);
    const float h = r.h * 0.66f;
    const float w = h * 1.8f;
    Rect sw{ r.x + r.w - w, r.y + (r.h - h) * 0.5f, w, h };
    const float maxW = std::max(0.0f, sw.x - r.x - r.h * 0.34f);
    const std::string shown = terminusEllipsize(label, textSize, maxW);
    drawTerminusText(canvas, shown, r.x, r.y + (r.h - textSize) * 0.5f, textSize, labelColor);
    canvas.rect(sw.x, sw.y, sw.w, sw.h, on ? onColor : offColor, sw.h * 0.5f);
    const float knob = sw.h * 0.82f;
    const float ky = sw.y + (sw.h - knob) * 0.5f;
    const float kx = on ? (sw.x + sw.w - knob - (sw.h - knob) * 0.5f)
                        : (sw.x + (sw.h - knob) * 0.5f);
    canvas.rect(kx, ky, knob, knob, knobColor, knob * 0.5f);
}

void drawTerminusSearchField(Canvas& canvas, const LayoutRect& rc,
                             const std::string& text, bool focused,
                             const char* placeholder, float textSize) {
    Rect s = toRect(rc);
    canvas.rect(s.x, s.y, s.w, s.h, toColor(CLR_INPUT_BG), UI_CORNER_RADIUS);
    canvas.rect(s.x, s.y + s.h - 1, s.w, 1, toColor(focused ? CLR_ACCENT : CLR_SEPARATOR));
    const bool empty = text.empty() && !focused;
    std::string shown = empty ? std::string(placeholder ? placeholder : "") : text;
    if (focused) shown += "|";
    const float maxW = std::max(0.0f, s.w - 16.0f - 8.0f);
    shown = terminusEllipsize(shown, textSize, maxW);
    drawTerminusText(canvas, shown, s.x + 8.0f, s.y + s.h * 0.5f - textSize * 0.5f, textSize,
                     toColor(empty ? CLR_TEXT_DIM : CLR_TEXT_PRIMARY));
}

float drawTerminusLabel(Canvas& canvas, const std::string& text,
                        float x, float y, float targetPx, float maxW, float lineH,
                        const Color& col) {
    const std::vector<std::string> lines = terminusWrap(text, targetPx, maxW);
    drawTerminusWrapped(canvas, text, x, y, targetPx, maxW, lineH, col);
    if (lines.empty()) return y;
    return y + (float)lines.size() * lineH;
}

} // namespace panels

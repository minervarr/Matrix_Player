#include "settings_panels.hh"
#include "theme.hh"
#include "canvas.hh"
#include "widgets.hh"
#include "msdf.hh"
#include "terminus_glyph.hh"
#include "pixel_marks.hh"

#include <algorithm>
#include <cmath>

namespace {
Rect toRect(const LayoutRect& r) {
    return { (float)r.left, (float)r.top, (float)(r.right - r.left), (float)(r.bottom - r.top) };
}
Color toColor(ColorRef c, float a = 1.0f) {
    return { GetRValue(c) / 255.0f, GetGValue(c) / 255.0f, GetBValue(c) / 255.0f, a };
}

void paintPx(Canvas& canvas, float x, float y, const panels::PxMark& m, int scale,
             const Color& col) {
    std::vector<panels::PxRun> rs;
    panels::pxRuns(m, scale, rs);
    for (const panels::PxRun& r : rs)
        canvas.rect(x + (float)r.x, y + (float)r.y, (float)r.w, (float)r.h, col);
}
} // namespace

namespace panels {

void drawButton(Canvas& canvas, const LayoutRect& rc, const std::string& label,
                 bool hover, float textSize, bool primary, bool terminusChrome) {
    Rect r = toRect(rc);
    float radius = UI_CORNER_RADIUS;   // uniform rounding — reads as a real button
    Color bg, fg;
    if (primary) {
        // The one filled accent: the commit. The label is the page color,
        // which paletteAcceptable keeps readable on the accent.
        bg = toColor(themeButtonFill(true, hover));
        fg = toColor(CLR_BG_MAIN);
    } else {
        bg = toColor(themeButtonFill(false, hover));
        fg = toColor(CLR_TEXT_PRIMARY);
    }

    if (terminusChrome) {
        // Terminus lands on a strike. The caller's text size is that strike's
        // target; a fraction of the button height is how Exit stayed on the
        // 16 px step after the body buttons had already stepped to 32.
        const float s = textSize > 0.0f ? textSize : r.h * 0.46f;
        const float drawn = terminusDrawnHeight(s);
        const float maxW = std::max(0.0f, r.w - std::max(r.h * 0.35f, drawn * 0.70f));
        const std::string shown = terminusEllipsize(label, s, maxW);
        const float tw = terminusTextWidth(shown, s);
        canvas.rect(r.x, r.y, r.w, r.h, bg, radius);
        if (tw > 0.0f)
            drawTerminusText(canvas, shown, r.x + (r.w - tw) * 0.5f,
                             r.y + (r.h - s) * 0.5f, s, fg);
        return;
    }
    (void)textSize;   // the serif path sizes the label to the button itself
    // Single line: shrink-then-ellipsis rather than wrapping a button label.
    widgets::drawFitButton(canvas, r, label, bg, fg, radius, widgets::kTextFit, false);
}

LayoutRect drawHeader(Canvas& canvas, const LayoutRect& area, const std::string& title,
                      float scale, float headerTextSize, LayoutRect& closeRc,
                      bool terminusChrome, bool closeHover, const char* actionLabel,
                      float actionTextSize) {
    Rect a = toRect(area);
    canvas.rect(a.x, a.y, a.w, a.h, toColor(CLR_BG_MAIN));

    // Values authored at the 1080 reference height (see gui/src/ui_metrics.hh);
    // `scale` is UiMetrics::scale, 1.0 there. The Terminus button is sized
    // from the label's strike first, so the box follows the glyph instead of
    // the glyph being a fraction of a box that was authored for another face.
    const char* lab = (actionLabel && actionLabel[0]) ? actionLabel : "Return";
    float headerH = 91.0f * scale;
    float closeW = 147.0f * scale, closeH = 52.0f * scale;
    float closeMargin = 32.0f * scale;
    float labelPx = 0.0f;
    if (terminusChrome) {
        labelPx = actionTextSize > 0.0f ? actionTextSize : headerTextSize;
        const float drawn = terminusDrawnHeight(labelPx);
        const float tw = terminusTextWidth(lab, labelPx);
        const float padX = std::max(14.0f * scale, drawn * 0.55f);
        const float padY = std::max(8.0f * scale, drawn * 0.38f);
        closeW = std::max(closeW, tw + padX * 2.0f);
        closeH = std::max(closeH, drawn + padY * 2.0f);
        headerH = std::max(headerH, closeH + 20.0f * scale);
    }
    const float tx = a.x + 39.0f * scale;
    const float ty = a.y + headerH * 0.5f - headerTextSize * 0.5f;
    const Color primaryCol = toColor(CLR_TEXT_PRIMARY);
    if (terminusChrome) {
        const float maxW = std::max(0.0f, a.w - 39.0f * scale - closeW - closeMargin - 8.0f * scale);
        const std::string shown = terminusEllipsize(title, headerTextSize, maxW);
        drawTerminusText(canvas, shown, tx, ty, headerTextSize, primaryCol);
    } else {
        canvas.textStyled(title, tx, ty, headerTextSize, primaryCol, FontStyle::Bold);
    }
    canvas.rect(a.x, a.y + headerH, a.w, std::max(1.0f, std::round(scale)),
                toColor(themeRule()));

    closeRc = { (int)(area.right - closeW - closeMargin), (int)(area.top + (headerH - closeH) * 0.5f),
                (int)(area.right - closeMargin),          (int)(area.top + (headerH + closeH) * 0.5f) };
    // Drawn HERE, before the panel body floods the curve buffer with Terminus
    // pixel-rects. Close used to be painted at the end of drawActivePanel, and
    // a Settings page with a list dropped it (and the action buttons) while
    // Manage Folders -- almost no body glyphs -- still showed it.
    if (terminusChrome)
        drawButton(canvas, closeRc, lab, closeHover, labelPx, false, true);

    return { area.left, (int)(area.top + headerH), area.right, area.bottom };
}

void drawScrollbar(Canvas& canvas, const LayoutRect& listArea,
                   int contentH, int scrollY, float scale) {
    Rect a = toRect(listArea);
    if (a.h <= 0.0f || contentH <= (int)a.h) return;   // everything already visible

    float barW = 9.0f * scale;
    float x    = a.x + a.w - barW - SP_XS * scale;

    canvas.rect(x, a.y, barW, a.h, toColor(themeRule()));

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
    const float textY = r.y + (r.h - textSize) * 0.5f;
    const panels::PxMark& disc = radioDisc();
    const float pref = r.x + r.h * 0.34f;
    PxPlace mark = placePxMark(r.x, textY, textSize, pref, disc.h);
    // The selected row's accent bar is 3 device px. The mark has to clear it
    // or the bar slices the ring.
    const float minLeft = r.x + 3.0f + (float)mark.scale;
    while (mark.x < minLeft)
        mark.x += (float)mark.scale;
    const float labelX = mark.x + (float)(disc.w * mark.scale) + (float)(kPxCell * mark.scale);
    const float pad = r.h * 0.34f;
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

    // Ring when idle, ring plus the inner dot when chosen. Both are the
    // disc's own pixels, so selecting does not change the silhouette.
    if (selected) {
        paintPx(canvas, mark.x, mark.y, radioRing(), mark.scale, dotOn);
        const int inset = radioDotOffset() * mark.scale;
        paintPx(canvas, mark.x + (float)inset, mark.y + (float)inset,
                radioDot(), mark.scale, dotOn);
    } else {
        paintPx(canvas, mark.x, mark.y, radioRing(), mark.scale, dotOff);
    }
    drawTerminusText(canvas, shown, labelX, textY, textSize,
                     selected ? textOn : textOff);
    return { (int)hit.x, (int)hit.y, (int)(hit.x + hit.w), (int)(hit.y + hit.h) };
}

std::vector<TerminusListRow> drawTerminusScrollList(
    Canvas& canvas, const LayoutRect& area, const std::vector<std::string>& items,
    int selected, float scrollPx, float rowH, int hoverIndex, float textSize,
    const Color& rowText, const Color& hoverBg, const Color& pillColor,
    const Color& pillText, const Color& selectedBar, float inset) {
    Rect a = toRect(area);
    canvas.rect(a.x, a.y, a.w, a.h, toColor(CLR_BG_MAIN));
    const auto saved = canvas.saveClip();
    canvas.setClip(a.x, a.y, a.w, a.h);
    const auto clip = canvas.saveClip();
    const float in = std::max(0.0f, inset);
    std::vector<TerminusListRow> visible;
    for (int i = 0; i < (int)items.size(); i++) {
        const float ry = a.y + (float)i * rowH - scrollPx;
        if (ry + rowH < a.y || ry > a.y + a.h) continue;
        // The list rect can be taller than the parent clip (a body that
        // scrolls around a huge catalogue). Skip rows the intersected clip
        // has already rejected so we do not emit thousands of clipped
        // Terminus runs for items that cannot appear. Skip also when the
        // glyph band would be sliced — a 1-bit face cut through the middle
        // reads as apostrophes, which is how "1C''" happened.
        if (clip.active && (ry + rowH < clip.y0 || ry > clip.y1)) continue;
        const float textY = ry + (rowH - textSize) * 0.5f;
        if (clip.active && (textY < clip.y0 || textY + textSize > clip.y1 + 0.01f))
            continue;
        Rect r{ a.x, ry, a.w, rowH };
        const float textX = r.x + in;
        const std::string shown = terminusEllipsize(items[(size_t)i], textSize,
                                                    std::max(0.0f, r.w - in * 2.0f));
        const bool sel = (i == selected);
        const float rad = 0.0f;
        if (sel)
            canvas.rect(r.x + in * 0.3f, r.y, r.w - in * 0.6f, rowH, pillColor, rad);
        else if (i == hoverIndex)
            canvas.rect(r.x + in * 0.3f, r.y, r.w - in * 0.6f, rowH, hoverBg, rad);
        if (sel && selectedBar.a > 0.0f)
            canvas.rect(r.x + in * 0.3f, r.y, 3.0f, rowH, selectedBar, rad);
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
    const float textY = r.y + (r.h - textSize) * 0.5f;
    const panels::PxMark& track = toggleFill();
    PxPlace probe = placePxMark(r.x, textY, textSize, r.x, track.h);
    const float tw = (float)(track.w * probe.scale);
    float pref = r.x + r.w - tw;
    PxPlace mark = placePxMark(r.x, textY, textSize, pref, track.h);
    if (mark.x + tw > r.x + r.w + 0.01f)
        mark.x -= (float)mark.scale;
    const float maxW = std::max(0.0f, mark.x - r.x - (float)(kPxCell * mark.scale));
    const std::string shown = terminusEllipsize(label, textSize, maxW);
    drawTerminusText(canvas, shown, r.x, textY, textSize, labelColor);
    // Off is the outline: a separator-grey fill at this size is a smudge,
    // and the 2px stroke is the same weight as the letters. On fills the
    // track. The knob is the same disc either way.
    paintPx(canvas, mark.x, mark.y, on ? toggleFill() : toggleOutline(), mark.scale,
            on ? onColor : offColor);
    const float kx = mark.x + (float)(toggleKnobColumn(on) * mark.scale);
    const float ky = mark.y + (float)(toggleKnobRow() * mark.scale);
    paintPx(canvas, kx, ky, toggleKnob(), mark.scale, knobColor);
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
    const auto clip = canvas.saveClip();
    const terminus::Pick pk = terminus::pickStrike(U'M', targetPx);
    const float cellH = pk.glyph ? (float)(pk.glyph->cellH * pk.scale) : targetPx;
    for (size_t i = 0; i < lines.size(); ++i) {
        const float yy = y + (float)i * lineH;
        if (clip.active && (yy + cellH > clip.y1 + 0.01f || yy + cellH < clip.y0))
            continue;
        drawTerminusText(canvas, lines[i], x, yy, targetPx, col);
    }
    if (lines.empty()) return y;
    return y + (float)lines.size() * lineH;
}

} // namespace panels

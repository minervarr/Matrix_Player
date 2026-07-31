// Pure icon math — no Canvas, no Vulkan, so ui_icons_test links this exact
// translation unit instead of re-deriving the placement rule. The drawing half
// lives in ui_icons_draw.cc.
#include "ui_icons.hh"

#include <algorithm>

// Both lookups are explicit switches rather than a table indexed by the enum:
// -Wswitch then makes a forgotten icon a build error instead of a silently
// blank button.
unsigned uiIconCodepoint(UiIcon icon) {
    switch (icon) {
    case UiIcon::Play:     return kIconCpPlay;
    case UiIcon::Stop:     return kIconCpStop;
    case UiIcon::Prev:     return kIconCpPrev;
    case UiIcon::Next:     return kIconCpNext;
    case UiIcon::Settings: return kIconCpSettings;
    case UiIcon::Warning:  return kIconCpWarning;
    case UiIcon::Quality:  return kIconCpQuality;
    }
    return 0;
}

float uiIconBoxEm(UiIcon icon) {
    switch (icon) {
    case UiIcon::Play:     return kIconBoxPlay;
    case UiIcon::Stop:     return kIconBoxStop;
    case UiIcon::Prev:     return kIconBoxPrev;
    case UiIcon::Next:     return kIconBoxNext;
    case UiIcon::Settings: return kIconBoxSettings;
    case UiIcon::Warning:  return kIconBoxWarning;
    case UiIcon::Quality:  return kIconBoxQuality;
    }
    return 0.0f;
}

IconPlacement uiIconPlacement(const LayoutRect& rc, UiIcon icon) {
    const float w = (float)(rc.right - rc.left);
    const float h = (float)(rc.bottom - rc.top);
    const float s = std::min(w, h);                 // same framing as the old icons
    const float ox = (float)rc.left + (w - s) * 0.5f;
    const float oy = (float)rc.top  + (h - s) * 0.5f;

    // The box spans boxEm ems, so one em is s/boxEm pixels. Its bottom edge is
    // the baseline (at oy + s); Canvas::text() takes the text TOP and adds
    // +size internally, so hand it baseline - size.
    const float boxEm = uiIconBoxEm(icon);
    if (boxEm <= 0.0f) return { ox, oy + s, 0.0f };  // unmapped — caller skips
    const float sizePx = s / boxEm;
    return { ox, oy + s - sizePx, sizePx };
}

#include "bar_a.hh"

#include "canvas.hh"
#include "terminus_glyph.hh"
#include "text_font.hh"   // FontStyle -- canvas.hh only forward-declares it
#include "text_util.hh"   // truncateToWidth -- lives in vk_canvas, already shared

// The two bridges player_view.cc also keeps. Three lines each, and deliberately
// duplicated rather than exported from there: this file must not include the
// desktop app's header, and moving them into color.hh would drag Canvas's Rect
// into a header that is currently free of it.
static Rect toRect(const LayoutRect& r) {
    return { (float)r.left, (float)r.top,
             (float)(r.right - r.left), (float)(r.bottom - r.top) };
}
static Color toColor(ColorRef c, float a = 1.0f) {
    return { GetRValue(c) / 255.0f, GetGValue(c) / 255.0f, GetBValue(c) / 255.0f, a };
}
static bool ptIn(const LayoutRect& r, int x, int y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

void drawSearchField(Canvas& canvas, const LayoutRect& rc, const std::string& text,
                     bool focused, const char* placeholder, float textSize) {
    Rect s = toRect(rc);
    canvas.rect(s.x, s.y, s.w, s.h, toColor(CLR_INPUT_BG), UI_CORNER_RADIUS);
    canvas.rect(s.x, s.y + s.h - 1, s.w, 1, toColor(focused ? CLR_ACCENT : CLR_SEPARATOR));
    bool empty = text.empty() && !focused;
    std::string shown = empty ? placeholder : text;
    ColorRef clr = empty ? CLR_TEXT_DIM : CLR_TEXT_PRIMARY;
    std::string fit = truncateToWidth(canvas, shown, s.w - 16 - 8, textSize, FontStyle::Roman);
    if (focused) fit += "|";
    canvas.text(fit, s.x + 8, s.y + s.h * 0.5f - textSize * 0.5f, textSize, toColor(clr));
}

// ── Drawing ──────────────────────────────────────────────────────────────────
//
// THE LETTERS ARE NOT ROTATED in the horizontal layout: a single capital reads
// the same upright at any orientation. Rotation is only for text whose LENGTH
// runs along the bar -- the AutoEQ profile name.
//
// Three cells used to be told apart only by theme.hh's grey ladder, because
// three of them said "S". They are now told apart by TYPEFACE AND WEIGHT first,
// and the ladder only confirms:
//
//   Settings  S              Terminus (a 1-bit pixel face)   DIM 128
//   Find      F              Computer Modern, regular        SECONDARY 170
//   filters   A E S C L R P  Computer Modern, BOLD           PRIMARY 242
//
// The filters are the most used cells, so they get the heaviest weight, which
// also holds up best in the shrunken cells of a phone held upright. Settings is
// machine chrome, not music, so it gets the terminal face. The one remaining
// pair of equal letters -- Settings and Singles -- is a pixel S beside a bold
// serif S, which do not resemble each other at any size.

namespace {

void drawEqBox(Canvas& canvas, const BarAModel& m) {
    if (m.rail.eqBox.right <= m.rail.eqBox.left) return;

    const bool vertical = (m.orient == UiOrientation::Vertical);
    const Rect box = toRect(m.rail.eqBox);

    // A frame, so the region reads as its own thing rather than as more cells.
    const float hair = m.metrics.stroke(1.0f);
    canvas.rect(box.x, box.y, box.w, box.h, toColor(CLR_BG_TRANSPORT));
    if (vertical) {
        canvas.rect(box.x, box.y, hair, box.h, toColor(CLR_SEPARATOR));
        canvas.rect(box.x + box.w - hair, box.y, hair, box.h, toColor(CLR_SEPARATOR));
    } else {
        canvas.rect(box.x, box.y, box.w, hair, toColor(CLR_SEPARATOR));
        canvas.rect(box.x, box.y + box.h - hair, box.w, hair, toColor(CLR_SEPARATOR));
    }
    // Selected while the switcher it opens is on screen; hover otherwise.
    if (m.eqSwitcherOpen)
        canvas.rect(box.x, box.y, box.w, box.h,
                    toColor(CLR_ACCENT, UI_SELECT_TINT_ALPHA), UI_CORNER_RADIUS);
    else if (m.hovered.item == BarAItem::EqBox)
        canvas.rect(box.x, box.y, box.w, box.h, toColor(CLR_HOVER), UI_CORNER_RADIUS);

    // The name is the one text in bar A whose LENGTH runs along the bar, so it
    // is rotated in the horizontal layout. Canvas honours setRotation() for
    // text (canvas.hh:175); it does not for image(), which is why nothing in
    // either bar depends on a rotated bitmap.
    const std::string text = m.eqNone ? "No AutoEQ" : m.eqName;
    const float sz  = m.metrics.text.secondary;
    const float pad = m.metrics.space(SP_SM);
    const ColorRef clr = m.eqNone ? CLR_TEXT_DIM
                       : (m.eqTentative ? CLR_TEXT_SECONDARY : CLR_TEXT_PRIMARY);
    if (vertical) {
        canvas.text(truncateToWidth(canvas, text, box.w - pad * 2, sz, FontStyle::Roman),
                    box.x + pad, box.y + box.h * 0.5f - sz * 0.5f, sz, toColor(clr));
        return;
    }
    const float cx = box.x + box.w * 0.5f, cy = box.y + box.h * 0.5f;
    canvas.setRotation(-1.57079633f, cx, cy);
    const std::string shown =
        truncateToWidth(canvas, text, box.h - pad * 2, sz, FontStyle::Roman);
    const float tw = canvas.textWidth(shown, sz);
    canvas.text(shown, cx - tw * 0.5f, cy - sz * 0.5f, sz, toColor(clr));
    canvas.clearRotation();
}

}  // namespace

void drawBarA(Canvas& canvas, const BarAModel& m) {
    const Rect bar = toRect(m.bar);
    if (bar.w <= 0 || bar.h <= 0) return;

    canvas.rect(bar.x, bar.y, bar.w, bar.h, toColor(CLR_BG_SIDEBAR));

    // The hairline sits on the bar's INNER edge -- the one facing the content --
    // which is the bottom in Vertical and the right in Horizontal.
    const float hair = m.metrics.stroke(1.0f);
    if (m.orient == UiOrientation::Vertical)
        canvas.rect(bar.x, bar.y + bar.h - hair, bar.w, hair, toColor(CLR_SEPARATOR));
    else
        canvas.rect(bar.x + bar.w - hair, bar.y, hair, bar.h, toColor(CLR_SEPARATOR));

    // A cell's background: selected (accent tint + a 3px accent bar on the
    // inner edge, pointing at the content it filters) or hovered (neutral
    // grey). Same selection family as the sidebar rows this replaced. False
    // when the cell is hidden, so the caller draws nothing in it.
    auto cellBg = [&](const LayoutRect& lr, bool active, bool hovered) -> bool {
        if (lr.right <= lr.left) return false;
        const Rect r = toRect(lr);
        if (active) {
            canvas.rect(r.x, r.y, r.w, r.h,
                        toColor(CLR_ACCENT, UI_SELECT_TINT_ALPHA), UI_CORNER_RADIUS);
            const float t = m.metrics.stroke(3.0f);
            if (m.orient == UiOrientation::Vertical)
                canvas.rect(r.x, r.y + r.h - t, r.w, t, toColor(CLR_ACCENT));
            else
                canvas.rect(r.x + r.w - t, r.y, t, r.h, toColor(CLR_ACCENT));
        } else if (hovered) {
            canvas.rect(r.x, r.y, r.w, r.h, toColor(CLR_HOVER), UI_CORNER_RADIUS);
        }
        return true;
    };
    // One Computer Modern glyph, centred in its cell, in the given weight.
    auto glyph = [&](const LayoutRect& lr, const char* g, FontStyle style, ColorRef clr) {
        const Rect r   = toRect(lr);
        const float sz = m.metrics.text.title;
        const float w  = canvas.textWidthStyled(g, sz, style);
        canvas.textStyled(g, r.x + (r.w - w) * 0.5f, r.y + r.h * 0.5f - sz * 0.5f,
                          sz, toColor(clr), style);
    };

    if (!m.searchOpen) {
        // Initials, in the reading order rail_layout.hh fixes: original
        // material by descending size (Albums, EPs, Singles), then the artist's
        // own material re-presented (Compilations, Live), then other people's
        // reworkings of it (Remixes), then Playlists. BOLD: these are the cells
        // the listener uses most.
        static const char* kGlyphs[] = { "A", "E", "S", "C", "L", "R" };
        for (int i = kRailAlbums; i <= kRailRemixes; i++) {
            const bool active = (m.activeLetter == i);
            if (cellBg(m.rail.letters[i], active,
                       m.hovered.item == BarAItem::Filter && m.hovered.index == i && !active))
                glyph(m.rail.letters[i], kGlyphs[i], FontStyle::Bold,
                      active ? CLR_ACCENT : CLR_TEXT_PRIMARY);
        }
        if (cellBg(m.rail.letters[kRailPlaylists], m.playlistsActive,
                   m.hovered.item == BarAItem::Playlists && !m.playlistsActive))
            glyph(m.rail.letters[kRailPlaylists], "P", FontStyle::Bold,
                  m.playlistsActive ? CLR_ACCENT : CLR_TEXT_PRIMARY);

        // Find: an F, which no filter uses, in the REGULAR weight and one step
        // down the ladder -- it is about the music, but it is not a section.
        if (cellBg(m.rail.search, false, m.hovered.item == BarAItem::Search))
            glyph(m.rail.search, "F", FontStyle::Roman, CLR_TEXT_SECONDARY);
    } else {
        // Open search: the field spans the middle, with a close cell at the far
        // end. The letters and the AutoEQ box are both gone -- see
        // computeRailLayout(), which is why this state looks identical in
        // bit-perfect and in Reference EQ.
        drawSearchField(canvas, m.rail.search, m.searchQuery, m.searchFocused,
                        "Search your library", m.metrics.text.secondary);
        if (cellBg(m.rail.close, false, m.hovered.item == BarAItem::SearchClose))
            glyph(m.rail.close, "×", FontStyle::Roman, CLR_TEXT_SECONDARY);
    }

    // Settings: pinned at the near end in every state, in TERMINUS. It does not
    // move when search opens: interrupting a filter to change a setting must
    // not cost what was typed. The Computer Modern fallback only runs if the
    // baked glyph is missing, which terminus_glyph_test makes a build-time
    // failure rather than something to discover on screen.
    if (cellBg(m.rail.settings, m.settingsActive,
               m.hovered.item == BarAItem::Settings && !m.settingsActive)) {
        const ColorRef c = m.settingsActive ? CLR_ACCENT : CLR_TEXT_DIM;
        if (!drawTerminusGlyph(canvas, m.rail.settings, U'S', m.metrics.text.title, toColor(c)))
            glyph(m.rail.settings, "S", FontStyle::Roman, c);
    }

    drawEqBox(canvas, m);
}

// ── Hit-testing ──────────────────────────────────────────────────────────────
//
// The same rects the drawing used, in the same file, so the two cannot disagree
// about where anything is.
BarAPick barAHitTest(const BarAModel& m, int x, int y) {
    for (int i = kRailAlbums; i <= kRailRemixes; i++)
        if (ptIn(m.rail.letters[i], x, y)) return { BarAItem::Filter, i };
    if (ptIn(m.rail.letters[kRailPlaylists], x, y)) return { BarAItem::Playlists, -1 };
    if (ptIn(m.rail.settings, x, y))               return { BarAItem::Settings, -1 };
    if (ptIn(m.rail.eqBox, x, y))                  return { BarAItem::EqBox, -1 };
    if (ptIn(m.rail.close, x, y))                  return { BarAItem::SearchClose, -1 };
    // The search rect is the Find cell when closed and the field when open;
    // only the former is a click target -- the field handles its own focus.
    if (ptIn(m.rail.search, x, y) && !m.searchOpen) return { BarAItem::Search, -1 };
    return {};
}

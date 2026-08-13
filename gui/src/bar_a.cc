#include "bar_a.hh"

#include "canvas.hh"
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
// THE LETTERS ARE NOT ROTATED in the horizontal layout, and that is deliberate
// even though everything else in the frame is. A single capital reads the same
// upright at any orientation, and turning it on its side would cost legibility
// to buy a consistency nobody can see. Rotation is for text whose LENGTH runs
// along the bar -- the AutoEQ profile name, the now-playing title -- where
// upright would not fit at all.
//
// Colour is theme.hh's text ladder, unchanged: filters PRIMARY (242), search
// SECONDARY (170), Settings DIM (128, the WCAG floor). That ladder is what
// separates the three cells that all say "S" -- Singles, Search, Settings.
// Position is what teaches them; colour only confirms. There is no room for a
// fourth S: see theme.hh's own comment on the floor.

namespace {

// The unfurled AutoEQ list, painted after the box it grows out of. Split out
// only to keep drawBarA readable; it has no separate meaning.
void drawEqBox(Canvas& canvas, const BarAModel& m) {
    if (m.rail.eqBox.right <= m.rail.eqBox.left) return;

    const bool vertical = (m.orient == UiOrientation::Vertical);
    Rect box = toRect(m.rail.eqBox);

    // A frame around it, so the region reads as its own thing rather than as
    // two more cells in the letter row.
    const float hair = m.metrics.stroke(1.0f);
    canvas.rect(box.x, box.y, box.w, box.h, toColor(CLR_BG_TRANSPORT));
    if (vertical) {
        canvas.rect(box.x, box.y, hair, box.h, toColor(CLR_SEPARATOR));
        canvas.rect(box.x + box.w - hair, box.y, hair, box.h, toColor(CLR_SEPARATOR));
    } else {
        canvas.rect(box.x, box.y, box.w, hair, toColor(CLR_SEPARATOR));
        canvas.rect(box.x, box.y + box.h - hair, box.w, hair, toColor(CLR_SEPARATOR));
    }

    // Rotated label: the AutoEQ name is exactly the text whose LENGTH runs
    // along the bar, so upright would not fit. Canvas honours setRotation()
    // for text (canvas.hh:175); it does not for image(), which is why nothing
    // in either bar depends on a rotated bitmap.
    auto label = [&](const LayoutRect& lr, const std::string& text, float sz,
                     ColorRef clr) {
        Rect r = toRect(lr);
        const float pad = m.metrics.space(SP_SM);
        if (vertical) {
            canvas.text(truncateToWidth(canvas, text, r.w - pad * 2, sz, FontStyle::Roman),
                        r.x + pad, r.y + r.h * 0.5f - sz * 0.5f, sz, toColor(clr));
            return;
        }
        const float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
        canvas.setRotation(-1.57079633f, cx, cy);
        const std::string shown =
            truncateToWidth(canvas, text, r.h - pad * 2, sz, FontStyle::Roman);
        const float tw = canvas.textWidth(shown, sz);
        canvas.text(shown, cx - tw * 0.5f, cy - sz * 0.5f, sz, toColor(clr));
        canvas.clearRotation();
    };

    {
        Rect r = toRect(m.rail.eqNone);
        if (m.hovered.item == BarAItem::EqNone && !m.eqNone)
            canvas.rect(r.x, r.y, r.w, r.h, toColor(CLR_HOVER), UI_CORNER_RADIUS);
        const float sz = m.metrics.text.body;
        // U+00D7 MULTIPLICATION SIGN, not a lowercase x: it is a symbol here,
        // not a letter, and the letters beside it are all real initials.
        canvas.textCentered("\xC3\x97", r.x + r.w * 0.5f, r.y + r.h * 0.5f - sz * 0.5f,
                            sz, toColor(m.eqNone ? CLR_ACCENT : CLR_TEXT_DIM));
    }
    {
        Rect r = toRect(m.rail.eqName);
        if (m.hovered.item == BarAItem::EqName)
            canvas.rect(r.x, r.y, r.w, r.h, toColor(CLR_HOVER), UI_CORNER_RADIUS);
        label(m.rail.eqName, m.eqNone ? "No AutoEQ" : m.eqName,
              m.metrics.text.secondary,
              m.eqNone ? CLR_TEXT_DIM
                       : (m.eqTentative ? CLR_TEXT_SECONDARY : CLR_TEXT_PRIMARY));
    }

    if (!m.eqListOpen) return;

    // The list HIDES the filter letters rather than floating over them --
    // forced by the renderer, which emits every rect before every glyph (see
    // RailInput::eqListOpen), so an overlay drawn last still comes out UNDER
    // text drawn earlier.
    for (size_t i = 0; i < m.eqRows.size(); i++) {
        const BarAEqRow& hr = m.eqRows[i];
        Rect r = toRect(hr.rc);
        canvas.rect(r.x, r.y, r.w, r.h, toColor(CLR_BG_TRANSPORT));
        if (hr.active)
            canvas.rect(r.x, r.y, r.w, r.h,
                        toColor(CLR_ACCENT, UI_SELECT_TINT_ALPHA), UI_CORNER_RADIUS);
        else if (m.hovered.item == BarAItem::EqRow && m.hovered.index == (int)i)
            canvas.rect(r.x, r.y, r.w, r.h, toColor(CLR_HOVER), UI_CORNER_RADIUS);

        label(hr.rc, hr.name, m.metrics.text.secondary,
              hr.active ? CLR_ACCENT : (hr.trial ? CLR_TEXT_DIM : CLR_TEXT_SECONDARY));
    }
    if (m.eqMore.right > m.eqMore.left) {
        Rect r = toRect(m.eqMore);
        canvas.rect(r.x, r.y, r.w, r.h, toColor(CLR_BG_TRANSPORT));
        if (m.hovered.item == BarAItem::EqMore)
            canvas.rect(r.x, r.y, r.w, r.h, toColor(CLR_HOVER), UI_CORNER_RADIUS);
        label(m.eqMore, "Search more\xE2\x80\xA6", m.metrics.text.caption, CLR_TEXT_DIM);
    }
}

}  // namespace

void drawBarA(Canvas& canvas, const BarAModel& m) {
    Rect bar = toRect(m.bar);
    if (bar.w <= 0 || bar.h <= 0) return;

    canvas.rect(bar.x, bar.y, bar.w, bar.h, toColor(CLR_BG_SIDEBAR));

    // The hairline sits on the bar's INNER edge -- the one facing the content --
    // which is the bottom in Vertical and the right in Horizontal.
    const float hair = m.metrics.stroke(1.0f);
    if (m.orient == UiOrientation::Vertical)
        canvas.rect(bar.x, bar.y + bar.h - hair, bar.w, hair, toColor(CLR_SEPARATOR));
    else
        canvas.rect(bar.x + bar.w - hair, bar.y, hair, bar.h, toColor(CLR_SEPARATOR));

    // One cell: optional selection/hover treatment, then a centred glyph. Same
    // selection family as the sidebar rows this replaced (accent tint + a 3px
    // accent bar, hover a neutral grey), so nothing about what "selected" looks
    // like changed -- only the shape it is drawn in.
    auto cell = [&](const LayoutRect& lr, const char* glyph, ColorRef base,
                    bool active, bool hovered) {
        if (lr.right <= lr.left) return;
        Rect r = toRect(lr);
        if (active) {
            canvas.rect(r.x, r.y, r.w, r.h,
                        toColor(CLR_ACCENT, UI_SELECT_TINT_ALPHA), UI_CORNER_RADIUS);
            // The accent bar goes on the inner edge, so it points at the
            // content it is filtering -- the rotated equivalent of the left bar
            // the sidebar rows used to carry.
            const float t = m.metrics.stroke(3.0f);
            if (m.orient == UiOrientation::Vertical)
                canvas.rect(r.x, r.y + r.h - t, r.w, t, toColor(CLR_ACCENT));
            else
                canvas.rect(r.x + r.w - t, r.y, t, r.h, toColor(CLR_ACCENT));
        } else if (hovered) {
            canvas.rect(r.x, r.y, r.w, r.h, toColor(CLR_HOVER), UI_CORNER_RADIUS);
        }
        const float sz = m.metrics.text.title;
        canvas.textCentered(glyph, r.x + r.w * 0.5f, r.y + r.h * 0.5f - sz * 0.5f,
                            sz, toColor(active ? CLR_ACCENT : base));
    };

    // Initials, in the reading order rail_layout.hh fixes: original material by
    // descending size (Albums, EPs, Singles), then the artist's own material
    // re-presented (Compilations, Live), then other people's reworkings of it
    // (Remixes), then Playlists.
    if (!m.searchOpen && !m.eqListOpen) {
        static const char* kGlyphs[] = { "A", "E", "S", "C", "L", "R" };
        for (int i = kRailAlbums; i <= kRailRemixes; i++) {
            const bool active = (m.activeLetter == i);
            cell(m.rail.letters[i], kGlyphs[i], CLR_TEXT_PRIMARY, active,
                 m.hovered.item == BarAItem::Filter && m.hovered.index == i && !active);
        }
        cell(m.rail.letters[kRailPlaylists], "P", CLR_TEXT_PRIMARY, m.playlistsActive,
             m.hovered.item == BarAItem::Playlists && !m.playlistsActive);

        // Search: one step down the ladder from the filters. It is about the
        // music, so it outranks Settings; it is not one of the sections, so it
        // does not sit level with them.
        cell(m.rail.search, "S", CLR_TEXT_SECONDARY, false,
             m.hovered.item == BarAItem::Search);
    } else if (m.searchOpen) {
        // Open search: the field spans the middle, with a close cell at the far
        // end. The filter letters and the AutoEQ box are both gone -- see
        // computeRailLayout(), which is why this state looks identical in
        // bit-perfect and in Reference EQ.
        drawSearchField(canvas, m.rail.search, m.searchQuery, m.searchFocused,
                        "Search your library", m.metrics.text.secondary);
        cell(m.rail.close, "\xC3\x97", CLR_TEXT_SECONDARY, false,
             m.hovered.item == BarAItem::SearchClose);
    }

    // Pinned at the near end in every state, DIM at the bottom of the ladder.
    // It does not move when search opens: interrupting a filter to change a
    // setting must not cost what was typed.
    cell(m.rail.settings, "S", CLR_TEXT_DIM, m.settingsActive,
         m.hovered.item == BarAItem::Settings && !m.settingsActive);

    drawEqBox(canvas, m);
}

// ── Hit-testing ──────────────────────────────────────────────────────────────
//
// The same rects the drawing used, in the same file, so the two cannot disagree
// about where anything is. Order matters where rects can overlap, and the
// comments say where.
BarAPick barAHitTest(const BarAModel& m, int x, int y) {
    for (int i = kRailAlbums; i <= kRailRemixes; i++)
        if (ptIn(m.rail.letters[i], x, y)) return { BarAItem::Filter, i };
    if (ptIn(m.rail.letters[kRailPlaylists], x, y)) return { BarAItem::Playlists, -1 };
    if (ptIn(m.rail.settings, x, y))               return { BarAItem::Settings, -1 };

    // The unfurled list is tested before the letters would be reached anyway --
    // computeRailLayout() empties them while it is open, which is what stops a
    // click meant for a profile landing on the filter underneath.
    for (int i = 0; i < (int)m.eqRows.size(); i++)
        if (ptIn(m.eqRows[i].rc, x, y)) return { BarAItem::EqRow, i };
    if (ptIn(m.eqMore, x, y))       return { BarAItem::EqMore, -1 };
    if (ptIn(m.rail.eqNone, x, y))  return { BarAItem::EqNone, -1 };
    if (ptIn(m.rail.eqName, x, y))  return { BarAItem::EqName, -1 };

    // The search cell is one rect that means two things depending on state: the
    // letter that OPENS search, or the field it turns into. Only the former is
    // a click target -- the field handles its own focus.
    if (ptIn(m.rail.close, x, y))  return { BarAItem::SearchClose, -1 };
    if (ptIn(m.rail.search, x, y) && !m.searchOpen) return { BarAItem::Search, -1 };
    return {};
}

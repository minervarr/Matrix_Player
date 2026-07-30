// Asserts must stay live even though the app builds Release (NDEBUG).
#undef NDEBUG
#include <cassert>
#include <cmath>
#include <cstdio>
#include <set>

// The REAL placement math, linked from src/ui_icons.cc — not restated here.
// That is why ui_icons.hh forward-declares Canvas/Color rather than including
// canvas.hh, and why the drawing half lives in ui_icons_draw.cc.
#include "ui_icons.hh"

static bool nearlyEqual(float a, float b) { return std::fabs(a - b) < 0.001f; }

static const UiIcon kAll[] = { UiIcon::Play, UiIcon::Stop, UiIcon::Prev,
                               UiIcon::Next, UiIcon::Settings, UiIcon::Warning };

int main() {
    // ── Codepoints are unique and inside the Private Use Area ──────────────
    std::set<unsigned> seen;
    for (unsigned cp : kIconCodepoints) {
        assert(cp >= 0xE000 && cp <= 0xF8FF);   // BMP PUA
        assert(seen.insert(cp).second);          // no duplicates
    }
    assert(seen.size() == sizeof(kIconCodepoints) / sizeof(kIconCodepoints[0]));

    // ── Every enumerator maps to a codepoint the font ships, uniquely ─────
    // Catches the drift that matters: adding a UiIcon without adding its SVG
    // (or renumbering ICONS in build_icon_font.py) would otherwise surface as
    // a silently blank — or silently WRONG — button at runtime. Checking only
    // "is this a known icon codepoint" is not enough: a copy-paste that made
    // two enumerators return the same constant passed that weaker check.
    std::set<unsigned> mapped;
    for (UiIcon icon : kAll) {
        const unsigned cp = uiIconCodepoint(icon);
        assert(cp != 0);                    // every enumerator is mapped
        assert(seen.count(cp) == 1);        // ... to a codepoint the font ships
        assert(mapped.insert(cp).second);   // ... and uniquely so
    }
    // Every shipped codepoint is reachable from some enumerator, so an icon
    // can't be baked into the atlas yet be undrawable.
    assert(mapped.size() == seen.size());

    // ── Every icon declares a usable design box ───────────────────────────
    // The box sets bake density (an N-em outline is baked at N*96px), so a
    // missing or nonsense value silently changes how sharp an icon is. It is
    // bounded on BOTH sides deliberately: too small softens under
    // magnification, too large means heavy minification, which smears just as
    // badly — the bug that made the 4-em gear and warning look blurry.
    for (UiIcon icon : kAll) {
        const float box = uiIconBoxEm(icon);
        assert(box >= kIconBoxEmMin);
        assert(box <= 4.0f);
    }

    // ── Square rect: the box fills it exactly, at any density ─────────────
    for (UiIcon icon : kAll) {
        const float box = uiIconBoxEm(icon);
        LayoutRect rc{ 100, 200, 180, 280 };      // 80x80
        IconPlacement p = uiIconPlacement(rc, icon);
        assert(nearlyEqual(p.sizePx, 80.0f / box));
        assert(nearlyEqual(p.x, 100.0f));
        // Baseline is the box's bottom edge; Canvas::text() adds +size back on.
        assert(nearlyEqual(p.y + p.sizePx, 280.0f));
    }

    // ── Wide rect: square is centred horizontally, flush vertically ───────
    {
        LayoutRect rc{ 0, 0, 200, 80 };           // 200x80 -> 80px box
        IconPlacement p = uiIconPlacement(rc, UiIcon::Play);
        assert(nearlyEqual(p.sizePx, 80.0f / uiIconBoxEm(UiIcon::Play)));
        assert(nearlyEqual(p.x, 60.0f));          // (200-80)/2
        assert(nearlyEqual(p.y + p.sizePx, 80.0f));
    }

    // ── Tall rect: square is centred vertically, flush horizontally ──────
    {
        LayoutRect rc{ 10, 0, 90, 200 };          // 80x200 -> 80px box
        IconPlacement p = uiIconPlacement(rc, UiIcon::Warning);
        assert(nearlyEqual(p.sizePx, 80.0f / uiIconBoxEm(UiIcon::Warning)));
        assert(nearlyEqual(p.x, 10.0f));
        assert(nearlyEqual(p.y + p.sizePx, 140.0f));  // (200-80)/2 + 80
    }

    // ── The box always lands inside the rect, at every size and density ──
    for (UiIcon icon : kAll) {
        for (int side : { 8, 17, 40, 71, 150, 284, 960 }) {
            LayoutRect rc{ 5, 7, 5 + side, 7 + side };
            IconPlacement p = uiIconPlacement(rc, icon);
            const float boxTop = p.y + p.sizePx - side;   // baseline - box height
            assert(p.x >= (float)rc.left - 0.001f);
            assert(p.x + side <= (float)rc.right + 0.001f);
            assert(boxTop >= (float)rc.top - 0.001f);
            assert(p.y + p.sizePx <= (float)rc.bottom + 0.001f);
            assert(p.sizePx > 0.0f);
        }
    }

    // ── Scale is linear in the rect, so icons stay sharp by construction ──
    for (UiIcon icon : kAll) {
        IconPlacement a = uiIconPlacement(LayoutRect{ 0, 0, 100, 100 }, icon);
        IconPlacement b = uiIconPlacement(LayoutRect{ 0, 0, 200, 200 }, icon);
        assert(nearlyEqual(b.sizePx, a.sizePx * 2.0f));
    }

    std::printf("ui_icons_test: all assertions passed\n");
    return 0;
}

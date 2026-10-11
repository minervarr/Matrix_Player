#undef NDEBUG
#include "theme.hh"
#include "theme_layout.hh"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

static void assertContiguousGroups() {
    std::vector<std::string> closed;
    std::string cur;
    for (int i = 0; i < themeCount(); ++i) {
        const std::string& g = themeAt(i).group;
        if (g == cur) continue;
        for (const std::string& seen : closed)
            assert(seen != g && "a group reappears");
        if (!cur.empty()) closed.push_back(cur);
        cur = g;
    }
}

static const Palette* findId(const char* id) {
    for (int i = 0; i < themeCount(); ++i)
        if (themeAt(i).id == id) return &themeAt(i);
    return nullptr;
}

static std::vector<uint8_t> rgbaFill(int w, int h, uint8_t r, uint8_t g, uint8_t b,
                                    uint8_t a = 255) {
    std::vector<uint8_t> px((size_t)w * (size_t)h * 4u);
    for (int i = 0; i < w * h; ++i) {
        px[(size_t)i * 4]     = r;
        px[(size_t)i * 4 + 1] = g;
        px[(size_t)i * 4 + 2] = b;
        px[(size_t)i * 4 + 3] = a;
    }
    return px;
}

static void rgbaBlock(std::vector<uint8_t>& px, int w, int x0, int y0, int rw, int rh,
                      uint8_t r, uint8_t g, uint8_t b) {
    for (int y = y0; y < y0 + rh; ++y) {
        for (int x = x0; x < x0 + rw; ++x) {
            const size_t i = ((size_t)y * (size_t)w + (size_t)x) * 4u;
            px[i] = r; px[i + 1] = g; px[i + 2] = b; px[i + 3] = 255;
        }
    }
}

static void rgbaDot(std::vector<uint8_t>& px, int w, int x, int y,
                    uint8_t r, uint8_t g, uint8_t b) {
    const size_t i = ((size_t)y * (size_t)w + (size_t)x) * 4u;
    px[i] = r; px[i + 1] = g; px[i + 2] = b; px[i + 3] = 255;
}

static int channelSpread(ColorRef c) {
    const int r = GetRValue(c), g = GetGValue(c), b = GetBValue(c);
    const int hi = std::max(r, std::max(g, b));
    const int lo = std::min(r, std::min(g, b));
    return hi - lo;
}

static void expectId(const char* got, const char* want) {
    assert(got != nullptr);
    assert(std::string(got) == want);
}

static std::string foldName(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

static void testDisplayOrder() {
    assert(themeCount() == 24);
    std::vector<int> order;
    themeDisplayOrder(order);
    assert((int)order.size() == themeCount());
    std::vector<char> seen((size_t)themeCount(), 0);
    for (int k = 0; k < (int)order.size(); ++k) {
        const int i = order[(size_t)k];
        assert(i >= 0 && i < themeCount());
        assert(!seen[(size_t)i]);
        seen[(size_t)i] = 1;
        if (k + 1 >= (int)order.size()) continue;
        const Palette& a = themeAt(i);
        const Palette& b = themeAt(order[(size_t)k + 1]);
        const int pa = (int)themeDaypartOf(a);
        const int pb = (int)themeDaypartOf(b);
        assert(pa <= pb);
        if (pa != pb) continue;
        const std::string na = foldName(a.name);
        const std::string nb = foldName(b.name);
        assert(na < nb || (na == nb && a.id <= b.id));
    }
    const Palette* paper = findId("paper");
    const Palette* matrix = findId("matrix");
    const Palette* hyper = findId("hyper");
    const Palette* nord = findId("nord");
    assert(paper && matrix && hyper && nord);
    assert(themeDaypartOf(*paper) == ThemeDaypart::Morning);
    assert(themeDaypartOf(*matrix) == ThemeDaypart::Night);
    assert(themeDaypartOf(*hyper) == ThemeDaypart::Night);
    assert(themeDaypartOf(*nord) == ThemeDaypart::Evening);
    assert(themeIsDay(*paper));
    assert(!themeIsDay(*matrix));
}

static void testDayparts() {
    assert(themeDaypartFromLuma(0.93f) == ThemeDaypart::Afternoon);
    assert(themeDaypartFromLuma(0.9301f) == ThemeDaypart::Morning);
    assert(themeDaypartFromLuma(0.45f) == ThemeDaypart::Evening);
    assert(themeDaypartFromLuma(0.4501f) == ThemeDaypart::Afternoon);
    assert(themeDaypartFromLuma(0.03f) == ThemeDaypart::Night);
    assert(themeDaypartFromLuma(0.0301f) == ThemeDaypart::Evening);
    assert(themeDaypartFromLuma(0.0f) == ThemeDaypart::Night);
    assert(themeDaypartFromLuma(1.0f) == ThemeDaypart::Morning);
    assert(std::string(themeDaypartId(ThemeDaypart::Morning)) == "morning");
    assert(std::string(themeDaypartId(ThemeDaypart::Night)) == "night");

    ThemeDaypart named = ThemeDaypart::Night;
    assert(themeDaypartFromName(" Evening ", named) && named == ThemeDaypart::Evening);
    assert(themeDaypartFromName("Morning", named) && named == ThemeDaypart::Morning);
    assert(!themeDaypartFromName("Terminal", named));
    assert(!themeDaypartFromName("Imported", named));
    assert(themeDaypartFromName("Night ", named));
    assert(named == ThemeDaypart::Night);

    Palette builtin = *findId("matrix");
    builtin.group = "morning";
    assert(themeDaypartOf(builtin) == ThemeDaypart::Night);
    builtin.id = "not-builtin";
    builtin.group = "Morning";
    assert(themeDaypartOf(builtin) == ThemeDaypart::Morning);

    const Palette* paper = findId("paper");
    ColorRef sw[20];
    const int n = themeSwatches(*paper, sw, 20);
    assert(n >= 2);
    assert(sw[0] == paper->bgMain);
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j)
            assert(sw[i] != sw[j]);
}

static void testThemeLayout() {
    const float three[] = { 100, 100, 100 };
    std::vector<ThemeFlowSlot> slots;
    float h = themeFlowPack(three, 3, 250, 20, 10, slots);
    assert(slots.size() == 3);
    assert(slots[0].index == 0 && slots[0].x == 0 && slots[0].y == 0 && slots[0].w == 100);
    assert(slots[1].index == 1 && slots[1].x == 110 && slots[1].y == 0 && slots[1].w == 100);
    assert(slots[2].index == 2 && slots[2].x == 0 && slots[2].y == 30 && slots[2].w == 100);
    assert(h == 50);

    const float wide[] = { 400 };
    h = themeFlowPack(wide, 1, 250, 20, 10, slots);
    assert(slots.size() == 1 && slots[0].x == 0 && slots[0].y == 0 && h == 20);
    h = themeFlowPack(nullptr, 0, 250, 20, 10, slots);
    assert(h == 0 && slots.empty());

    std::vector<int> shown;
    bool plus = true;
    themeOverviewRow(three, 3, 250, 10, 40, shown, plus);
    assert(shown.size() == 1 && shown[0] == 0 && plus);

    const float two[] = { 100, 100 };
    themeOverviewRow(two, 2, 250, 10, 40, shown, plus);
    assert(shown.size() == 2 && shown[0] == 0 && shown[1] == 1 && !plus);

    themeOverviewRow(wide, 1, 250, 10, 40, shown, plus);
    assert(shown.size() == 1 && !plus);

    const float twoWide[] = { 400, 400 };
    themeOverviewRow(twoWide, 2, 250, 10, 40, shown, plus);
    assert(shown.size() == 1 && shown[0] == 0 && plus);

    assert(themeStripWidth(0, 10, 2) == 0);
    assert(themeStripWidth(1, 10, 2) == 10);
    assert(themeStripWidth(3, 10, 2) == 34);

    std::vector<int> a{ 0, 1, 2, 3, 4, 5, 6, 7 };
    std::vector<int> b = a;
    themeSampleShuffle(a, 1);
    themeSampleShuffle(b, 0);
    assert(a == b);
    std::vector<int> c{ 0, 1, 2, 3, 4, 5, 6, 7 };
    themeSampleShuffle(c, 1);
    assert(a == c);
    std::vector<int> seen(8, 0);
    for (int v : a) {
        assert(v >= 0 && v < 8);
        seen[(size_t)v] = 1;
    }
    for (int s : seen) assert(s == 1);
    assert(a != std::vector<int>({ 0, 1, 2, 3, 4, 5, 6, 7 }));
}

static int matchAll(const std::vector<uint8_t>& px, int w, int h) {
    const CoverColors c = coverColorsFromRgba(px.data(), w, h);
    std::vector<Palette> pals;
    pals.reserve((size_t)themeCount());
    for (int i = 0; i < themeCount(); ++i) pals.push_back(themeAt(i));
    return matchTheme(c, pals.data(), (int)pals.size());
}

static void testCoverMatch() {
    {
        auto px = rgbaFill(32, 32, 30, 30, 46);
        rgbaBlock(px, 32, 0, 0, 10, 10, 203, 166, 247);
        const int i = matchAll(px, 32, 32);
        assert(i >= 0);
        assert(themeAt(i).id == "catppuccin");
    }
    {
        auto px = rgbaFill(32, 32, 46, 52, 64);
        rgbaBlock(px, 32, 0, 0, 10, 10, 136, 192, 208);
        const int i = matchAll(px, 32, 32);
        assert(i >= 0);
        assert(themeAt(i).id == "nord");
    }
    {
        auto px = rgbaFill(16, 16, 255, 255, 255);
        const CoverColors c = coverColorsFromRgba(px.data(), 16, 16);
        assert(c.ready && !c.hasSignal);
        const int i = matchAll(px, 16, 16);
        assert(i >= 0);
        assert(themeAt(i).id == "paper");
        assert(themeIsDay(themeAt(i)));
    }
    {
        auto px = rgbaFill(32, 32, 0, 0, 0);
        const int i = matchAll(px, 32, 32);
        assert(i >= 0);
        assert(themeAt(i).id == "ink");
    }
    {
        auto px = rgbaFill(32, 32, 0, 0, 0);
        const CoverColors c = coverColorsFromRgba(px.data(), 32, 32);
        const Palette* paper = findId("paper");
        assert(paper);
        assert(matchTheme(c, paper, 1) < 0);
    }
    {
        auto px = rgbaFill(16, 16, 255, 255, 255);
        const CoverColors c = coverColorsFromRgba(px.data(), 16, 16);
        const Palette* ink = findId("ink");
        assert(ink);
        assert(matchTheme(c, ink, 1) < 0);
    }
    {
        Palette later = *findId("paper");
        Palette earlier = later;
        later.id = "bbb";
        earlier.id = "aaa";
        const Palette pair[2] = { later, earlier };
        auto px = rgbaFill(16, 16, 255, 255, 255);
        const CoverColors c = coverColorsFromRgba(px.data(), 16, 16);
        const int i = matchTheme(c, pair, 2);
        assert(i == 1);
        assert(pair[i].id == "aaa");
    }
    {
        // A white sleeve and a magenta accent that sorts first. This is the
        // Avalancha cover: ten themes share pure white, and the lesser id
        // was Enfocado Light, accent RGB(199, 0, 139). The quiet page wins.
        Palette loud = *findId("paper");
        loud.id = "aaa";
        loud.accent = RGB(199, 0, 139);
        const Palette quiet = *findId("paper");
        const Palette pair[2] = { loud, quiet };
        auto px = rgbaFill(16, 16, 255, 255, 255);
        const CoverColors c = coverColorsFromRgba(px.data(), 16, 16);
        assert(c.ready && !c.hasSignal);
        const int i = matchTheme(c, pair, 2);
        assert(i == 1);
        assert(pair[i].id == "paper");
    }
    {
        // The same magenta, now a real mass on the sleeve, still pulls.
        Palette loud = *findId("paper");
        loud.id = "aaa";
        loud.accent = RGB(199, 0, 139);
        const Palette quiet = *findId("paper");
        const Palette pair[2] = { loud, quiet };
        auto px = rgbaFill(32, 32, 255, 255, 255);
        rgbaBlock(px, 32, 0, 0, 10, 10, 199, 0, 139);
        const CoverColors c = coverColorsFromRgba(px.data(), 32, 32);
        assert(c.ready && c.hasSignal);
        const int i = matchTheme(c, pair, 2);
        assert(i == 0);
        assert(pair[i].id == "aaa");
    }
    {
        // White field, black photograph. Both accents are gray, so the
        // neutral charge ties and the ink is what picks the black type.
        Palette gray = *findId("paper");
        gray.id = "aaa";
        gray.accent = RGB(180, 180, 180);
        const Palette quiet = *findId("paper");
        const Palette pair[2] = { gray, quiet };
        auto px = rgbaFill(32, 32, 255, 255, 255);
        rgbaBlock(px, 32, 0, 0, 10, 10, 0, 0, 0);
        const CoverColors c = coverColorsFromRgba(px.data(), 32, 32);
        assert(c.ready && c.hasSignal);
        assert(GetRValue(c.signal) < 8 && GetGValue(c.signal) < 8 && GetBValue(c.signal) < 8);
        const int i = matchTheme(c, pair, 2);
        assert(i == 1);
        assert(pair[i].id == "paper");
    }
    {
        // A warm page stays on a warm sleeve. The neutral charge is smaller
        // than that step, or every light cover would collapse onto Paper.
        Palette warm = *findId("paper");
        warm.id = "warm";
        warm.bgMain = RGB(255, 252, 240);
        warm.accent = RGB(132, 102, 13);
        const Palette quiet = *findId("paper");
        const Palette pair[2] = { warm, quiet };
        auto px = rgbaFill(16, 16, 255, 252, 240);
        const CoverColors c = coverColorsFromRgba(px.data(), 16, 16);
        assert(c.ready && !c.hasSignal);
        const int i = matchTheme(c, pair, 2);
        assert(i == 0);
        assert(pair[i].id == "warm");
    }
    assert(!coverColorsFromRgba(rgbaFill(32, 32, 255, 255, 255, 0).data(), 32, 32).ready);
    assert(!coverColorsFromRgba(rgbaFill(8, 8, 10, 10, 10, 100).data(), 8, 8).ready);
    assert(!coverColorsFromRgba(rgbaFill(3, 3, 255, 255, 255).data(), 3, 3).ready);
    assert(coverColorsFromRgba(rgbaFill(4, 4, 255, 255, 255).data(), 4, 4).ready);
    assert(!coverColorsFromRgba(nullptr, 16, 16).ready);
    assert(matchTheme(CoverColors{}, nullptr, 0) < 0);
}

static void assertCoverReadable(const Palette& p) {
    assert(paletteAcceptable(p));
    assert(themeContrast(p.textAlbum, p.bgMain) >= 4.5f);
    assert(themeContrast(p.textPrimary, p.bgMain) >= 4.5f);
    assert(themeContrast(p.textPrimary, p.bgTransport) >= 4.5f);
    assert(themeContrast(p.textDim, p.bgTransport) >= 4.5f);
    assert(themeChannelDist(p.accent, p.error) >= 48);
    const ColorRef q[4] = { p.qDsd, p.qDxd, p.qHires, p.qStandard };
    for (ColorRef c : q)
        assert(themeContrast(c, p.bgTrack) >= 3.0f);
    for (int i = 0; i < 4; ++i)
        for (int j = i + 1; j < 4; ++j)
            assert(themeChannelDist(q[i], q[j]) >= 24);
}

static void testPaletteFromCover() {
    const int before = themeCount();
    Palette untouched;
    untouched.bgMain = RGB(1, 2, 3);
    assert(!paletteFromCover(nullptr, 16, 16, untouched));
    assert(untouched.bgMain == RGB(1, 2, 3));
    auto blank = rgbaFill(3, 3, 10, 10, 10);
    assert(!paletteFromCover(blank.data(), 3, 3, untouched));
    assert(untouched.bgMain == RGB(1, 2, 3));
    auto clear = rgbaFill(32, 32, 255, 255, 255, 0);
    assert(!paletteFromCover(clear.data(), 32, 32, untouched));

    {
        // One dark red. The page is that red, and the accent stays quiet
        // because the picture named no second color.
        auto px = rgbaFill(32, 32, 90, 12, 18);
        Palette p;
        assert(paletteFromCover(px.data(), 32, 32, p));
        assertCoverReadable(p);
        assert(p.bgMain == RGB(90, 12, 18));
        assert(themeLuma(p.bgMain) <= 0.45f);
        assert(themeLuma(p.textPrimary) > themeLuma(p.bgMain));
        assert(themeAccentQuiet(p));
        assert(p.id == "cover");
        Palette again;
        assert(paletteFromCover(px.data(), 32, 32, again));
        assert(again.bgMain == p.bgMain && again.accent == p.accent
               && again.textPrimary == p.textPrimary && again.error == p.error);
    }
    {
        // Dark red field, a gold mass. The page stays red. The accent stays
        // in that gold rather than collapsing to white.
        auto px = rgbaFill(32, 32, 90, 12, 18);
        rgbaBlock(px, 32, 0, 0, 12, 12, 212, 175, 55);
        Palette p;
        assert(paletteFromCover(px.data(), 32, 32, p));
        assertCoverReadable(p);
        assert(GetRValue(p.bgMain) > GetGValue(p.bgMain) + 15);
        assert(GetRValue(p.bgMain) > GetBValue(p.bgMain) + 15);
        assert(themeLuma(p.bgMain) <= 0.45f);
        assert(!themeAccentQuiet(p));
        assert(GetRValue(p.accent) > GetBValue(p.accent));
        assert(GetGValue(p.accent) > GetBValue(p.accent));
    }
    {
        // White sleeve, black type, a blue mark. Light page, dark ink,
        // blue accent.
        auto px = rgbaFill(32, 32, 250, 250, 250);
        rgbaBlock(px, 32, 0, 0, 12, 12, 8, 8, 8);
        rgbaBlock(px, 32, 20, 20, 12, 12, 20, 60, 210);
        Palette p;
        assert(paletteFromCover(px.data(), 32, 32, p));
        assertCoverReadable(p);
        assert(themeLuma(p.bgMain) > 0.45f);
        assert(themeLuma(p.textPrimary) < themeLuma(p.bgMain));
        assert(themeLuma(p.textAlbum) < themeLuma(p.bgMain));
        assert(GetBValue(p.accent) > GetRValue(p.accent));
        assert(GetBValue(p.accent) > GetGValue(p.accent));
    }
    {
        auto px = rgbaFill(32, 32, 0, 0, 0);
        Palette p;
        assert(paletteFromCover(px.data(), 32, 32, p));
        assertCoverReadable(p);
        assert(themeAccentQuiet(p));
        assert(themeLuma(p.textPrimary) > 0.5f);
    }
    {
        auto px = rgbaFill(32, 32, 255, 255, 255);
        Palette p;
        assert(paletteFromCover(px.data(), 32, 32, p));
        assertCoverReadable(p);
        assert(themeLuma(p.bgMain) > 0.9f);
        assert(themeAccentQuiet(p));
        assert(themeLuma(p.textPrimary) < 0.2f);
        assert(themeLuma(p.textAlbum) < themeLuma(p.bgMain));
    }
    {
        // A mid gray has no room for a ladder until the page moves toward
        // black. It still has to come back readable, in that same gray.
        auto px = rgbaFill(32, 32, 128, 128, 128);
        Palette p;
        assert(paletteFromCover(px.data(), 32, 32, p));
        assertCoverReadable(p);
        const int spread = std::abs((int)GetRValue(p.bgMain) - (int)GetGValue(p.bgMain))
                         + std::abs((int)GetGValue(p.bgMain) - (int)GetBValue(p.bgMain));
        assert(spread <= 8);
        assert(themeLuma(p.textPrimary) > themeLuma(p.bgMain));
    }
    {
        // A small saturated mark is the sleeve. The large dull mass is not.
        // 144 of 4096 pixels is under the old 8% cut, which threw the mark away.
        auto px = rgbaFill(64, 64, 16, 20, 36);
        rgbaBlock(px, 64, 0, 0, 24, 64, 55, 58, 64);
        rgbaBlock(px, 64, 40, 26, 12, 12, 230, 100, 20);
        Palette p;
        assert(paletteFromCover(px.data(), 64, 64, p));
        assertCoverReadable(p);
        assert(!themeAccentQuiet(p));
        assert(GetRValue(p.accent) > GetGValue(p.accent));
        assert(GetRValue(p.accent) > GetBValue(p.accent));
        assert(GetBValue(p.bgMain) + 8 >= GetRValue(p.bgMain));
    }
    {
        // Two marks. The one in the middle is the accent. The other stays
        // in the mosaic fill and in the album title, instead of being averaged
        // into the accent.
        auto px = rgbaFill(64, 64, 22, 16, 30);
        rgbaBlock(px, 64, 24, 24, 16, 16, 230, 110, 30);
        rgbaBlock(px, 64, 0, 0, 16, 16, 20, 170, 160);
        Palette p;
        assert(paletteFromCover(px.data(), 64, 64, p));
        assertCoverReadable(p);
        assert(GetRValue(p.accent) > GetGValue(p.accent));
        assert(GetGValue(p.accent) > GetBValue(p.accent));
        assert(GetGValue(p.tileMore) > GetRValue(p.tileMore));
        assert(GetBValue(p.tileMore) > GetRValue(p.tileMore));
        assert(channelSpread(p.textAlbum) >= 24);
        assert(themeChannelDist(p.textAlbum, p.textPrimary) >= 20);
        assert(themeChannelDist(p.accent, p.tileMore) >= 40);
        Palette again;
        assert(paletteFromCover(px.data(), 64, 64, again));
        assert(again.accent == p.accent && again.tileMore == p.tileMore
               && again.textAlbum == p.textAlbum && again.bgTrack == p.bgTrack);
    }
    {
        // Cream type stays cream. Pushing it to pure white would pass the
        // contrast rules and still be the wrong sleeve.
        auto px = rgbaFill(64, 64, 28, 20, 16);
        rgbaBlock(px, 64, 44, 0, 20, 64, 236, 220, 196);
        Palette p;
        assert(paletteFromCover(px.data(), 64, 64, p));
        assertCoverReadable(p);
        assert(GetRValue(p.textPrimary) >= GetBValue(p.textPrimary) + 8);
        assert(GetGValue(p.textPrimary) >= GetBValue(p.textPrimary) + 4);
    }
    {
        // Scattered single pixels are noise. The gold block is the mark.
        auto px = rgbaFill(64, 64, 18, 12, 22);
        for (int i = 0; i < 48; ++i) {
            const int x = (i * 17 + 3) % 64;
            const int y = (i * 13 + 5) % 64;
            rgbaDot(px, 64, x, y,
                    (uint8_t)(30 + (i * 37) % 200),
                    (uint8_t)(20 + (i * 19) % 180),
                    (uint8_t)(40 + (i * 53) % 200));
        }
        rgbaBlock(px, 64, 26, 26, 12, 12, 212, 175, 55);
        Palette p;
        assert(paletteFromCover(px.data(), 64, 64, p));
        assertCoverReadable(p);
        assert(GetRValue(p.accent) > GetBValue(p.accent) + 20);
        assert(GetGValue(p.accent) > GetBValue(p.accent) + 20);
    }
    {
        // The blue figure is state. The red in the picture is the error,
        // and it is this red, not the house pink (equal green and blue).
        auto px = rgbaFill(64, 64, 14, 16, 28);
        rgbaBlock(px, 64, 24, 22, 16, 16, 30, 70, 220);
        rgbaBlock(px, 64, 2, 40, 14, 14, 190, 15, 90);
        Palette p;
        assert(paletteFromCover(px.data(), 64, 64, p));
        assertCoverReadable(p);
        assert(GetBValue(p.accent) > GetRValue(p.accent));
        assert(GetBValue(p.accent) > GetGValue(p.accent));
        assert(GetRValue(p.error) > GetGValue(p.error) + 30);
        assert(GetBValue(p.error) > GetGValue(p.error) + 20);
    }
    {
        // A sand panel beside a navy field shows up on the track page.
        // The page itself stays the navy.
        auto px = rgbaFill(64, 64, 10, 20, 48);
        rgbaBlock(px, 64, 44, 0, 20, 64, 200, 168, 120);
        Palette p;
        assert(paletteFromCover(px.data(), 64, 64, p));
        assertCoverReadable(p);
        assert(GetBValue(p.bgMain) > GetRValue(p.bgMain) + 10);
        const int pageLean = (int)GetRValue(p.bgMain) - (int)GetBValue(p.bgMain);
        const int trackLean = (int)GetRValue(p.bgTrack) - (int)GetBValue(p.bgTrack);
        assert(trackLean > pageLean + 8);
    }
    {
        auto gold = rgbaFill(64, 64, 20, 48, 32);
        rgbaBlock(gold, 64, 24, 24, 14, 14, 220, 180, 40);
        auto violet = rgbaFill(64, 64, 20, 48, 32);
        rgbaBlock(violet, 64, 24, 24, 14, 14, 160, 60, 220);
        Palette a, b;
        assert(paletteFromCover(gold.data(), 64, 64, a));
        assert(paletteFromCover(violet.data(), 64, 64, b));
        assertCoverReadable(a);
        assertCoverReadable(b);
        assert(GetRValue(a.accent) > GetBValue(a.accent));
        assert(GetGValue(a.accent) > GetBValue(a.accent));
        assert(GetBValue(b.accent) > GetRValue(b.accent));
        assert(GetBValue(b.accent) > GetGValue(b.accent));
        assert(themeChannelDist(a.accent, b.accent) >= 40);
    }
    {
        // A small red on black is the mark. A larger night blue on that
        // same black is the field: it is there, and the eye does not keep
        // it as its own color. The accent is the red.
        auto px = rgbaFill(64, 64, 0, 0, 0);
        rgbaBlock(px, 64, 0, 0, 24, 24, 18, 26, 64);
        rgbaBlock(px, 64, 28, 28, 8, 8, 200, 30, 30);
        Palette p;
        assert(paletteFromCover(px.data(), 64, 64, p));
        assertCoverReadable(p);
        assert(themeLuma(p.bgMain) < 0.08f);
        assert(!themeAccentQuiet(p));
        assert(GetRValue(p.accent) > GetGValue(p.accent) + 40);
        assert(GetRValue(p.accent) > GetBValue(p.accent) + 40);
    }
    {
        // The night blue alone does not invent an accent. Nothing in the
        // picture separates from the black.
        auto px = rgbaFill(64, 64, 0, 0, 0);
        rgbaBlock(px, 64, 8, 8, 20, 20, 18, 26, 64);
        Palette p;
        assert(paletteFromCover(px.data(), 64, 64, p));
        assertCoverReadable(p);
        assert(themeAccentQuiet(p));
        assert(themeLuma(p.bgMain) < 0.08f);
    }
    {
        // A blue that actually leaves the black is a mark. This is not
        // the night blue above.
        auto px = rgbaFill(64, 64, 0, 0, 0);
        rgbaBlock(px, 64, 24, 24, 16, 16, 40, 70, 200);
        Palette p;
        assert(paletteFromCover(px.data(), 64, 64, p));
        assertCoverReadable(p);
        assert(!themeAccentQuiet(p));
        assert(GetBValue(p.accent) > GetRValue(p.accent));
        assert(GetBValue(p.accent) > GetGValue(p.accent));
    }
    {
        // Three pigments stay three. The accent, the mosaic and the album
        // title each keep a hue, instead of the second and third washing
        // back into the page.
        auto px = rgbaFill(64, 64, 18, 14, 22);
        rgbaBlock(px, 64, 24, 24, 16, 16, 230, 110, 30);
        rgbaBlock(px, 64, 0, 0, 14, 14, 20, 170, 160);
        rgbaBlock(px, 64, 48, 46, 14, 14, 160, 60, 220);
        Palette p;
        assert(paletteFromCover(px.data(), 64, 64, p));
        assertCoverReadable(p);
        assert(GetRValue(p.accent) > GetGValue(p.accent));
        assert(GetGValue(p.accent) > GetBValue(p.accent));
        assert(channelSpread(p.tileMore) >= 24);
        assert(channelSpread(p.textAlbum) >= 24);
        assert(themeChannelDist(p.accent, p.tileMore) >= 36);
        assert(themeChannelDist(p.accent, p.textAlbum) >= 36);
        assert(themeChannelDist(p.tileMore, p.textAlbum) >= 36);
    }

    uint32_t slots[20];
    Palette round;
    round.bgMain = RGB(1, 2, 3);
    paletteWriteSlots(*findId("matrix"), slots);
    paletteReadSlots(slots, round);
    const Palette* matrix = findId("matrix");
    assert(round.bgMain == matrix->bgMain);
    assert(round.accent == matrix->accent);
    assert(round.qHires == matrix->qHires);
    assert(round.textDim == matrix->textDim);
    assert(themeCount() == before);
}

static void testChooseTheme() {
    expectId(chooseTheme(ThemeMode::Static, true, true, "nord", "paper", "hyper"), "paper");
    expectId(chooseTheme(ThemeMode::Static, false, false, "nord", "", "hyper"), "matrix");
    expectId(chooseTheme(ThemeMode::Static, true, true, "nord", "missing", "hyper"), "matrix");
    expectId(chooseTheme(ThemeMode::Static, false, false, nullptr, nullptr, nullptr), "matrix");
    expectId(chooseTheme(ThemeMode::Dynamic, false, true, "nord", "paper", "hyper"), "matrix");
    expectId(chooseTheme(ThemeMode::Dynamic, true, true, "nord", "paper", "hyper"), "nord");
    expectId(chooseTheme(ThemeMode::Dynamic, true, true, "missing", "paper", "hyper"), "matrix");
    expectId(chooseTheme(ThemeMode::Dynamic, true, false, "nord", "paper", "hyper"), "hyper");
    expectId(chooseTheme(ThemeMode::Dynamic, true, false, "nord", "paper", ""), "matrix");
    expectId(chooseTheme(ThemeMode::Dynamic, true, false, "nord", "paper", "missing"), "matrix");
}

static void testCopyThemes() {
    namespace fs = std::filesystem;
    const fs::path src = fs::temp_directory_path() / "matrix-player-theme-copy-src";
    const fs::path dst = fs::temp_directory_path() / "matrix-player-theme-copy-dst";
    fs::remove_all(src);
    fs::remove_all(dst);
    fs::create_directories(src / "sub");
    {
        std::ofstream((src / "a.toml").string()) << "id = a\n";
        std::ofstream((src / "B.THEME").string()) << "id = b\n";
        std::ofstream((src / "notes.txt").string()) << "no\n";
        std::ofstream((src / "sub" / "skip.toml").string()) << "id = skip\n";
    }
    const ThemeCopyResult first = copyThemeFiles(src.string(), dst.string());
    assert(first.copied == 2);
    assert(first.already == 0);
    assert(fs::is_regular_file(dst / "a.toml"));
    assert(fs::is_regular_file(dst / "B.THEME"));
    assert(!fs::exists(dst / "notes.txt"));
    assert(!fs::exists(dst / "skip.toml"));
    assert(!fs::exists(dst / "sub"));
    const ThemeCopyResult second = copyThemeFiles(src.string(), dst.string());
    assert(second.copied == 0);
    assert(second.already == 2);
    fs::remove_all(src);
    fs::remove_all(dst);
}

int main() {
    checkBuiltinThemes();
    assert(themeCount() == 24);
    assertContiguousGroups();

    const Palette* matrix = findId("matrix");
    const Palette* paper = findId("paper");
    const Palette* hyper = findId("hyper");
    const Palette* nord = findId("nord");
    const Palette* cat = findId("catppuccin");
    const Palette* tokyo = findId("tokyo-night");
    const Palette* rose = findId("rose-pine");
    const Palette* kana = findId("kanagawa");
    assert(matrix && paper && hyper && nord && cat && tokyo && rose && kana);
    assert(matrix->bgMain == RGB(10, 10, 10));
    assert(matrix->accent == RGB(0, 200, 83));
    assert(paper->bgMain == RGB(255, 255, 255));
    assert(hyper->accent == RGB(0, 255, 255));
    assert(nord->bgMain == RGB(46, 52, 64));
    assert(nord->accent == RGB(136, 192, 208));
    assert(cat->accent == RGB(203, 166, 247));
    assert(tokyo->accent == RGB(122, 162, 247));
    assert(rose->accent == RGB(235, 111, 146));
    assert(rose->error == RGB(255, 110, 110));
    assert(kana->accent == RGB(127, 180, 202));

    // The original eight ids are still there, so a saved ui_theme resolves.
    for (const char* id : { "matrix", "ink", "paper", "nordic", "dracula",
                            "pink", "hyper", "chicago95" })
        assert(findId(id) != nullptr);

    assert(applyTheme("matrix"));
    assert(themeRule() == CLR_SEPARATOR);
    assert(themeButtonFill(true, false) == CLR_ACCENT);
    assert(applyTheme("paper"));
    assert(themeRule() == CLR_SEPARATOR);
    assert(applyTheme("hyper"));
    assert(themeRule() == CLR_SEPARATOR);
    assert(std::string(activeThemeId()) == "hyper");

    const char* nordText =
        "# Colors (Nord)\n"
        "[colors.primary]\n"
        "background = '#2E3440'\n"
        "foreground = '#D8DEE9'\n"
        "[colors.normal]\n"
        "black   = '#3B4252'\n"
        "red     = '#BF616A'\n"
        "green   = '#A3BE8C'\n"
        "yellow  = '#EBCB8B'\n"
        "blue    = '#81A1C1'\n"
        "magenta = '#B48EAD'\n"
        "cyan    = '#88C0D0'\n"
        "white   = '#E5E9F0'\n"
        "[colors.bright]\n"
        "black   = '#4C566A'\n"
        "red     = '#BF616A'\n"
        "green   = '#A3BE8C'\n"
        "yellow  = '#EBCB8B'\n"
        "blue    = '#81A1C1'\n"
        "magenta = '#B48EAD'\n"
        "cyan    = '#8FBCBB'\n"
        "white   = '#ECEFF4'\n";
    Palette parsed;
    std::string err;
    if (!parseThemeText(nordText, "nord-file", parsed, err)) {
        fprintf(stderr, "nord: %s\n", err.c_str());
        assert(false);
    }
    assert(parsed.id == "nord-file");
    assert(parsed.name == "Nord");
    assert(parsed.group == "Imported");
    assert(parsed.bgMain == RGB(46, 52, 64));
    assert(parsed.accent == RGB(136, 192, 208));
    assert(paletteAcceptable(parsed));

    assert(!parseThemeText("this is not a theme\n", "x", parsed, err));

    const char* direct =
        "id = custom-ink\n"
        "name = Custom Ink\n"
        "group = Imported\n"
        "blurb = A direct file.\n"
        "bg = #0a0a0a\n"
        "text = #f2f2f2\n"
        "accent = #00c853\n";
    if (!parseThemeText(direct, "custom-ink", parsed, err)) {
        fprintf(stderr, "direct: %s\n", err.c_str());
        assert(false);
    }
    assert(parsed.id == "custom-ink");
    assert(parsed.name == "Custom Ink");
    assert(parsed.blurb == "A direct file.");
    assert(parsed.bgMain == RGB(10, 10, 10));
    assert(parsed.accent == RGB(0, 200, 83));
    assert(paletteAcceptable(parsed));

    // A file that names every slot, and names them so they fail, is refused.
    // Deriving from bg/text/accent alone would have repaired it.
    const char* bad =
        "id = bad\nname = Bad\ngroup = Imported\nblurb = no\n"
        "bg = #ffffff\nbg.track = #ffffff\nbg.sidebar = #ffffff\nbg.transport = #ffffff\n"
        "text = #eeeeee\ntext.secondary = #eeeeee\ntext.dim = #eeeeee\ntext.album = #eeeeee\n"
        "accent = #dddddd\nhover = #f0f0f0\nseparator = #f8f8f8\n"
        "input = #ffffff\nplaceholder = #ffffff\ntile = #cccccc\n"
        "error = #eeeeee\nwarning = #eeeeee\n"
        "quality.dsd = #ffffff\nquality.dxd = #ffffff\n"
        "quality.hires = #ffffff\nquality.standard = #ffffff\n";
    assert(!parseThemeText(bad, "bad", parsed, err));

    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "matrix-player-theme-test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        std::ofstream((dir / "matrix.theme").string())
            << "id = matrix\nname = Fake\nbg = #000000\ntext = #ffffff\naccent = #00ff00\n";
        std::ofstream((dir / "hello.theme").string())
            << "id = hello-theme\nname = Hello\ngroup = Imported\n"
               "blurb = Hi.\nbg = #111111\ntext = #eeeeee\naccent = #33cc77\n";
        std::ofstream((dir / "again.theme").string())
            << "id = hello-theme\nname = Second\nbg = #111111\ntext = #eeeeee\naccent = #33cc77\n";
        std::ofstream((dir / "aaa-night.theme").string())
            << "id = aaa-night\nname = Extra Night\ngroup = Night\n"
               "blurb = Joins the night group.\nbg = #101820\ntext = #e8eef8\naccent = #70c0d0\n";
        std::ofstream((dir / "garbage.theme").string()) << "not a theme at all\n";
    }

    const int added = reloadImportedThemes(dir.string());
    assert(added == 2);   // hello-theme and aaa-night; matrix and the duplicate skipped
    assert(themeSkipCount() == 3);   // matrix (built-in), again (duplicate), garbage
    assert(themeCount() == 26);
    const Palette* still = findId("matrix");
    assert(still && still->name == "Matrix");
    assert(GetRValue(still->bgMain) == 10);
    assert(findId("hello-theme") != nullptr);
    assert(findId("aaa-night") != nullptr);
    assertContiguousGroups();

    // aaa-night sorts first and joins Night, so it sits before Day.
    int nightAt = -1, dayAt = -1, helloAt = -1;
    for (int i = 0; i < themeCount(); ++i) {
        if (themeAt(i).id == "aaa-night") nightAt = i;
        if (themeAt(i).id == "catppuccin-latte") dayAt = i;
        if (themeAt(i).id == "hello-theme") helloAt = i;
    }
    assert(nightAt >= 0 && dayAt > nightAt);
    assert(helloAt == themeCount() - 1);

    assert(applyTheme("paper"));
    assert(reloadImportedThemes(dir.string()) == 2);
    assert(themeSkipCount() == 3);
    assert(std::string(activeThemeId()) == "paper");
    assert(activeThemeIndex() >= 0);
    assert(themeAt(activeThemeIndex()).id == "paper");

    assert(applyTheme("hello-theme"));
    const ColorRef keptBg = CLR_BG_MAIN;
    const ColorRef keptAccent = CLR_ACCENT;
    fs::remove(dir / "hello.theme");
    fs::remove(dir / "again.theme");
    fs::remove(dir / "aaa-night.theme");
    assert(reloadImportedThemes(dir.string()) == 0);
    assert(themeSkipCount() == 2);   // matrix.theme and garbage.theme are still there
    assert(activeThemeIndex() < 0);
    assert(std::string(activeThemeId()) == "hello-theme");
    // The applied tokens stay. Nothing in the list is highlighted, and the
    // page does not snap back to Matrix just because the file went away.
    assert(CLR_BG_MAIN == keptBg);
    assert(CLR_ACCENT == keptAccent);

    fs::remove_all(dir);
    testDisplayOrder();
    testDayparts();
    testThemeLayout();
    testCoverMatch();
    testPaletteFromCover();
    testChooseTheme();
    testCopyThemes();
    applyTheme("matrix");
    return 0;
}

#include "theme.hh"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>

// The palette catalogue.
//
// Built-ins are frozen literals. The first eight are the palettes this app
// shipped with, byte for byte, so a saved ui_theme still names the same page.
// The rest were derived once from Alacritty themes (REFERENCEonly/alacritty-theme)
// and then frozen: a theme that changes because a generator was re-run is a
// theme a listener cannot trust. Four of those accents were put back to the
// palette's own signature color afterwards — Catppuccin mauve, Rosé Pine's
// rose, Tokyo Night's blue, Kanagawa's cyan — because the contest that picks
// an accent would otherwise have chosen a different hue.
//
// A file in the themes directory is derived the same way at load, then kept
// only if paletteAcceptable() agrees. UI thread only.

namespace {

constexpr ColorRef kQnD = RGB(255, 255, 255);
constexpr ColorRef kQnX = RGB(255, 165, 0);
constexpr ColorRef kQnH = RGB(0, 255, 255);
constexpr ColorRef kQnS = RGB(255, 255, 0);
constexpr ColorRef kQpD = RGB(20, 20, 20);
constexpr ColorRef kQpX = RGB(140, 70, 0);
constexpr ColorRef kQpH = RGB(0, 105, 115);
constexpr ColorRef kQpS = RGB(120, 96, 0);

// Field order of c[]: bg, track, side, transport, primary, secondary, dim,
// album, accent, hover, separator, input, placeholder, tile, error, warning,
// qDsd, qDxd, qHires, qStandard.
struct Builtin {
    const char* id;
    const char* group;
    const char* name;
    const char* blurb;
    ColorRef    c[20];
};

const Builtin kBuiltins[] = {
    { "matrix", "Monochrome", "Matrix",
      "Near-black pages. The green mark means playing or selected, and nothing else.",
      { RGB(10,10,10), RGB(14,14,14), RGB(18,18,18), RGB(22,22,22),
        RGB(242,242,242), RGB(170,170,170), RGB(128,128,128), RGB(255,255,255),
        RGB(0,200,83), RGB(38,38,38), RGB(36,36,36), RGB(24,24,24),
        RGB(28,28,28), RGB(30,104,62), RGB(255,110,110), RGB(224,180,40),
        kQnD, kQnX, kQnH, kQnS } },

    { "ink", "Monochrome", "Ink",
      "Black and white, mostly black. Two inks, and no second color.",
      { RGB(0,0,0), RGB(0,0,0), RGB(8,8,8), RGB(16,16,16),
        RGB(245,245,245), RGB(190,190,190), RGB(160,160,160), RGB(255,255,255),
        RGB(255,255,255), RGB(28,28,28), RGB(42,42,42), RGB(12,12,12),
        RGB(22,22,22), RGB(96,96,96), RGB(255,110,110), RGB(224,180,40),
        kQnD, kQnX, kQnH, kQnS } },

    { "paper", "Monochrome", "Paper",
      "The same two inks, turned over. A white page and black type.",
      { RGB(255,255,255), RGB(250,250,250), RGB(244,244,244), RGB(236,236,236),
        RGB(16,16,16), RGB(70,70,70), RGB(96,96,96), RGB(0,0,0),
        RGB(0,0,0), RGB(230,230,230), RGB(210,210,210), RGB(248,248,248),
        RGB(232,232,232), RGB(160,160,160), RGB(153,27,27), RGB(122,78,0),
        RGB(16,16,16), RGB(140,70,0), RGB(0,105,115), RGB(120,96,0) } },

    { "nordic", "Night", "Nordic",
      "Cool blue-gray. Quiet, and the frost blue is the only mark of state.",
      { RGB(36,41,51), RGB(30,34,43), RGB(43,49,61), RGB(32,37,46),
        RGB(187,189,175), RGB(168,172,164), RGB(158,164,158), RGB(220,224,216),
        RGB(143,180,216), RGB(48,53,63), RGB(58,64,76), RGB(30,35,45),
        RGB(40,45,55), RGB(70,110,90), RGB(255,110,110), RGB(224,180,40),
        kQnD, kQnX, kQnH, kQnS } },

    { "nord", "Night", "Nord",
      "The Nord palette. A blue-gray page, and frost is the only mark of state.",
      { RGB(46,52,64), RGB(33,37,46), RGB(54,60,72), RGB(61,66,77),
        RGB(225,230,238), RGB(195,200,208), RGB(170,175,184), RGB(255,255,255),
        RGB(136,192,208), RGB(69,74,85), RGB(84,89,98), RGB(56,62,74),
        RGB(63,68,79), RGB(86,115,129), RGB(216,160,165), RGB(235,203,139),
        kQnD, kQnX, kQnH, kQnS } },

    { "dracula", "Night", "Dracula",
      "A purple night. Violet means selected, and the page stays blue-black.",
      { RGB(40,42,54), RGB(33,35,46), RGB(46,48,62), RGB(46,48,62),
        RGB(248,248,242), RGB(198,200,206), RGB(176,180,196), RGB(255,255,250),
        RGB(189,147,249), RGB(56,58,74), RGB(68,71,90), RGB(36,38,50),
        RGB(50,52,68), RGB(80,80,96), RGB(255,110,110), RGB(224,180,40),
        kQnD, kQnX, kQnH, kQnS } },

    { "pink", "Night", "Pink",
      "Navy page, rose type. The warm one beside Nordic and Dracula.",
      { RGB(32,35,48), RGB(26,28,40), RGB(38,42,58), RGB(44,48,64),
        RGB(255,240,245), RGB(220,190,200), RGB(190,160,170), RGB(255,240,245),
        RGB(255,128,168), RGB(50,42,58), RGB(70,60,78), RGB(28,30,44),
        RGB(42,36,52), RGB(120,80,100), RGB(255,110,110), RGB(224,180,40),
        kQnD, kQnX, kQnH, kQnS } },

    { "catppuccin", "Night", "Catppuccin",
      "Catppuccin Mocha. A warm dark page, and mauve means selected.",
      { RGB(30,30,46), RGB(22,22,33), RGB(39,39,54), RGB(46,46,61),
        RGB(205,214,244), RGB(174,181,208), RGB(144,149,174), RGB(246,248,253),
        RGB(203,166,247), RGB(55,55,69), RGB(70,70,84), RGB(41,41,56),
        RGB(48,48,63), RGB(108,91,136), RGB(243,139,168), RGB(249,226,175),
        kQnD, kQnX, kQnH, kQnS } },

    { "tokyo-night", "Night", "Tokyo Night",
      "Tokyo Night. Deep blue, and a brighter blue for what is playing.",
      { RGB(26,27,38), RGB(19,19,27), RGB(35,36,47), RGB(42,43,53),
        RGB(188,195,223), RGB(162,168,193), RGB(140,145,168), RGB(224,227,240),
        RGB(122,162,247), RGB(51,52,62), RGB(67,68,77), RGB(37,38,49),
        RGB(44,45,55), RGB(69,88,132), RGB(247,118,142), RGB(224,175,104),
        kQnD, kQnX, kQnH, kQnS } },

    { "rose-pine", "Night", "Rose Pine",
      "Rose Pine. A muted night, and rose means selected.",
      { RGB(25,23,36), RGB(18,17,26), RGB(34,32,45), RGB(41,39,51),
        RGB(224,222,244), RGB(174,172,192), RGB(143,141,159), RGB(255,255,255),
        RGB(235,111,146), RGB(50,49,60), RGB(66,65,75), RGB(36,35,47),
        RGB(43,42,54), RGB(119,63,86), RGB(255,110,110), RGB(240,148,134),
        kQnD, kQnX, kQnH, kQnS } },

    { "kanagawa", "Night", "Kanagawa",
      "Kanagawa Wave. Ink blue, with the wave's own cyan as state.",
      { RGB(31,31,40), RGB(22,22,29), RGB(40,40,49), RGB(47,47,55),
        RGB(220,215,186), RGB(186,182,160), RGB(155,152,136), RGB(249,248,242),
        RGB(127,180,202), RGB(56,56,64), RGB(71,71,79), RGB(42,42,51),
        RGB(49,49,57), RGB(74,98,113), RGB(215,127,129), RGB(192,163,110),
        kQnD, kQnX, kQnH, kQnS } },

    { "everforest", "Night", "Everforest",
      "Everforest. A green-gray night, quiet on purpose.",
      { RGB(45,53,59), RGB(32,38,42), RGB(53,61,67), RGB(60,67,73),
        RGB(235,229,216), RGB(203,199,189), RGB(176,174,167), RGB(255,255,255),
        RGB(167,192,128), RGB(68,75,81), RGB(83,89,94), RGB(56,63,69),
        RGB(62,69,75), RGB(100,116,90), RGB(235,153,154), RGB(219,188,127),
        kQnD, kQnX, kQnH, kQnS } },

    { "night-owl", "Night", "Night Owl",
      "Night Owl. A deep blue made for a long session.",
      { RGB(1,22,39), RGB(1,16,28), RGB(11,31,48), RGB(19,38,54),
        RGB(214,222,235), RGB(161,172,186), RGB(127,140,155), RGB(248,250,252),
        RGB(33,199,168), RGB(29,48,63), RGB(47,64,78), RGB(14,34,50),
        RGB(21,41,56), RGB(15,102,97), RGB(239,88,85), RGB(197,228,120),
        kQnD, kQnX, kQnH, kQnS } },

    { "catppuccin-latte", "Day", "Catppuccin Latte",
      "Catppuccin Latte. The same hues as Mocha, on a light page.",
      { RGB(239,241,245), RGB(234,236,240), RGB(229,231,235), RGB(223,225,229),
        RGB(60,62,82), RGB(81,83,102), RGB(97,99,116), RGB(43,44,59),
        RGB(27,90,217), RGB(220,222,225), RGB(201,202,206), RGB(231,233,236),
        RGB(222,224,228), RGB(144,173,232), RGB(198,14,54), RGB(139,88,18),
        kQpD, kQpX, kQpH, kQpS } },

    { "gruvbox-light", "Day", "Gruvbox Light",
      "Gruvbox turned over. Warm paper and the same earth colors.",
      { RGB(251,241,199), RGB(246,236,195), RGB(241,231,191), RGB(235,225,186),
        RGB(60,56,54), RGB(79,74,68), RGB(105,99,88), RGB(41,38,37),
        RGB(7,102,120), RGB(231,222,183), RGB(211,202,167), RGB(242,233,192),
        RGB(233,224,185), RGB(141,178,163), RGB(193,34,28), RGB(130,92,20),
        kQpD, kQpX, kQpH, kQpS } },

    { "rose-pine-dawn", "Day", "Rose Pine Dawn",
      "Rose Pine Dawn. The night palette turned toward morning.",
      { RGB(250,244,237), RGB(245,239,232), RGB(240,234,228), RGB(234,228,222),
        RGB(66,62,92), RGB(88,84,109), RGB(104,100,122), RGB(48,45,68),
        RGB(40,105,131), RGB(230,224,218), RGB(210,205,199), RGB(241,235,229),
        RGB(232,227,220), RGB(156,181,189), RGB(148,82,101), RGB(138,92,31),
        kQpD, kQpX, kQpH, kQpS } },

    { "solarized-light", "Day", "Solarized Light",
      "Solarized light. Cream paper, and the type still clears.",
      { RGB(253,246,227), RGB(248,241,222), RGB(243,236,218), RGB(237,230,212),
        RGB(55,68,73), RGB(79,89,91), RGB(96,105,105), RGB(40,50,54),
        RGB(95,109,0), RGB(233,226,209), RGB(213,207,191), RGB(244,237,219),
        RGB(235,229,211), RGB(182,184,125), RGB(194,44,42), RGB(169,75,22),
        kQpD, kQpX, kQpH, kQpS } },

    { "flexoki-light", "Day", "Flexoki Light",
      "Flexoki light. Warm paper and dark ink.",
      { RGB(255,252,240), RGB(250,247,235), RGB(245,242,230), RGB(238,236,224),
        RGB(16,15,15), RGB(81,79,76), RGB(107,105,101), RGB(16,15,15),
        RGB(132,102,13), RGB(235,232,221), RGB(214,212,202), RGB(246,243,232),
        RGB(237,234,223), RGB(200,184,138), RGB(183,67,57), RGB(159,87,34),
        kQpD, kQpX, kQpH, kQpS } },

    { "hyper", "Terminal", "Hyper",
      "Pure black and full-brightness cyan. Nothing here is muted.",
      { RGB(0,0,0), RGB(0,0,0), RGB(8,8,8), RGB(12,12,12),
        RGB(255,255,255), RGB(208,208,208), RGB(170,170,170), RGB(255,255,255),
        RGB(0,255,255), RGB(24,24,24), RGB(40,40,40), RGB(12,12,12),
        RGB(20,20,20), RGB(70,70,70), RGB(255,110,110), RGB(224,180,40),
        kQnD, kQnX, kQnH, kQnS } },

    { "chicago95", "Terminal", "Chicago95",
      "The same black screen as Hyper, in the 1995 sixteen colors. Grayer type, and smaller steps between them.",
      { RGB(0,0,0), RGB(0,0,0), RGB(20,20,20), RGB(32,32,32),
        RGB(192,199,200), RGB(176,182,184), RGB(150,150,150), RGB(255,255,255),
        RGB(0,168,168), RGB(48,48,48), RGB(84,84,84), RGB(16,16,16),
        RGB(40,40,40), RGB(90,90,90), RGB(255,110,110), RGB(224,180,40),
        kQnD, kQnX, kQnH, kQnS } },

    { "gruvbox", "Terminal", "Gruvbox",
      "Gruvbox. Warm black and the retro earth colors.",
      { RGB(40,40,40), RGB(29,29,29), RGB(49,49,49), RGB(55,55,55),
        RGB(235,219,178), RGB(204,190,156), RGB(171,160,133), RGB(255,255,255),
        RGB(184,187,38), RGB(64,64,64), RGB(79,79,79), RGB(51,51,51),
        RGB(57,57,57), RGB(105,106,39), RGB(227,135,131), RGB(215,153,33),
        kQnD, kQnX, kQnH, kQnS } },

    { "solarized", "Terminal", "Solarized",
      "Solarized dark. The low-contrast pair, lifted just enough to stay readable.",
      { RGB(0,43,54), RGB(0,31,39), RGB(10,51,62), RGB(18,58,68),
        RGB(203,211,211), RGB(168,182,184), RGB(143,161,164), RGB(241,244,244),
        RGB(149,167,34), RGB(28,66,76), RGB(46,81,90), RGB(13,54,64),
        RGB(20,60,70), RGB(67,99,45), RGB(233,127,126), RGB(191,154,36),
        kQnD, kQnX, kQnH, kQnS } },

    { "flexoki", "Terminal", "Flexoki",
      "Flexoki. A warm dark page and ink-like type.",
      { RGB(40,39,38), RGB(29,28,27), RGB(49,48,47), RGB(55,54,53),
        RGB(255,252,240), RGB(193,190,181), RGB(161,158,151), RGB(255,255,255),
        RGB(208,162,21), RGB(64,63,62), RGB(79,78,77), RGB(51,50,49),
        RGB(57,56,55), RGB(116,94,30), RGB(211,141,137), RGB(189,156,51),
        kQnD, kQnX, kQnH, kQnS } },

    { "synthwave", "Terminal", "Synthwave",
      "Synthwave. Black, magenta, and a neon cyan for state.",
      { RGB(38,35,53), RGB(27,25,38), RGB(47,44,61), RGB(53,50,67),
        RGB(255,255,255), RGB(188,187,192), RGB(156,155,163), RGB(255,255,255),
        RGB(3,237,249), RGB(62,59,75), RGB(77,75,89), RGB(49,46,63),
        RGB(55,53,69), RGB(22,126,141), RGB(254,110,119), RGB(243,231,15),
        kQnD, kQnX, kQnH, kQnS } },
};

constexpr int kBuiltinCount = (int)(sizeof(kBuiltins) / sizeof(kBuiltins[0]));

Palette fromBuiltin(const Builtin& b) {
    Palette p;
    p.id = b.id;
    p.group = b.group;
    p.name = b.name;
    p.blurb = b.blurb;
    p.bgMain = b.c[0];
    p.bgTrack = b.c[1];
    p.bgSidebar = b.c[2];
    p.bgTransport = b.c[3];
    p.textPrimary = b.c[4];
    p.textSecondary = b.c[5];
    p.textDim = b.c[6];
    p.textAlbum = b.c[7];
    p.accent = b.c[8];
    p.hover = b.c[9];
    p.separator = b.c[10];
    p.inputBg = b.c[11];
    p.placeholder = b.c[12];
    p.tileMore = b.c[13];
    p.error = b.c[14];
    p.warning = b.c[15];
    p.qDsd = b.c[16];
    p.qDxd = b.c[17];
    p.qHires = b.c[18];
    p.qStandard = b.c[19];
    return p;
}

bool isBuiltinId(const std::string& id) {
    for (const Builtin& b : kBuiltins)
        if (id == b.id) return true;
    return false;
}

std::vector<Palette> gCatalog;
std::string          gActiveId = "matrix";
int                  gActiveIndex = 0;
bool                 gBuilt = false;
int                  gThemeSkips = 0;

void resetToBuiltins() {
    gCatalog.clear();
    gCatalog.reserve((size_t)kBuiltinCount + 8);
    for (const Builtin& b : kBuiltins)
        gCatalog.push_back(fromBuiltin(b));
    gBuilt = true;
}

void ensureBuilt() {
    if (gBuilt) return;
    resetToBuiltins();
    gActiveId = "matrix";
    gActiveIndex = 0;
}

void reindexActive() {
    gActiveIndex = -1;
    for (int i = 0; i < (int)gCatalog.size(); ++i) {
        if (gCatalog[(size_t)i].id == gActiveId) {
            gActiveIndex = i;
            return;
        }
    }
}

// ── Deriving a palette from a terminal theme ────────────────────────────────
// Same steps the frozen palettes were built with. Double, not float: the
// binary search that walks a color to a contrast target is sensitive to the
// width of the intermediate, and this is the width it was tuned at.

struct Rgb { int r = 0, g = 0, b = 0; };

ColorRef pack(Rgb c) {
    auto b = [](int v) {
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        return (uint8_t)v;
    };
    return RGB(b(c.r), b(c.g), b(c.b));
}

double chanD(int c) {
    const double s = c / 255.0;
    return s <= 0.04045 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
}
double lumaD(Rgb c) {
    return 0.2126 * chanD(c.r) + 0.7152 * chanD(c.g) + 0.0722 * chanD(c.b);
}
double contrastD(Rgb a, Rgb b) {
    const double l1 = lumaD(a), l2 = lumaD(b);
    const double hi = std::max(l1, l2), lo = std::min(l1, l2);
    return (hi + 0.05) / (lo + 0.05);
}
Rgb mixD(Rgb a, Rgb b, double t) {
    auto ch = [&](int ca, int cb) {
        int v = (int)std::lround((double)ca + ((double)cb - (double)ca) * t);
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        return v;
    };
    return { ch(a.r, b.r), ch(a.g, b.g), ch(a.b, b.b) };
}
int distD(Rgb a, Rgb b) {
    return std::abs(a.r - b.r) + std::abs(a.g - b.g) + std::abs(a.b - b.b);
}
double satD(Rgb c) {
    const int mx = std::max(c.r, std::max(c.g, c.b));
    const int mn = std::min(c.r, std::min(c.g, c.b));
    if (mx == 0) return 0.0;
    return (double)(mx - mn) / (double)mx;
}

Rgb pushUntil(Rgb start, Rgb dest, Rgb bg, Rgb bg2, double need) {
    if (contrastD(start, bg) >= need && contrastD(start, bg2) >= need)
        return start;
    Rgb best = dest;
    double lo = 0.0, hi = 1.0;
    for (int i = 0; i < 28; ++i) {
        const double mid = (lo + hi) / 2.0;
        const Rgb c = mixD(start, dest, mid);
        if (contrastD(c, bg) >= need && contrastD(c, bg2) >= need) {
            best = c;
            hi = mid;
        } else {
            lo = mid;
        }
    }
    if (contrastD(best, bg) >= need && contrastD(best, bg2) >= need)
        return best;
    return dest;
}

Rgb mixToContrast(Rgb src, Rgb toward, Rgb bg, Rgb bg2, double target) {
    Rgb best = src;
    double bestErr = std::fabs(contrastD(src, bg) - target);
    for (int i = 0; i <= 100; ++i) {
        const Rgb c = mixD(src, toward, i / 100.0);
        if (contrastD(c, bg) < 4.5 || contrastD(c, bg2) < 4.5) continue;
        const double err = std::fabs(contrastD(c, bg) - target);
        if (err < bestErr - 1e-6) {
            best = c;
            bestErr = err;
        }
    }
    return best;
}

struct Ansi {
    bool hasBg = false, hasFg = false;
    Rgb  bg{}, fg{};
    Rgb  normal[8]{};
    bool hasN[8]{};
    Rgb  bright[8]{};
    bool hasB[8]{};
};

int ansiIndex(const std::string& key) {
    static const char* kNames[] = {
        "black", "red", "green", "yellow", "blue", "magenta", "cyan", "white"
    };
    for (int i = 0; i < 8; ++i)
        if (key == kNames[i]) return i;
    return -1;
}

bool fillFromAnsi(const Ansi& src, bool forceAccent, Rgb forced,
                  Palette& out, std::string& error) {
    if (!src.hasBg || !src.hasFg) {
        error = "needs a background and a foreground";
        return false;
    }
    const bool light = lumaD(src.bg) > 0.45;
    const Rgb ink = light ? Rgb{0, 0, 0} : Rgb{255, 255, 255};
    const Rgb paper = light ? Rgb{255, 255, 255} : Rgb{0, 0, 0};

    Rgb bgMain = src.bg;
    Rgb bgTrack, bgSide, bgTrans;
    if (light) {
        bgTrack = mixD(src.bg, ink, 0.02);
        bgSide  = mixD(src.bg, ink, 0.04);
        bgTrans = mixD(src.bg, ink, 0.065);
    } else {
        bgTrack = mixD(src.bg, paper, 0.28);
        bgSide  = mixD(src.bg, ink, 0.04);
        bgTrans = mixD(src.bg, ink, 0.07);
    }

    Rgb primary{}, secondary{}, dim{}, album{};
    auto ladder = [&](Rgb main, Rgb trans) {
        primary = src.fg;
        if (std::min(contrastD(primary, main), contrastD(primary, trans)) < 8.0)
            primary = pushUntil(src.fg, ink, main, trans, 8.0);
        const double pc = contrastD(primary, main);
        album = pushUntil(primary, ink, main, trans, std::min(15.0, pc + 1.2));
        const double secTarget = std::max(6.2, std::min(pc - 2.6, 8.0));
        secondary = mixToContrast(primary, main, main, trans, secTarget);
        const double sc = contrastD(secondary, main);
        const double dimTarget = std::max(4.7, std::min(sc - 1.8, 5.15));
        dim = mixToContrast(secondary, main, main, trans, dimTarget);
    };
    ladder(bgMain, bgTrans);
    for (int n = 0; n < 8; ++n) {
        if (distD(primary, secondary) >= 28 && distD(secondary, dim) >= 22
            && contrastD(primary, bgMain) > contrastD(secondary, bgMain)
            && contrastD(secondary, bgMain) > contrastD(dim, bgMain)
            && contrastD(dim, bgTrans) >= 4.5)
            break;
        bgTrans = mixD(bgTrans, paper, 0.18);
        bgSide  = mixD(bgSide, paper, 0.10);
        ladder(bgMain, bgTrans);
    }
    if (!(contrastD(primary, bgMain) > contrastD(secondary, bgMain)
          && contrastD(secondary, bgMain) > contrastD(dim, bgMain)
          && distD(primary, secondary) >= 24 && distD(secondary, dim) >= 18)) {
        error = "the text ladder has no room on this page";
        return false;
    }

    Rgb accent{};
    if (forceAccent) {
        accent = pushUntil(forced, ink, bgMain, bgTrans, 4.55);
    } else {
        bool found = false;
        double bestScore = -1e9, bestCr = -1.0;
        static const char* kOrder[] = { "green", "cyan", "blue", "magenta", "yellow" };
        static const double kBias[] = { 4.0, 5.0, 5.0, 6.0, -12.0 };
        for (int ni = 0; ni < 5; ++ni) {
            const int idx = ansiIndex(kOrder[ni]);
            const Rgb* buckets[2] = { src.bright, src.normal };
            const bool* has[2] = { src.hasB, src.hasN };
            for (int bi = 0; bi < 2; ++bi) {
                if (!has[bi][idx]) continue;
                const Rgb raw = buckets[bi][idx];
                const Rgb adj = pushUntil(raw, ink, bgMain, bgTrans, 4.55);
                const double cr = std::min(contrastD(adj, bgMain), contrastD(adj, bgTrans));
                if (cr < 4.5) continue;
                const double moved = distD(adj, raw) / 3.0;
                const double score = satD(adj) * 100.0 - moved * 0.25 + kBias[ni];
                if (!found || score > bestScore || (score == bestScore && cr > bestCr)) {
                    found = true;
                    bestScore = score;
                    bestCr = cr;
                    accent = adj;
                }
            }
        }
        if (!found) {
            error = "no color clears as an accent";
            return false;
        }
    }

    Rgb hover, sep, inp, ph;
    if (light) {
        hover = mixD(bgMain, ink, 0.08);
        sep   = mixD(bgMain, ink, 0.16);
        inp   = mixD(bgMain, ink, 0.035);
        ph    = mixD(bgMain, ink, 0.07);
    } else {
        hover = mixD(bgMain, ink, 0.11);
        sep   = mixD(bgMain, ink, 0.18);
        inp   = mixD(bgMain, ink, 0.05);
        ph    = mixD(bgMain, ink, 0.08);
    }
    const Rgb tile = mixD(accent, bgMain, 0.55);

    const Rgb red0 = src.hasN[1] ? src.normal[1] : Rgb{220, 70, 70};
    const Rgb yel0 = src.hasN[3] ? src.normal[3] : Rgb{224, 180, 40};
    Rgb errorC = pushUntil(red0, ink, bgMain, bgTrans, 4.55);
    Rgb warning = pushUntil(yel0, ink, bgMain, bgTrans, 4.55);
    if (distD(warning, accent) < 48)
        warning = pushUntil(mixD(red0, yel0, 0.45), ink, bgMain, bgTrans, 4.55);
    if (distD(errorC, accent) < 48) {
        const Rgb away = light ? Rgb{80, 0, 0} : Rgb{255, 255, 255};
        errorC = pushUntil(mixD(errorC, away, 0.35), ink, bgMain, bgTrans, 4.55);
    }

    auto adjust = [&](Rgb c) { return pushUntil(c, ink, bgTrack, bgTrack, 3.05); };
    Rgb q0, q1, q2, q3;
    if (light) {
        q0 = {20, 20, 20}; q1 = {140, 70, 0}; q2 = {0, 105, 115}; q3 = {120, 96, 0};
    } else {
        q0 = {255, 255, 255}; q1 = {255, 165, 0}; q2 = {0, 255, 255}; q3 = {255, 255, 0};
    }
    q0 = adjust(q0); q1 = adjust(q1); q2 = adjust(q2); q3 = adjust(q3);

    out.bgMain = pack(bgMain);
    out.bgTrack = pack(bgTrack);
    out.bgSidebar = pack(bgSide);
    out.bgTransport = pack(bgTrans);
    out.textPrimary = pack(primary);
    out.textSecondary = pack(secondary);
    out.textDim = pack(dim);
    out.textAlbum = pack(album);
    out.accent = pack(accent);
    out.hover = pack(hover);
    out.separator = pack(sep);
    out.inputBg = pack(inp);
    out.placeholder = pack(ph);
    out.tileMore = pack(tile);
    out.error = pack(errorC);
    out.warning = pack(warning);
    out.qDsd = pack(q0);
    out.qDxd = pack(q1);
    out.qHires = pack(q2);
    out.qStandard = pack(q3);
    if (!paletteAcceptable(out)) {
        error = "fails the contrast rules";
        return false;
    }
    return true;
}

// ── Text ────────────────────────────────────────────────────────────────────

std::string trimCopy(std::string s) {
    size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    size_t b = s.size();
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

std::string unquote(std::string s) {
    s = trimCopy(std::move(s));
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"')
                          || (s.front() == '\'' && s.back() == '\'')))
        s = s.substr(1, s.size() - 2);
    return s;
}

// A '#' starts a comment only outside quotes, and only when it is not a hex
// color. `bg = #0a0a0a` is the ordinary way to write a direct file; the
// Alacritty files quote the same token. `# note` still drops.
bool hexRun(const std::string& s, size_t i) {
    if (i >= s.size() || s[i] != '#') return false;
    size_t n = 0;
    for (size_t j = i + 1; j < s.size(); ++j) {
        const unsigned char c = (unsigned char)s[j];
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')
                      || (c >= 'A' && c <= 'F');
        if (!hex) break;
        ++n;
    }
    return n == 3 || n == 6 || n == 8;
}

std::string stripComment(const std::string& raw) {
    std::string out;
    char q = 0;
    for (size_t i = 0; i < raw.size(); ++i) {
        const char ch = raw[i];
        if (q) {
            out.push_back(ch);
            if (ch == q) q = 0;
            continue;
        }
        if (ch == '"' || ch == '\'') { q = ch; out.push_back(ch); continue; }
        if (ch == '#') {
            if (!hexRun(raw, i)) break;
            out.push_back(ch);
            continue;
        }
        out.push_back(ch);
    }
    return trimCopy(out);
}

bool parseHex(const std::string& token, Rgb& out) {
    std::string s = unquote(token);
    const auto sp = s.find_first_of(" \t");
    if (sp != std::string::npos) s = s.substr(0, sp);
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (s.size() >= 2 && s[0] == '0' && s[1] == 'x') s = s.substr(2);
    if (!s.empty() && s[0] == '#') s = s.substr(1);
    if (s.size() == 3) {
        std::string e;
        for (char c : s) { e.push_back(c); e.push_back(c); }
        s = std::move(e);
    }
    if (s.size() == 8) s = s.substr(0, 6);
    if (s.size() != 6) return false;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    int v[6];
    for (int i = 0; i < 6; ++i) {
        v[i] = nib(s[(size_t)i]);
        if (v[i] < 0) return false;
    }
    out = { v[0] * 16 + v[1], v[2] * 16 + v[3], v[4] * 16 + v[5] };
    return true;
}

std::string lowerCopy(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

std::string sanitizeId(std::string s) {
    std::string o;
    bool dash = false;
    for (unsigned char ch : s) {
        char c = (char)ch;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            o.push_back(c);
            dash = false;
        } else if (!o.empty() && !dash) {
            o.push_back('-');
            dash = true;
        }
    }
    while (!o.empty() && o.back() == '-') o.pop_back();
    return o;
}

std::string prettifyStem(const std::string& stem) {
    std::string s = stem;
    for (char& c : s)
        if (c == '_' || c == '-') c = ' ';
    bool up = true;
    for (char& c : s) {
        if (c == ' ') { up = true; continue; }
        if (up && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        up = false;
    }
    return trimCopy(s);
}

std::string nameFromComment(const std::string& text) {
    std::string cur;
    for (size_t i = 0; i <= text.size(); ++i) {
        const char ch = i == text.size() ? '\n' : text[i];
        if (ch != '\n') { cur.push_back(ch); continue; }
        const std::string line = trimCopy(cur);
        cur.clear();
        if (line.empty() || line[0] != '#') continue;
        const std::string rest = line.substr(1);
        const std::string low = lowerCopy(rest);
        const auto colors = low.find("colors");
        const auto lp = rest.find('(');
        const auto rp = rest.find(')');
        if (colors == std::string::npos || lp == std::string::npos || rp == std::string::npos
            || rp <= lp)
            continue;
        const std::string n = trimCopy(rest.substr(lp + 1, rp - lp - 1));
        if (!n.empty()) return n;
    }
    return {};
}

struct DirectKeys {
    std::map<std::string, std::string> kv;
    bool alacritty = false;
    Ansi ansi;
};

DirectKeys scanText(const std::string& text) {
    DirectKeys d;
    std::string section;
    std::string cur;
    for (size_t i = 0; i <= text.size(); ++i) {
        const char ch = i == text.size() ? '\n' : text[i];
        if (ch != '\n') { cur.push_back(ch); continue; }
        const std::string line = stripComment(cur);
        cur.clear();
        if (line.empty()) continue;
        if (line.front() == '[' && line.back() == ']') {
            section = lowerCopy(trimCopy(line.substr(1, line.size() - 2)));
            if (section == "colors.primary" || section == "colors.normal"
                || section == "colors.bright")
                d.alacritty = true;
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = lowerCopy(trimCopy(line.substr(0, eq)));
        std::string val = trimCopy(line.substr(eq + 1));
        if (key.empty() || val.empty() || val.front() == '{') continue;
        Rgb rgb{};
        const bool isColor = parseHex(val, rgb);
        if (section == "colors.primary" && isColor) {
            if (key == "background") { d.ansi.bg = rgb; d.ansi.hasBg = true; }
            else if (key == "foreground") { d.ansi.fg = rgb; d.ansi.hasFg = true; }
        } else if (section == "colors.normal" && isColor) {
            const int idx = ansiIndex(key);
            if (idx >= 0) { d.ansi.normal[idx] = rgb; d.ansi.hasN[idx] = true; }
        } else if (section == "colors.bright" && isColor) {
            const int idx = ansiIndex(key);
            if (idx >= 0) { d.ansi.bright[idx] = rgb; d.ansi.hasB[idx] = true; }
        } else if (section.empty()) {
            d.kv[key] = unquote(val);
        }
    }
    return d;
}

bool applyDirectOverride(Palette& p, const std::map<std::string, std::string>& kv,
                         const char* key, ColorRef& slot, std::string& error) {
    const auto it = kv.find(key);
    if (it == kv.end()) return true;
    Rgb rgb{};
    if (!parseHex(it->second, rgb)) {
        error = std::string(key) + " is not a color";
        return false;
    }
    slot = pack(rgb);
    return true;
}

}  // namespace

int themeCount() {
    ensureBuilt();
    return (int)gCatalog.size();
}

const Palette& themeAt(int index) {
    ensureBuilt();
    assert(index >= 0 && index < (int)gCatalog.size());
    return gCatalog[(size_t)index];
}

int activeThemeIndex() {
    ensureBuilt();
    return gActiveIndex;
}

const char* activeThemeId() {
    ensureBuilt();
    return gActiveId.c_str();
}

bool applyTheme(const char* id) {
    ensureBuilt();
    if (!id || !id[0]) id = "matrix";
    int found = -1;
    for (int i = 0; i < (int)gCatalog.size(); ++i)
        if (gCatalog[(size_t)i].id == id) { found = i; break; }
    if (found < 0) return false;
    if (!paletteAcceptable(gCatalog[(size_t)found])) {
        // An import that fails is refused at load. A built-in that fails was
        // edited into a page the contrast rules reject.
        if (isBuiltinId(gCatalog[(size_t)found].id))
            assert(false && "theme fails contrast");
        return false;
    }
    applyPalette(gCatalog[(size_t)found]);
    gActiveId = gCatalog[(size_t)found].id;
    gActiveIndex = found;
    return true;
}

void checkBuiltinThemes() {
    // Every check in this function is an assert. NDEBUG removes the assert
    // and leaves the loop variable that existed only to feed it, which is
    // -Wunused-variable in a Release build. The walk is therefore debug-only.
    // player_view.cc calls this under the same guard; the symbol stays so a
    // debug test can link it.
#ifndef NDEBUG
    std::vector<std::string> closed;
    std::string cur;
    std::vector<std::string> ids;
    ids.reserve((size_t)kBuiltinCount);
    for (const Builtin& b : kBuiltins) {
        const Palette p = fromBuiltin(b);
        assert(paletteAcceptable(p) && "theme fails contrast");
        assert(!p.id.empty() && !p.group.empty() && !p.name.empty());
        if (cur != b.group) {
            for (const std::string& g : closed)
                assert(g != b.group && "theme group repeats");
            if (!cur.empty()) closed.push_back(cur);
            cur = b.group;
        }
        ids.push_back(p.id);
    }
    std::sort(ids.begin(), ids.end());
    assert(std::adjacent_find(ids.begin(), ids.end()) == ids.end());
    assert(kBuiltinCount == 24);
#endif
}

bool parseThemeText(const std::string& text, const std::string& fallbackId,
                    Palette& out, std::string& error) {
    const DirectKeys scanned = scanText(text);
    Palette built;
    if (scanned.alacritty) {
        if (!fillFromAnsi(scanned.ansi, false, {}, built, error))
            return false;
    } else {
        Rgb bg{}, fg{}, accent{};
        const auto bgIt = scanned.kv.find("bg");
        const auto fgIt = scanned.kv.find("text");
        const auto acIt = scanned.kv.find("accent");
        if (bgIt == scanned.kv.end() || fgIt == scanned.kv.end()
            || acIt == scanned.kv.end()) {
            error = "needs bg, text and accent";
            return false;
        }
        if (!parseHex(bgIt->second, bg) || !parseHex(fgIt->second, fg)
            || !parseHex(acIt->second, accent)) {
            error = "bg, text or accent is not a color";
            return false;
        }
        Ansi mini;
        mini.hasBg = mini.hasFg = true;
        mini.bg = bg;
        mini.fg = fg;
        if (!fillFromAnsi(mini, true, accent, built, error))
            return false;
        const char* colorKeys[] = {
            "bg", "bg.track", "bg.sidebar", "bg.transport",
            "text", "text.secondary", "text.dim", "text.album",
            "accent", "hover", "separator", "input", "placeholder", "tile",
            "error", "warning",
            "quality.dsd", "quality.dxd", "quality.hires", "quality.standard"
        };
        ColorRef* slots[] = {
            &built.bgMain, &built.bgTrack, &built.bgSidebar, &built.bgTransport,
            &built.textPrimary, &built.textSecondary, &built.textDim, &built.textAlbum,
            &built.accent, &built.hover, &built.separator, &built.inputBg,
            &built.placeholder, &built.tileMore,
            &built.error, &built.warning,
            &built.qDsd, &built.qDxd, &built.qHires, &built.qStandard
        };
        for (size_t i = 0; i < sizeof(colorKeys) / sizeof(colorKeys[0]); ++i)
            if (!applyDirectOverride(built, scanned.kv, colorKeys[i], *slots[i], error))
                return false;
        if (!paletteAcceptable(built)) {
            error = "fails the contrast rules";
            return false;
        }
    }

    std::string id = sanitizeId(scanned.kv.count("id") ? scanned.kv.at("id") : fallbackId);
    if (id.empty()) {
        error = "id is empty";
        return false;
    }
    std::string name;
    if (scanned.kv.count("name")) name = scanned.kv.at("name");
    if (name.empty()) name = nameFromComment(text);
    if (name.empty()) name = prettifyStem(fallbackId.empty() ? id : fallbackId);
    if (name.empty()) name = id;

    std::string group = "Imported";
    if (scanned.kv.count("group") && !trimCopy(scanned.kv.at("group")).empty())
        group = trimCopy(scanned.kv.at("group"));
    std::string blurb = "Imported from a color file.";
    if (scanned.kv.count("blurb") && !trimCopy(scanned.kv.at("blurb")).empty())
        blurb = trimCopy(scanned.kv.at("blurb"));

    built.id = std::move(id);
    built.group = std::move(group);
    built.name = std::move(name);
    built.blurb = std::move(blurb);
    out = std::move(built);
    error.clear();
    return true;
}

int loadThemeDir(const std::string& dir, bool createDir) {
    int added = 0;
    namespace fs = std::filesystem;
    std::error_code ec;
    if (createDir) {
        fs::create_directories(dir, ec);
        if (ec) {
            fprintf(stderr, "[Theme] could not create %s (%s)\n",
                    dir.c_str(), ec.message().c_str());
            reindexActive();
            return 0;
        }
    } else if (!fs::is_directory(dir, ec) || ec) {
        return 0;
    }

    std::vector<fs::path> files;
    fs::directory_iterator it(fs::path(dir), ec);
    if (ec) {
        fprintf(stderr, "[Theme] could not read %s (%s)\n",
                dir.c_str(), ec.message().c_str());
        reindexActive();
        return 0;
    }
    for (; it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) break;
        std::error_code fec;
        if (!it->is_regular_file(fec) || fec) continue;
        const std::string ext = lowerCopy(it->path().extension().string());
        if (ext == ".theme" || ext == ".toml")
            files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());

    std::vector<Palette> imports;
    for (const fs::path& path : files) {
        std::ifstream in(path);
        if (!in) {
            fprintf(stderr, "[Theme] %s: could not read\n", path.string().c_str());
            ++gThemeSkips;
            continue;
        }
        const std::string text((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        Palette parsed;
        std::string error;
        const std::string stem = path.stem().string();
        if (!parseThemeText(text, stem, parsed, error)) {
            fprintf(stderr, "[Theme] %s: %s\n", path.string().c_str(), error.c_str());
            ++gThemeSkips;
            continue;
        }
        if (isBuiltinId(parsed.id)) {
            fprintf(stderr, "[Theme] %s: id \"%s\" is built in, left unchanged\n",
                    path.string().c_str(), parsed.id.c_str());
            ++gThemeSkips;
            continue;
        }
        bool dup = false;
        for (const Palette& have : gCatalog)
            if (have.id == parsed.id) { dup = true; break; }
        if (!dup) {
            for (const Palette& have : imports)
                if (have.id == parsed.id) { dup = true; break; }
        }
        if (dup) {
            // A second file in this folder is a real collision. An id already
            // loaded from an earlier folder is the listener's copy winning
            // over the bundled one, and the page should not call that left out.
            bool inBatch = false;
            for (const Palette& have : imports)
                if (have.id == parsed.id) { inBatch = true; break; }
            if (inBatch) {
                fprintf(stderr, "[Theme] %s: id \"%s\" already imported\n",
                        path.string().c_str(), parsed.id.c_str());
                ++gThemeSkips;
            }
            continue;
        }
        imports.push_back(std::move(parsed));
    }
    std::sort(imports.begin(), imports.end(),
              [](const Palette& a, const Palette& b) { return a.id < b.id; });

    // Insert so a group stays one run. An import that names an existing group
    // joins the end of that run; a new group is appended. Processing in id
    // order keeps each of those runs sorted. The page does not read this
    // order — themeDisplayOrder does — but a group that splits in two would
    // fail the catalogue's own contiguity check.
    for (Palette& p : imports) {
        int at = (int)gCatalog.size();
        for (int i = 0; i < (int)gCatalog.size(); ++i) {
            if (gCatalog[(size_t)i].group != p.group) continue;
            at = i + 1;
            while (at < (int)gCatalog.size() && gCatalog[(size_t)at].group == p.group)
                ++at;
            break;
        }
        gCatalog.insert(gCatalog.begin() + at, std::move(p));
        ++added;
    }
    if (added > 0)
        fprintf(stderr, "[Theme] %d from %s\n", added, dir.c_str());
    reindexActive();
    return added;
}

int reloadImportedThemes(const std::string& dir) {
    ensureBuilt();
    resetToBuiltins();
    gThemeSkips = 0;
    return loadThemeDir(dir, true);
}

int addImportedThemes(const std::string& dir, bool createDir) {
    ensureBuilt();
    return loadThemeDir(dir, createDir);
}

namespace {

struct Lab {
    double L = 0;
    double a = 0;
    double b = 0;
};

double linSrgb(int channel) {
    const double s = (double)channel / 255.0;
    return s <= 0.04045 ? s / 12.92
                        : std::pow((s + 0.055) / 1.055, 2.4);
}

Lab oklabOf(ColorRef c) {
    const double R = linSrgb(GetRValue(c));
    const double G = linSrgb(GetGValue(c));
    const double B = linSrgb(GetBValue(c));
    const double l = 0.4122214708 * R + 0.5363325363 * G + 0.0514459929 * B;
    const double m = 0.2119034982 * R + 0.6806995451 * G + 0.1073969566 * B;
    const double s = 0.0883024619 * R + 0.2817188376 * G + 0.6299787005 * B;
    const double l_ = std::cbrt(l);
    const double m_ = std::cbrt(m);
    const double s_ = std::cbrt(s);
    Lab o;
    o.L = 0.2104542553 * l_ + 0.7936177850 * m_ - 0.0040720468 * s_;
    o.a = 1.9779984951 * l_ - 2.4285922050 * m_ + 0.4505937099 * s_;
    o.b = 0.0259040371 * l_ + 0.7827717662 * m_ - 0.8086757660 * s_;
    return o;
}

double labChroma(const Lab& o) { return std::hypot(o.a, o.b); }

double labDist(const Lab& x, const Lab& y) {
    const double dL = x.L - y.L;
    const double da = x.a - y.a;
    const double db = x.b - y.b;
    return std::sqrt(dL * dL + da * da + db * db);
}

int axisBin(double v, bool lightness) {
    double t = lightness ? v : (v + 0.4) / 0.8;
    int i = (int)std::floor(t * 8.0);
    if (i < 0) i = 0;
    if (i > 7) i = 7;
    return i;
}

int meanByte(uint64_t sum, int count) {
    if (count <= 0) return 0;
    int v = (int)std::lround((double)sum / (double)count);
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return v;
}

bool themeListed(const char* id) {
    if (!id || !id[0]) return false;
    ensureBuilt();
    for (const Palette& p : gCatalog)
        if (p.id == id) return true;
    return false;
}

std::string extLower(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    for (char& c : ext)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return ext;
}

}  // namespace

int themeSkipCount() { return gThemeSkips; }

bool themeIsBuiltinId(const char* id) {
    return id && id[0] && isBuiltinId(id);
}

namespace {

std::string canonDaypart(const std::string& group) {
    size_t a = 0;
    while (a < group.size() && (group[a] == ' ' || group[a] == '\t' || group[a] == '\r'))
        ++a;
    size_t b = group.size();
    while (b > a && (group[b - 1] == ' ' || group[b - 1] == '\t' || group[b - 1] == '\r'))
        --b;
    std::string o;
    o.reserve(b - a);
    for (size_t i = a; i < b; ++i) {
        char c = group[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        o.push_back(c);
    }
    return o;
}

std::string foldThemeName(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

}  // namespace

ThemeDaypart themeDaypartFromLuma(float luma) {
    if (luma > kThemeMorningLuma) return ThemeDaypart::Morning;
    if (luma > kThemeAfternoonLuma) return ThemeDaypart::Afternoon;
    if (luma > kThemeEveningLuma) return ThemeDaypart::Evening;
    return ThemeDaypart::Night;
}

bool themeDaypartFromName(const std::string& group, ThemeDaypart& out) {
    const std::string g = canonDaypart(group);
    if (g == "morning")   { out = ThemeDaypart::Morning;   return true; }
    if (g == "afternoon") { out = ThemeDaypart::Afternoon; return true; }
    if (g == "evening")   { out = ThemeDaypart::Evening;   return true; }
    if (g == "night")     { out = ThemeDaypart::Night;     return true; }
    return false;
}

ThemeDaypart themeDaypartOf(const Palette& p) {
    ThemeDaypart named;
    if (!isBuiltinId(p.id) && themeDaypartFromName(p.group, named))
        return named;
    return themeDaypartFromLuma(themeLuma(p.bgMain));
}

const char* themeDaypartId(ThemeDaypart d) {
    switch (d) {
    case ThemeDaypart::Morning:   return "morning";
    case ThemeDaypart::Afternoon: return "afternoon";
    case ThemeDaypart::Evening:   return "evening";
    case ThemeDaypart::Night:     return "night";
    }
    return "night";
}

int themeSwatches(const Palette& p, ColorRef* out, int cap) {
    if (!out || cap <= 0) return 0;
    const ColorRef all[] = {
        p.bgMain, p.bgTrack, p.bgSidebar, p.bgTransport,
        p.textPrimary, p.textSecondary, p.textDim, p.textAlbum,
        p.accent, p.hover, p.separator, p.inputBg, p.placeholder, p.tileMore,
        p.error, p.warning, p.qDsd, p.qDxd, p.qHires, p.qStandard
    };
    int n = 0;
    for (ColorRef c : all) {
        bool seen = false;
        for (int i = 0; i < n; ++i)
            if (out[i] == c) { seen = true; break; }
        if (seen) continue;
        if (n >= cap) break;
        out[n++] = c;
    }
    return n;
}

bool themeIsDay(const Palette& p) { return themeLuma(p.bgMain) > 0.45f; }

bool themeAccentQuiet(const Palette& p) { return labChroma(oklabOf(p.accent)) < 0.04; }

double themeAccentHue(const Palette& p) {
    const Lab o = oklabOf(p.accent);
    double h = std::atan2(o.b, o.a);
    if (h < 0) h += 6.283185307179586;
    return h;
}

void themeDisplayOrder(std::vector<int>& out) {
    ensureBuilt();
    out.resize(gCatalog.size());
    for (int i = 0; i < (int)gCatalog.size(); ++i) out[(size_t)i] = i;
    std::sort(out.begin(), out.end(), [](int ia, int ib) {
        const Palette& a = gCatalog[(size_t)ia];
        const Palette& b = gCatalog[(size_t)ib];
        const int pa = (int)themeDaypartOf(a);
        const int pb = (int)themeDaypartOf(b);
        if (pa != pb) return pa < pb;
        const std::string na = foldThemeName(a.name);
        const std::string nb = foldThemeName(b.name);
        if (na != nb) return na < nb;
        return a.id < b.id;
    });
}

// The nearest-theme match reads this. The cover palette does not: a coarse
// bin cannot tell a sleeve's own marks apart. See paletteFromCover.
struct CoverBin { int count = 0; uint64_t sr = 0, sg = 0, sb = 0; };
struct CoverHist {
    CoverBin bin[512]{};
    int kept = 0;
    ColorRef mean(int i) const {
        const CoverBin& b = bin[i];
        return RGB(meanByte(b.sr, b.count), meanByte(b.sg, b.count),
                   meanByte(b.sb, b.count));
    }
};

static CoverHist sampleCover(const uint8_t* rgba, int w, int h) {
    CoverHist hist;
    if (!rgba || w <= 0 || h <= 0) return hist;
    const int stepX = std::max(1, w / 32);
    const int stepY = std::max(1, h / 32);
    for (int y = 0; y < h; y += stepY) {
        for (int x = 0; x < w; x += stepX) {
            const uint8_t* p = rgba + ((size_t)y * (size_t)w + (size_t)x) * 4u;
            if (p[3] < 128) continue;
            const Lab o = oklabOf(RGB(p[0], p[1], p[2]));
            const int idx = axisBin(o.L, true) * 64
                          + axisBin(o.a, false) * 8
                          + axisBin(o.b, false);
            CoverBin& bin = hist.bin[idx];
            ++bin.count;
            bin.sr += p[0];
            bin.sg += p[1];
            bin.sb += p[2];
            ++hist.kept;
        }
    }
    return hist;
}

CoverColors coverColorsFromRgba(const uint8_t* rgba, int w, int h) {
    CoverColors out;
    const CoverHist hist = sampleCover(rgba, w, h);
    if (hist.kept < 16) return out;

    int ground = -1;
    for (int i = 0; i < 512; ++i) {
        if (hist.bin[i].count == 0) continue;
        if (ground < 0 || hist.bin[i].count > hist.bin[ground].count) ground = i;
    }
    if (ground < 0) return out;

    auto meanOf = [&](int i) { return hist.mean(i); };
    out.ground = meanOf(ground);
    out.ready = true;

    // A second mass steers the accent when it is a color the eye can name,
    // or when it is ink (the black of a photograph, the white of a page).
    // A tint between the two gates is a cast from the file: counting it
    // would paint the accent pink on a sleeve that has no pink.
    constexpr double kShare = 0.08;
    constexpr double kColorChroma = 0.03;
    constexpr double kInkChroma = 0.01;
    const double share = (double)hist.kept * kShare;
    int signal = -1;
    double bestChroma = -1;
    for (int i = 0; i < 512; ++i) {
        if (i == ground || hist.bin[i].count <= 0) continue;
        if ((double)hist.bin[i].count < share) continue;
        const ColorRef mean = meanOf(i);
        const double ch = labChroma(oklabOf(mean));
        if (ch < kColorChroma) continue;
        const bool better = signal < 0
            || ch > bestChroma + 1e-9
            || (std::abs(ch - bestChroma) <= 1e-9 && hist.bin[i].count > hist.bin[signal].count);
        if (!better) continue;
        signal = i;
        bestChroma = ch;
    }
    if (signal < 0) {
        for (int i = 0; i < 512; ++i) {
            if (i == ground || hist.bin[i].count <= 0) continue;
            if ((double)hist.bin[i].count < share) continue;
            if (labChroma(oklabOf(meanOf(i))) >= kInkChroma) continue;
            if (signal < 0 || hist.bin[i].count > hist.bin[signal].count) signal = i;
        }
    }
    if (signal >= 0) {
        out.hasSignal = true;
        out.signal = meanOf(signal);
    }
    return out;
}

int matchTheme(const CoverColors& cover, const Palette* pals, int n) {
    if (!cover.ready || !pals || n <= 0) return -1;
    const bool day = themeLuma(cover.ground) > 0.45f;
    const Lab ground = oklabOf(cover.ground);
    const Lab signal = cover.hasSignal ? oklabOf(cover.signal) : Lab{};
    // 0.35 keeps a real second color in the decision without letting it
    // outvote the page. 0.08 is the charge for inventing a hue when the
    // cover named none: an accent as loud as Enfocado Light's magenta
    // (chroma ~0.23) costs about the OKLab step from pure white to Flexoki
    // Light, so a white sleeve keeps black type and a warm sleeve still
    // keeps its warm page.
    constexpr double kAccentPull = 0.35;
    constexpr double kNeutralAccent = 0.08;
    int win = -1;
    double best = 0;
    for (int i = 0; i < n; ++i) {
        if (themeIsDay(pals[i]) != day) continue;
        const Lab accent = oklabOf(pals[i].accent);
        double score = labDist(oklabOf(pals[i].bgMain), ground);
        if (cover.hasSignal)
            score += kAccentPull * labDist(accent, signal);
        else
            score += kNeutralAccent * labChroma(accent);
        const bool better = win < 0
            || score < best - 1e-9
            || (std::abs(score - best) <= 1e-9 && pals[i].id < pals[win].id);
        if (!better) continue;
        win = i;
        best = score;
    }
    return win;
}

const char* chooseTheme(ThemeMode mode, bool playing, bool coverReady,
                        const char* matchId, const char* savedId,
                        const char* heldId) {
    if (mode == ThemeMode::Static)
        return themeListed(savedId) ? savedId : "matrix";
    if (!playing) return "matrix";
    if (coverReady) return themeListed(matchId) ? matchId : "matrix";
    return themeListed(heldId) ? heldId : "matrix";
}

double encodeLin(double c) {
    if (c <= 0.0) return 0.0;
    if (c >= 1.0) return 255.0;
    const double s = c <= 0.0031308 ? 12.92 * c
                                    : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
    const double clamped = std::min(1.0, std::max(0.0, s));
    return std::lround(clamped * 255.0);
}

Rgb rgbFromOklab(Lab o) {
    const double l_ = o.L + 0.3963377774 * o.a + 0.2158037573 * o.b;
    const double m_ = o.L - 0.1055613458 * o.a - 0.0638541728 * o.b;
    const double s_ = o.L - 0.0894841775 * o.a - 1.2914855480 * o.b;
    const double l = l_ * l_ * l_;
    const double m = m_ * m_ * m_;
    const double s = s_ * s_ * s_;
    const double r =  4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s;
    const double g = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s;
    const double b = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s;
    return { (int)encodeLin(r), (int)encodeLin(g), (int)encodeLin(b) };
}

double hueOf(const Lab& o) {
    double h = std::atan2(o.b, o.a);
    if (h < 0) h += 6.283185307179586;
    return h;
}

double hueGap(double a, double b) {
    double d = std::fabs(a - b);
    if (d > 3.141592653589793) d = 6.283185307179586 - d;
    return d;
}

// Lightness only. a and b stay, so a red stays red and cream type stays cream.
// An exact source that already clears `need` is returned unchanged — a
// round trip through OKLab would repaint a color the sleeve actually used.
Rgb forceReadable(Rgb src, Rgb page, Rgb trans, double need, bool toWhite) {
    if (contrastD(src, page) >= need && contrastD(src, trans) >= need)
        return src;
    const Lab o = oklabOf(pack(src));
    const double dest = toWhite ? 1.0 : 0.0;
    auto at = [&](double t) {
        Lab x = o;
        x.L = o.L + (dest - o.L) * t;
        if (x.L < 0) x.L = 0;
        if (x.L > 1) x.L = 1;
        return x;
    };
    auto ok = [&](const Lab& x) {
        const Rgb c = rgbFromOklab(x);
        return contrastD(c, page) >= need && contrastD(c, trans) >= need;
    };
    if (!ok(at(1.0))) return rgbFromOklab(at(1.0));
    double lo = 0, hi = 1;
    Lab best = at(1.0);
    for (int i = 0; i < 22; ++i) {
        const double mid = (lo + hi) * 0.5;
        const Lab x = at(mid);
        if (ok(x)) { best = x; hi = mid; }
        else lo = mid;
    }
    return rgbFromOklab(best);
}

// Walk lightness from `src` toward the page until contrast against the page
// is as close as possible to `target`, without dropping under 4.52 on either
// surface. Hue stays on `src`.
Rgb pullReadable(Rgb src, Rgb page, Rgb trans, double target) {
    const Lab from = oklabOf(pack(src));
    const Lab to = oklabOf(pack(page));
    Rgb best = src;
    double bestErr = 1e9;
    for (int i = 0; i <= 48; ++i) {
        Lab o = from;
        o.L = from.L + (to.L - from.L) * (i / 48.0);
        const Rgb c = rgbFromOklab(o);
        if (contrastD(c, page) < 4.52 || contrastD(c, trans) < 4.52) continue;
        const double err = std::fabs(contrastD(c, page) - target);
        if (err < bestErr) { bestErr = err; best = c; }
    }
    return best;
}

bool ladderFits(Rgb primary, Rgb secondary, Rgb dim, Rgb page, Rgb trans) {
    const double cp = contrastD(primary, page);
    const double cs = contrastD(secondary, page);
    const double cd = contrastD(dim, page);
    if (!(cp > cs && cs > cd)) return false;
    if (distD(primary, secondary) < 28) return false;
    if (distD(secondary, dim) < 22) return false;
    if (contrastD(primary, trans) < 4.55) return false;
    if (contrastD(secondary, trans) < 4.55) return false;
    if (contrastD(dim, trans) < 4.55) return false;
    if (cd < 4.55) return false;
    return true;
}

bool buildLadder(Rgb ink, Rgb page, Rgb trans, bool light,
                 Rgb& primary, Rgb& secondary, Rgb& dim, Rgb& album) {
    primary = forceReadable(ink, page, trans, 8.0, !light);
    if (contrastD(primary, page) < 4.52 || contrastD(primary, trans) < 4.52)
        return false;
    const double pc = contrastD(primary, page);
    const double secTarget = std::max(6.2, std::min(pc - 2.6, 8.0));
    secondary = pullReadable(primary, page, trans, secTarget);
    const double sc = contrastD(secondary, page);
    const double dimTarget = std::max(4.72, std::min(sc - 1.8, 5.15));
    dim = pullReadable(secondary, page, trans, dimTarget);
    album = forceReadable(primary, page, trans, std::min(15.0, pc + 1.0), !light);
    if (contrastD(album, page) < 4.52) album = primary;
    if (ladderFits(primary, secondary, dim, page, trans)) return true;
    for (int k = 0; k < 8; ++k) {
        secondary = pullReadable(secondary, page, trans,
                                 std::max(4.8, contrastD(secondary, page) - 0.55));
        dim = pullReadable(dim, page, trans,
                           std::max(4.6, contrastD(dim, page) - 0.4));
        if (ladderFits(primary, secondary, dim, page, trans)) return true;
    }
    return false;
}

Lab mixLab(Lab a, Lab b, double t) {
    Lab o;
    o.L = a.L + (b.L - a.L) * t;
    o.a = a.a + (b.a - a.a) * t;
    o.b = a.b + (b.b - a.b) * t;
    return o;
}

double clamp01(double v) {
    if (v < 0) return 0;
    if (v > 1) return 1;
    return v;
}

// One pigment found in the sleeve. `n` is area. `witness` counts regions
// where this color is the brightest mark in that region, which is how a thin
// gold line survives next to a field that outweighs it.
struct CoverVoice {
    double sr = 0, sg = 0, sb = 0;
    double sL = 0, sa = 0, sbL = 0;
    int n = 0;
    int center = 0;
    int rim = 0;
    int witness = 0;
    Rgb rgb{};
    Lab lab{};
};

struct CoverPx {
    Lab lab;
    int r, g, b;
    int cell;
    int center;
    int rim;
    int peak;
};

constexpr int kCoverCells = 12;
constexpr int kCoverPer = 10;
constexpr int kVoiceCap = 36;

void voiceMerge(CoverVoice& a, const CoverVoice& b) {
    a.sr += b.sr;
    a.sg += b.sg;
    a.sb += b.sb;
    a.sL += b.sL;
    a.sa += b.sa;
    a.sbL += b.sbL;
    a.n += b.n;
    a.center += b.center;
    a.rim += b.rim;
    a.witness += b.witness;
}

Lab voiceCenter(const CoverVoice& v) {
    return Lab{ v.sL / v.n, v.sa / v.n, v.sbL / v.n };
}

double voiceTau(const Lab& a, const Lab& b) {
    const double meanL = std::max(0.0, 0.5 * (a.L + b.L));
    const bool neutral = labChroma(a) < 0.025 && labChroma(b) < 0.025;
    // One pigment in daylight is about two just-noticeable OKLab steps.
    // In the dark the eye stops splitting shades the metric still splits,
    // so a black and the blue living inside it can be one voice. The cap
    // stays under the distance of a red that actually leaves the black.
    const double widen = meanL >= 0.35 ? 1.0 : (0.35 / std::max(meanL, 0.12));
    double tau = (neutral ? 0.055 : 0.040) * std::min(widen, 2.2);
    if (tau > 0.10) tau = 0.10;
    return tau;
}

void voiceAdd(CoverVoice& v, const CoverPx& s) {
    v.sr += s.r;
    v.sg += s.g;
    v.sb += s.b;
    v.sL += s.lab.L;
    v.sa += s.lab.a;
    v.sbL += s.lab.b;
    v.n += 1;
    v.center += s.center;
    v.rim += s.rim;
}

void voiceFinish(CoverVoice& v) {
    if (v.n <= 0) return;
    auto byte = [](double sum, int n) {
        int c = (int)std::lround(sum / (double)n);
        if (c < 0) c = 0;
        if (c > 255) c = 255;
        return c;
    };
    v.rgb = { byte(v.sr, v.n), byte(v.sg, v.n), byte(v.sb, v.n) };
    v.lab = oklabOf(pack(v.rgb));
}

int nearestVoice(const Lab& lab, const std::vector<CoverVoice>& vs, double tau) {
    int best = -1;
    double bd = 1e9;
    for (int i = 0; i < (int)vs.size(); ++i) {
        if (vs[(size_t)i].n <= 0 && tau < 0) continue;
        const Lab c = vs[(size_t)i].n > 0
            ? Lab{ vs[(size_t)i].sL / vs[(size_t)i].n,
                   vs[(size_t)i].sa / vs[(size_t)i].n,
                   vs[(size_t)i].sbL / vs[(size_t)i].n }
            : vs[(size_t)i].lab;
        const double d = labDist(lab, c);
        const double limit = tau < 0 ? 1e9 : voiceTau(lab, c);
        if (tau >= 0 && d > limit) continue;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

// Every twelfth of the sleeve is sampled, so a mark in a corner is not
// skipped because a grid happened to step over it. A pixel that is the most
// saturated point of its region, and clearly more saturated than that
// region, is a witness: the same pigment witnessed twice is a real mark,
// one noisy pixel is not.
std::vector<CoverVoice> readCoverVoices(const uint8_t* rgba, int w, int h) {
    std::vector<CoverVoice> empty;
    if (!rgba || w <= 0 || h <= 0) return empty;
    std::vector<CoverPx> px;
    px.reserve((size_t)kCoverCells * kCoverCells * kCoverPer);
    struct CellAcc { double sumC = 0; int n = 0; int peak = -1; double peakC = -1; };
    CellAcc cells[(size_t)kCoverCells * kCoverCells];

    for (int cy = 0; cy < kCoverCells; ++cy) {
        const int y0 = (int)((long long)cy * h / kCoverCells);
        const int y1 = (int)((long long)(cy + 1) * h / kCoverCells);
        if (y1 <= y0) continue;
        const int ys = std::max(1, (y1 - y0) / kCoverPer);
        for (int cx = 0; cx < kCoverCells; ++cx) {
            const int x0 = (int)((long long)cx * w / kCoverCells);
            const int x1 = (int)((long long)(cx + 1) * w / kCoverCells);
            if (x1 <= x0) continue;
            const int xs = std::max(1, (x1 - x0) / kCoverPer);
            CellAcc& cell = cells[(size_t)cy * kCoverCells + cx];
            for (int y = y0; y < y1; y += ys) {
                for (int x = x0; x < x1; x += xs) {
                    const uint8_t* p = rgba + ((size_t)y * (size_t)w + (size_t)x) * 4u;
                    if (p[3] < 128) continue;
                    CoverPx s;
                    s.r = p[0];
                    s.g = p[1];
                    s.b = p[2];
                    s.lab = oklabOf(RGB(p[0], p[1], p[2]));
                    s.cell = cy * kCoverCells + cx;
                    const bool inCenter = x >= w / 4 && x < w - w / 4
                                        && y >= h / 4 && y < h - h / 4;
                    const bool onRim = x < w / 12 || x >= w - w / 12
                                    || y < h / 12 || y >= h - h / 12;
                    s.center = inCenter ? 1 : 0;
                    s.rim = onRim ? 1 : 0;
                    s.peak = 0;
                    const double ch = labChroma(s.lab);
                    cell.sumC += ch;
                    cell.n += 1;
                    if (ch > cell.peakC) {
                        cell.peakC = ch;
                        cell.peak = (int)px.size();
                    }
                    px.push_back(s);
                }
            }
        }
    }
    if ((int)px.size() < 16) return empty;

    for (const CellAcc& cell : cells) {
        if (cell.n < 4 || cell.peak < 0) continue;
        const double mean = cell.sumC / (double)cell.n;
        if (cell.peakC >= 0.08 && cell.peakC >= mean + 0.04)
            px[(size_t)cell.peak].peak = 1;
    }

    std::vector<CoverVoice> vs;
    vs.reserve(kVoiceCap);
    for (const CoverPx& s : px) {
        int at = nearestVoice(s.lab, vs, 1);
        if (at < 0) {
            // A new hue that is more its own color than the closest pair
            // already stored takes a slot. The pair collapses. Grain yields
            // to a mark that shows up late in the picture.
            if ((int)vs.size() >= kVoiceCap) {
                int ia = -1, ib = -1;
                double bd = 1e9;
                for (int i = 0; i < (int)vs.size(); ++i) {
                    for (int j = i + 1; j < (int)vs.size(); ++j) {
                        const double d = labDist(voiceCenter(vs[(size_t)i]),
                                                  voiceCenter(vs[(size_t)j]));
                        if (d < bd) { bd = d; ia = i; ib = j; }
                    }
                }
                const int near = nearestVoice(s.lab, vs, -1);
                const double dn = near >= 0
                    ? labDist(s.lab, voiceCenter(vs[(size_t)near])) : 1e9;
                if (ia >= 0 && bd + 1e-9 < dn && bd < 0.12) {
                    voiceMerge(vs[(size_t)ia], vs[(size_t)ib]);
                    vs.erase(vs.begin() + ib);
                    CoverVoice born;
                    voiceAdd(born, s);
                    vs.push_back(born);
                    continue;
                }
                at = near;
            } else {
                CoverVoice born;
                voiceAdd(born, s);
                vs.push_back(born);
                continue;
            }
        }
        if (at >= 0) voiceAdd(vs[(size_t)at], s);
    }

    for (int pass = 0; pass < 3; ++pass) {
        std::vector<CoverVoice> next(vs.size());
        for (const CoverPx& s : px) {
            const int at = nearestVoice(s.lab, vs, -1);
            if (at >= 0) voiceAdd(next[(size_t)at], s);
        }
        vs.clear();
        for (CoverVoice& v : next)
            if (v.n > 0) vs.push_back(v);
        if (vs.empty()) return empty;
    }

    bool counted[kVoiceCap][kCoverCells * kCoverCells] = {};
    for (const CoverPx& s : px) {
        if (!s.peak) continue;
        const int at = nearestVoice(s.lab, vs, -1);
        if (at < 0 || at >= kVoiceCap) continue;
        if (counted[at][s.cell]) continue;
        counted[at][s.cell] = true;
        vs[(size_t)at].witness += 1;
    }
    for (CoverVoice& v : vs) voiceFinish(v);
    return vs;
}

int pageVoice(const std::vector<CoverVoice>& vs) {
    int best = -1;
    for (int i = 0; i < (int)vs.size(); ++i) {
        if (best < 0 || vs[(size_t)i].n > vs[(size_t)best].n) best = i;
    }
    return best;
}

double voiceShare(const CoverVoice& v, int total) {
    return total > 0 ? (double)v.n / (double)total : 0;
}

// How loudly a pigment separates from the field it sits on.
// OKLab chroma treats a red dot on black and a night blue on that same
// black as cousins. The eye does not. Two things it actually uses: the
// luminance step (the red has one, the night blue does not) and
// colorfulness, which collapses as the pigment gets dark. A night blue
// on black scores near 0.02. A red on that black scores near 0.38.
// Anything under the floor is the field.
constexpr double kNoticeFloor = 0.04;

double voiceNotice(const Lab& lab, Rgb rgb, const Lab& page, Rgb pageRgb) {
    const double gap = labDist(lab, page);
    const double seen = labChroma(lab) * std::pow(std::max(lab.L, 0.0), 0.70);
    const double lift = std::max(0.0, contrastD(rgb, pageRgb) - 1.0);
    if (lift <= 0.0 || gap <= 0.0) return 0.0;
    return gap * (0.20 + seen) * std::pow(lift, 0.72);
}

// A voice the eye cannot pick out of the page is the page. Folding it
// in is what keeps a blue that lives inside the black from becoming a
// second color. A red dot stays, because its notice clears the floor.
void foldUnseen(std::vector<CoverVoice>& vs) {
    if (vs.size() < 2) return;
    const int page = pageVoice(vs);
    if (page < 0) return;
    const Lab pageLab = vs[(size_t)page].lab;
    const Rgb pageRgb = vs[(size_t)page].rgb;
    std::vector<CoverVoice> kept;
    kept.reserve(vs.size());
    CoverVoice ground = vs[(size_t)page];
    for (int i = 0; i < (int)vs.size(); ++i) {
        if (i == page) continue;
        const CoverVoice& v = vs[(size_t)i];
        if (voiceNotice(v.lab, v.rgb, pageLab, pageRgb) < kNoticeFloor) {
            voiceMerge(ground, v);
            continue;
        }
        kept.push_back(v);
    }
    voiceFinish(ground);
    kept.push_back(std::move(ground));
    vs.swap(kept);
}

// Notice is the sentence. Area and the middle of the sleeve break ties.
// A gray is ink, not a mark, however bright. A single noisy pixel is
// neither: four samples of one popping hue is a dot, one sample is grain.
double voiceScore(const CoverVoice& v, const Lab& page, Rgb pageRgb, int total) {
    const double share = voiceShare(v, total);
    const double chroma = labChroma(v.lab);
    if (chroma < 0.03) return -1;
    const double notice = voiceNotice(v.lab, v.rgb, page, pageRgb);
    if (notice < kNoticeFloor) return -1;
    if (share < 0.008 && v.witness < 2 && v.n < 4) return -1;
    double presence = share;
    if (v.witness >= 2)
        presence = std::max(presence, std::min(0.08, 0.015 * (double)v.witness));
    else if (v.witness >= 1 && notice >= 0.12 && share < 0.02)
        presence = std::max(presence, 0.018);
    const double centerFrac = v.n > 0 ? (double)v.center / (double)v.n : 0;
    const double rimFrac = v.n > 0 ? (double)v.rim / (double)v.n : 0;
    double place = 1.0 + 0.50 * centerFrac;
    if (rimFrac > 0.72 && share < 0.18) place *= 0.55;
    return notice * std::pow(std::max(presence, 1e-6), 0.22) * place;
}

bool hueNear(const Lab& lab, double target, double window, double minChroma) {
    if (labChroma(lab) < minChroma) return false;
    return hueGap(hueOf(lab), target) <= window;
}

int findHueVoice(const std::vector<CoverVoice>& vs, int total, int page, int skip,
                 double target, double window, double minShare) {
    int best = -1;
    for (int i = 0; i < (int)vs.size(); ++i) {
        if (i == page || i == skip) continue;
        const CoverVoice& v = vs[(size_t)i];
        if (!hueNear(v.lab, target, window, 0.045)) continue;
        if (voiceShare(v, total) < minShare && v.witness < 2) continue;
        if (best < 0 || v.n > vs[(size_t)best].n) best = i;
    }
    return best;
}

Rgb readableVoice(const CoverVoice& v, Rgb page, Rgb trans, double need, bool toWhite,
                  double maxMove) {
    const Rgb pushed = forceReadable(v.rgb, page, trans, need, toWhite);
    const Lab src = v.lab;
    const Lab got = oklabOf(pack(pushed));
    if (std::fabs(got.L - src.L) > maxMove && contrastD(v.rgb, page) < need)
        return Rgb{-1, -1, -1};
    if (labChroma(src) >= 0.05 && labChroma(got) < 0.035)
        return Rgb{-1, -1, -1};
    return pushed;
}

bool tuneMarks(Rgb track, Rgb accent, const std::vector<CoverVoice>& vs,
               int total, int page, int accentIdx, Rgb& q0, Rgb& q1, Rgb& q2, Rgb& q3) {
    const bool lightTrack = lumaD(track) > 0.45;
    const Rgb pole = lightTrack ? Rgb{0, 0, 0} : Rgb{255, 255, 255};
    const Rgb sem[4] = {
        lightTrack ? Rgb{20, 20, 20} : Rgb{255, 255, 255},
        lightTrack ? Rgb{140, 70, 0} : Rgb{255, 165, 0},
        lightTrack ? Rgb{0, 105, 115} : Rgb{0, 255, 255},
        lightTrack ? Rgb{120, 96, 0} : Rgb{255, 255, 0}
    };
    Rgb marks[4];
    for (int m = 0; m < 4; ++m) {
        Rgb chosen = sem[m];
        if (m == 0 && !lightTrack) {
            int paper = -1;
            for (int i = 0; i < (int)vs.size(); ++i) {
                if (i == page) continue;
                const CoverVoice& v = vs[(size_t)i];
                if (labChroma(v.lab) >= 0.06) continue;
                if (v.lab.L < vs[(size_t)page].lab.L + 0.25) continue;
                if (voiceShare(v, total) < 0.03) continue;
                if (paper < 0 || v.n > vs[(size_t)paper].n) paper = i;
            }
            if (paper >= 0) chosen = vs[(size_t)paper].rgb;
        } else if (m > 0) {
            const Lab semLab = oklabOf(pack(sem[m]));
            const int src = findHueVoice(vs, total, page, accentIdx,
                                          hueOf(semLab), 0.40, 0.012);
            if (src >= 0 && labChroma(semLab) >= 0.04) {
                const double ch = labChroma(semLab);
                const double ang = hueOf(vs[(size_t)src].lab);
                Lab nudged = semLab;
                nudged.a = std::cos(ang) * ch;
                nudged.b = std::sin(ang) * ch;
                chosen = rgbFromOklab(nudged);
            }
        }
        marks[m] = pushUntil(chosen, pole, track, track, 3.05);
    }
    if (labChroma(oklabOf(pack(accent))) >= 0.04) {
        for (Rgb& mark : marks) {
            if (distD(mark, accent) >= 36) continue;
            const Rgb pulled = pushUntil(mixD(mark, pole, 0.40), pole, track, track, 3.05);
            mark = pulled;
        }
    }
    for (int pass = 0; pass < 4; ++pass) {
        bool spaced = true;
        for (int i = 0; i < 4 && spaced; ++i)
            for (int j = i + 1; j < 4; ++j)
                if (distD(marks[i], marks[j]) < 24) spaced = false;
        if (spaced) break;
        for (int m = 0; m < 4; ++m)
            marks[m] = pushUntil(sem[m], pole, track, track, 3.05);
    }
    for (int i = 0; i < 4; ++i) {
        if (contrastD(marks[i], track) < 3.0) return false;
        for (int j = i + 1; j < 4; ++j)
            if (distD(marks[i], marks[j]) < 24) return false;
    }
    q0 = marks[0];
    q1 = marks[1];
    q2 = marks[2];
    q3 = marks[3];
    return true;
}

bool coverPaletteAt(const std::vector<CoverVoice>& vs, int total, int pageIdx,
                    double shift, Palette& out) {
    const CoverVoice& ground = vs[(size_t)pageIdx];
    const bool light = lumaD(ground.rgb) > 0.45;
    Lab pageLab = ground.lab;
    Rgb pageRgb = ground.rgb;
    if (shift > 0) {
        const double dest = light ? 0.97 : 0.12;
        pageLab.L = clamp01(ground.lab.L + (dest - ground.lab.L) * shift);
        pageRgb = rgbFromOklab(pageLab);
        pageLab = oklabOf(pack(pageRgb));
    }

    int inkIdx = -1;
    double inkScore = -1;
    for (int i = 0; i < (int)vs.size(); ++i) {
        if (i == pageIdx) continue;
        const CoverVoice& v = vs[(size_t)i];
        if (labChroma(v.lab) >= 0.08) continue;
        const double share = voiceShare(v, total);
        if (share < 0.03 && v.witness < 2) continue;
        const double gap = light ? (pageLab.L - v.lab.L) : (v.lab.L - pageLab.L);
        if (gap < 0.15) continue;
        const double score = gap * std::sqrt(std::max(share, 0.02));
        if (score > inkScore) { inkScore = score; inkIdx = i; }
    }
    Rgb ink = light ? Rgb{0, 0, 0} : Rgb{255, 255, 255};
    if (inkIdx >= 0) {
        ink = vs[(size_t)inkIdx].rgb;
    } else {
        // No real ink. A small warm or cool pigment still tints the pole,
        // so a black sleeve with cream type does not become pure white.
        int tint = -1;
        double tintGap = 0;
        for (int i = 0; i < (int)vs.size(); ++i) {
            if (i == pageIdx) continue;
            const CoverVoice& v = vs[(size_t)i];
            if (labChroma(v.lab) >= 0.08) continue;
            const double gap = std::fabs(v.lab.L - pageLab.L);
            if (gap > tintGap) { tintGap = gap; tint = i; }
        }
        if (tint >= 0 && tintGap >= 0.12) {
            Lab o = oklabOf(pack(ink));
            o.a = vs[(size_t)tint].lab.a * 0.35;
            o.b = vs[(size_t)tint].lab.b * 0.35;
            o.L = light ? 0.0 : 1.0;
            const Rgb tinted = rgbFromOklab(o);
            if (labChroma(oklabOf(pack(tinted))) < 0.08) ink = tinted;
        }
    }

    std::vector<int> marks;
    marks.reserve(vs.size());
    for (int i = 0; i < (int)vs.size(); ++i) {
        if (i == pageIdx) continue;
        if (voiceScore(vs[(size_t)i], pageLab, pageRgb, total) < 0) continue;
        marks.push_back(i);
    }
    std::sort(marks.begin(), marks.end(), [&](int a, int b) {
        const double sa = voiceScore(vs[(size_t)a], pageLab, pageRgb, total);
        const double sb = voiceScore(vs[(size_t)b], pageLab, pageRgb, total);
        if (std::fabs(sa - sb) > 1e-12) return sa > sb;
        if (vs[(size_t)a].n != vs[(size_t)b].n) return vs[(size_t)a].n > vs[(size_t)b].n;
        return a < b;
    });

    int accentIdx = marks.empty() ? -1 : marks[0];
    // A red that merely ties another mark is the error signal. A red that
    // is the sleeve — clearly ahead of everything else — stays the accent.
    if (accentIdx >= 0 && marks.size() >= 2
        && hueNear(vs[(size_t)accentIdx].lab, 0.0, 0.50, 0.05)) {
        const double top = voiceScore(vs[(size_t)accentIdx], pageLab, pageRgb, total);
        for (size_t k = 1; k < marks.size(); ++k) {
            const int alt = marks[k];
            if (hueNear(vs[(size_t)alt].lab, 0.0, 0.50, 0.05)) continue;
            const double altScore = voiceScore(vs[(size_t)alt], pageLab, pageRgb, total);
            if (altScore >= top * 0.75 && top < altScore * 1.6) {
                accentIdx = alt;
                break;
            }
        }
    }

    int field2 = -1, field3 = -1;
    for (int i = 0; i < (int)vs.size(); ++i) {
        if (i == pageIdx) continue;
        if (voiceShare(vs[(size_t)i], total) < 0.06) continue;
        if (labDist(vs[(size_t)i].lab, pageLab) < 0.06) continue;
        if (voiceNotice(vs[(size_t)i].lab, vs[(size_t)i].rgb, pageLab, pageRgb) < kNoticeFloor)
            continue;
        if (field2 < 0 || vs[(size_t)i].n > vs[(size_t)field2].n) {
            field3 = field2;
            field2 = i;
        } else if (field3 < 0 || vs[(size_t)i].n > vs[(size_t)field3].n) {
            field3 = i;
        }
    }
    if (field3 >= 0 && field2 >= 0
        && labDist(vs[(size_t)field3].lab, vs[(size_t)field2].lab) < 0.05)
        field3 = -1;

    auto admit = [&](const Lab& base, int field) {
        if (field < 0) return base;
        const double jump = std::fabs(vs[(size_t)field].lab.L - base.L);
        double mt = 0.42;
        if (jump > 0.35) mt = 0.28;
        if (jump > 0.55) mt = 0.18;
        Lab o = mixLab(base, vs[(size_t)field].lab, mt);
        o.L = clamp01(o.L);
        return o;
    };
    Lab trackLab = pageLab;
    trackLab.L = clamp01(pageLab.L + (light ? -0.04 : 0.05));
    trackLab = admit(trackLab, field2);
    Lab sideLab = pageLab;
    sideLab.L = clamp01(pageLab.L + (light ? -0.03 : 0.032));
    sideLab = admit(sideLab, field3);
    Lab transLab = pageLab;
    transLab.L = clamp01(pageLab.L + (light ? -0.05 : 0.018));
    if (!light) {
        int dark = -1;
        for (int i = 0; i < (int)vs.size(); ++i) {
            if (i == pageIdx) continue;
            if (vs[(size_t)i].lab.L > pageLab.L - 0.08) continue;
            if (voiceShare(vs[(size_t)i], total) < 0.05) continue;
            if (dark < 0 || vs[(size_t)i].n > vs[(size_t)dark].n) dark = i;
        }
        if (dark >= 0) transLab = mixLab(transLab, vs[(size_t)dark].lab, 0.35);
    }
    // The bar is its own surface. A second field the eye can separate
    // from the page is admitted into it, less so when that field is a
    // bright panel on a dark sleeve — the ladder still has to clear.
    if (field2 >= 0) {
        const double jump = std::fabs(vs[(size_t)field2].lab.L - transLab.L);
        double mt = 0.50;
        if (jump > 0.35) mt = 0.26;
        if (jump > 0.55) mt = 0.14;
        const double seen = voiceNotice(vs[(size_t)field2].lab, vs[(size_t)field2].rgb,
                                         pageLab, pageRgb);
        if (seen >= 0.08) transLab = mixLab(transLab, vs[(size_t)field2].lab, mt);
    }
    transLab.L = clamp01(transLab.L);

    Rgb trackRgb = rgbFromOklab(trackLab);
    Rgb sideRgb = rgbFromOklab(sideLab);
    Rgb transRgb = rgbFromOklab(transLab);
    Rgb primary{}, secondary{}, dim{}, album{};
    bool ladder = false;
    for (int n = 0; n < 6; ++n) {
        if (buildLadder(ink, pageRgb, transRgb, light, primary, secondary, dim, album)
            && contrastD(dim, transRgb) >= 4.55) {
            ladder = true;
            break;
        }
        Lab t = oklabOf(pack(transRgb));
        t.L = clamp01(t.L + (light ? 0.05 : -0.05));
        transRgb = rgbFromOklab(t);
    }
    if (!ladder) return false;
    if (!ladderFits(primary, secondary, dim, pageRgb, transRgb)) return false;

    const bool toWhite = !light;
    int titleIdx = -1;
    for (int id : marks) {
        if (id == accentIdx) continue;
        if (labChroma(vs[(size_t)id].lab) < 0.045) continue;
        const Rgb tried = readableVoice(vs[(size_t)id], pageRgb, transRgb, 4.65, toWhite, 0.35);
        if (tried.r < 0) continue;
        if (distD(tried, primary) < 20) continue;
        if (contrastD(tried, pageRgb) < 4.6) continue;
        album = tried;
        titleIdx = id;
        break;
    }

    Rgb accent = ink;
    if (accentIdx >= 0) {
        const Rgb tried = readableVoice(vs[(size_t)accentIdx], pageRgb, transRgb, 4.55, toWhite, 0.55);
        if (tried.r >= 0 && labChroma(oklabOf(pack(tried))) >= 0.04)
            accent = tried;
        else
            accentIdx = -1;
    }
    accent = forceReadable(accent, pageRgb, transRgb, 4.55, toWhite);

    int tileIdx = -1;
    int ruleIdx = -1;
    for (int id : marks) {
        if (id == accentIdx || id == titleIdx) continue;
        if (tileIdx < 0) tileIdx = id;
        else if (ruleIdx < 0) { ruleIdx = id; break; }
    }
    if (tileIdx < 0) tileIdx = titleIdx >= 0 ? titleIdx : accentIdx;
    int washIdx = -1;
    for (int id : marks) {
        if (id == accentIdx || id == titleIdx || id == tileIdx || id == ruleIdx) continue;
        washIdx = id;
        break;
    }
    // A second mark stays recognizable. Mixing it halfway back into the
    // page was how two sleeves with different figures came out one wash.
    Lab tileLab = mixLab(oklabOf(pack(accent)), pageLab, 0.55);
    if (tileIdx >= 0)
        tileLab = mixLab(vs[(size_t)tileIdx].lab, pageLab, 0.22);
    if (ruleIdx < 0) ruleIdx = tileIdx;

    Lab sepLab = mixLab(pageLab, oklabOf(pack(dim)), 0.22);
    if (ruleIdx >= 0) sepLab = mixLab(sepLab, vs[(size_t)ruleIdx].lab, 0.34);
    for (int n = 0; n < 8; ++n) {
        if (contrastD(rgbFromOklab(sepLab), pageRgb) >= 1.25) break;
        sepLab = mixLab(sepLab, oklabOf(pack(dim)), 0.18);
    }

    auto surface = [&](double dL, int tint, double amt) {
        Lab o = pageLab;
        o.L = clamp01(pageLab.L + dL);
        if (tint >= 0) o = mixLab(o, vs[(size_t)tint].lab, amt);
        return rgbFromOklab(o);
    };
    const int tint = tileIdx;
    const int hoverTint = washIdx >= 0 ? washIdx : tint;
    const Rgb hover = surface(light ? -0.06 : 0.08, hoverTint, washIdx >= 0 ? 0.34 : 0.22);
    const Rgb inp = surface(light ? -0.03 : 0.045, tint, 0.18);
    const Rgb ph = surface(light ? -0.05 : 0.065, ruleIdx >= 0 ? ruleIdx : tint, 0.14);

    const double amberHue = hueOf(oklabOf(pack(Rgb{224, 180, 40})));
    int redIdx = findHueVoice(vs, total, pageIdx, accentIdx, 0.0, 0.55, 0.012);
    int amberIdx = findHueVoice(vs, total, pageIdx, accentIdx, amberHue, 0.70, 0.012);
    Rgb errorC = redIdx >= 0 ? vs[(size_t)redIdx].rgb : Rgb{220, 70, 70};
    Rgb warning = amberIdx >= 0 ? vs[(size_t)amberIdx].rgb : Rgb{224, 180, 40};
    errorC = forceReadable(errorC, pageRgb, transRgb, 4.55, toWhite);
    warning = forceReadable(warning, pageRgb, transRgb, 4.55, toWhite);
    // A gold accent and the house amber are the same sentence. Walk the
    // warning toward red, and the error toward the ink pole, until each
    // stands apart from the accent. Hue moves; the role does not.
    for (int n = 0; n < 4 && distD(warning, accent) < 48; ++n)
        warning = forceReadable(mixD(Rgb{200, 55, 45}, warning, 0.35),
                                pageRgb, transRgb, 4.55, toWhite);
    for (int n = 0; n < 4 && distD(errorC, accent) < 48; ++n) {
        const Rgb away = light ? Rgb{90, 0, 0} : Rgb{255, 255, 255};
        errorC = forceReadable(mixD(errorC, away, 0.40), pageRgb, transRgb, 4.55, toWhite);
    }
    if (distD(errorC, accent) < 48 || distD(warning, accent) < 48) return false;
    if (contrastD(errorC, pageRgb) < 4.5 || contrastD(errorC, transRgb) < 4.5) return false;
    if (contrastD(warning, pageRgb) < 4.5 || contrastD(warning, transRgb) < 4.5) return false;

    Rgb q0, q1, q2, q3;
    if (!tuneMarks(trackRgb, accent, vs, total, pageIdx, accentIdx, q0, q1, q2, q3)) {
        trackLab = pageLab;
        trackLab.L = clamp01(pageLab.L + (light ? -0.04 : 0.05));
        trackRgb = rgbFromOklab(trackLab);
        if (!tuneMarks(trackRgb, accent, vs, total, pageIdx, accentIdx, q0, q1, q2, q3))
            return false;
    }

    Palette built;
    built.bgMain = pack(pageRgb);
    built.bgTrack = pack(trackRgb);
    built.bgSidebar = pack(sideRgb);
    built.bgTransport = pack(transRgb);
    built.textPrimary = pack(primary);
    built.textSecondary = pack(secondary);
    built.textDim = pack(dim);
    built.textAlbum = pack(album);
    built.accent = pack(accent);
    built.hover = pack(hover);
    built.separator = pack(rgbFromOklab(sepLab));
    built.inputBg = pack(inp);
    built.placeholder = pack(ph);
    built.tileMore = pack(rgbFromOklab(tileLab));
    built.error = pack(errorC);
    built.warning = pack(warning);
    built.qDsd = pack(q0);
    built.qDxd = pack(q1);
    built.qHires = pack(q2);
    built.qStandard = pack(q3);
    if (!paletteAcceptable(built)) return false;
    if (themeContrast(built.textAlbum, built.bgMain) < 4.5f) return false;
    if (themeChannelDist(built.accent, built.error) < 48) return false;
    built.id = "cover";
    built.name.clear();
    built.blurb.clear();
    built.group = themeDaypartId(themeDaypartFromLuma(themeLuma(built.bgMain)));
    out = std::move(built);
    return true;
}

bool paletteFromCover(const uint8_t* rgba, int w, int h, Palette& out) {
    std::vector<CoverVoice> vs = readCoverVoices(rgba, w, h);
    if (vs.empty()) return false;
    foldUnseen(vs);
    if (vs.empty()) return false;
    int total = 0;
    for (const CoverVoice& v : vs) total += v.n;
    const int pageIdx = pageVoice(vs);
    if (pageIdx < 0 || total < 16) return false;

    // The page walks toward its own pole only as far as the ladder needs.
    // Step 0 is the sleeve's color, exact, with no OKLab round trip.
    for (int step = 0; step <= 6; ++step) {
        Palette built;
        if (!coverPaletteAt(vs, total, pageIdx, (step / 6.0) * 0.85, built))
            continue;
        out = std::move(built);
        return true;
    }
    return false;
}

ThemeCopyResult copyThemeFiles(const std::string& fromDir, const std::string& toDir) {
    ThemeCopyResult result;
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(toDir, ec);
    if (ec) return result;
    fs::directory_iterator it(fs::path(fromDir), ec);
    if (ec) return result;
    std::vector<fs::path> files;
    for (; it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) break;
        std::error_code fec;
        if (!it->is_regular_file(fec) || fec) continue;
        const std::string ext = extLower(it->path());
        if (ext == ".theme" || ext == ".toml")
            files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    for (const fs::path& src : files) {
        const fs::path dest = fs::path(toDir) / src.filename();
        std::error_code existsEc;
        if (fs::exists(dest, existsEc) && !existsEc) {
            ++result.already;
            continue;
        }
        std::error_code copyEc;
        fs::copy_file(src, dest, copyEc);
        if (!copyEc) ++result.copied;
    }
    return result;
}

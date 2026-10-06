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

int reloadImportedThemes(const std::string& dir) {
    ensureBuilt();
    resetToBuiltins();
    int added = 0;
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        fprintf(stderr, "[Theme] could not create %s (%s)\n",
                dir.c_str(), ec.message().c_str());
        reindexActive();
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
            continue;
        }
        const std::string text((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        Palette parsed;
        std::string error;
        const std::string stem = path.stem().string();
        if (!parseThemeText(text, stem, parsed, error)) {
            fprintf(stderr, "[Theme] %s: %s\n", path.string().c_str(), error.c_str());
            continue;
        }
        if (isBuiltinId(parsed.id)) {
            fprintf(stderr, "[Theme] %s: id \"%s\" is built in, left unchanged\n",
                    path.string().c_str(), parsed.id.c_str());
            continue;
        }
        bool dup = false;
        for (const Palette& have : imports)
            if (have.id == parsed.id) { dup = true; break; }
        if (dup) {
            fprintf(stderr, "[Theme] %s: id \"%s\" already imported\n",
                    path.string().c_str(), parsed.id.c_str());
            continue;
        }
        fprintf(stderr, "[Theme] loaded %s from %s\n",
                parsed.id.c_str(), path.string().c_str());
        imports.push_back(std::move(parsed));
    }
    std::sort(imports.begin(), imports.end(),
              [](const Palette& a, const Palette& b) { return a.id < b.id; });

    // Insert so a group stays one run. An import that names an existing group
    // joins the end of that run; a new group is appended. Processing in id
    // order keeps each of those runs sorted.
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
    reindexActive();
    return added;
}

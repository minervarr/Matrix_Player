#pragma once
#include "color.hh"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

// The chrome tokens are the ACTIVE theme. They start as Matrix — the look this
// app shipped with — and applyTheme() replaces them. Every draw reads them
// through toColor() at draw time, so a pick recolors the next frame.
//
// Quality-tier colors live in the same theme because Paper (a white page)
// cannot use the neon marks that were chosen for black. qualityColorFor()
// reads the live values. The thresholds do not change.

inline ColorRef CLR_BG_MAIN         = RGB(10, 10, 10);
inline ColorRef CLR_BG_SIDEBAR      = RGB(18, 18, 18);
inline ColorRef CLR_BG_TRANSPORT    = RGB(22, 22, 22);
inline ColorRef CLR_BG_TRACKPANEL   = RGB(14, 14, 14);
// Text ladder, ORDERED by contrast against CLR_BG_MAIN:
// primary loudest, then secondary, then dim. Matrix is 242 / 170 / 128.
// Dim on the transport bar is the tightest pair in Matrix (4.58:1).
inline ColorRef CLR_TEXT_PRIMARY     = RGB(242, 242, 242);
inline ColorRef CLR_TEXT_SECONDARY   = RGB(170, 170, 170);
inline ColorRef CLR_TEXT_DIM         = RGB(128, 128, 128);
inline ColorRef CLR_ACCENT           = RGB(0, 200, 83);
inline ColorRef CLR_HOVER            = RGB(38, 38, 38);
inline ColorRef CLR_SEPARATOR        = RGB(36, 36, 36);
inline ColorRef CLR_INPUT_BG         = RGB(24, 24, 24);
inline ColorRef CLR_TILE_PLACEHOLDER = RGB(28, 28, 28);
inline ColorRef CLR_TEXT_ALBUM_TITLE = RGB(255, 255, 255);
// Mosaic "and more" quadrant. Information, never the state accent.
inline ColorRef CLR_TILE_MORE_GREEN  = RGB(30, 104, 62);
inline ColorRef CLR_ERROR            = RGB(255, 110, 110);
inline ColorRef CLR_WARNING          = RGB(224, 180, 40);

static constexpr float UI_CORNER_RADIUS = 0.0f;

static constexpr float SP_XS =  6.0f;
static constexpr float SP_SM = 13.0f;
static constexpr float SP_MD = 19.0f;
static constexpr float SP_LG = 32.0f;
static constexpr float SP_XL = 65.0f;

static constexpr float UI_SELECT_TINT_ALPHA = 0.16f;

inline ColorRef CLR_QUALITY_DSD      = RGB(255, 255, 255);
inline ColorRef CLR_QUALITY_DXD      = RGB(255, 165, 0);
inline ColorRef CLR_QUALITY_HIRES    = RGB(0, 255, 255);
inline ColorRef CLR_QUALITY_STANDARD = RGB(255, 255, 0);

struct QualityColor {
    bool     hasColor = false;
    ColorRef color    = 0;
};

// sampleRate in Hz. isDsd wins. Below 44.1 kHz there is no tier.
// One consumer: the per-track quality mark in the album view.
inline QualityColor qualityColorFor(int sampleRate, bool isDsd) {
    if (isDsd)                return { true, CLR_QUALITY_DSD };
    if (sampleRate >= 352800) return { true, CLR_QUALITY_DXD };
    if (sampleRate >= 64000)  return { true, CLR_QUALITY_HIRES };
    if (sampleRate >= 44100)  return { true, CLR_QUALITY_STANDARD };
    return { false, 0 };
}

// ── Themes ──────────────────────────────────────────────────────────────────
// The chrome tokens above are one palette, whichever applyTheme() last copied
// in. The catalogue itself — the built-in palettes, plus any file dropped
// beside the database — lives in theme_catalog.cc. Strings, not literals,
// because an imported file owns its name.
//
// Dark built-ins share one error red, (255,110,110). The old (220,70,70)
// drops under 4.5:1 on Dracula's and Pink's transport bars. Paper uses a
// darker red because that same bright red disappears on white.

struct Palette {
    std::string id;
    std::string group;
    std::string name;
    std::string blurb;
    ColorRef bgMain = 0, bgTrack = 0, bgSidebar = 0, bgTransport = 0;
    ColorRef textPrimary = 0, textSecondary = 0, textDim = 0, textAlbum = 0;
    ColorRef accent = 0, hover = 0, separator = 0, inputBg = 0, placeholder = 0, tileMore = 0;
    ColorRef error = 0, warning = 0;
    ColorRef qDsd = 0, qDxd = 0, qHires = 0, qStandard = 0;
};

inline float themeChannel(int c) {
    const float s = (float)c / 255.0f;
    return s <= 0.04045f ? s / 12.92f
                         : std::pow((s + 0.055f) / 1.055f, 2.4f);
}
inline float themeLuma(ColorRef c) {
    return 0.2126f * themeChannel(GetRValue(c))
         + 0.7152f * themeChannel(GetGValue(c))
         + 0.0722f * themeChannel(GetBValue(c));
}
inline float themeContrast(ColorRef a, ColorRef b) {
    const float l1 = themeLuma(a), l2 = themeLuma(b);
    const float hi = std::max(l1, l2), lo = std::min(l1, l2);
    return (hi + 0.05f) / (lo + 0.05f);
}

// Sum of per-channel distances. The contrast ladder can be numerically
// ordered and still be three greys a couple of levels apart; a listener
// cannot tell those apart, so the ladder also has to move.
inline int themeChannelDist(ColorRef a, ColorRef b) {
    return std::abs((int)GetRValue(a) - (int)GetRValue(b))
         + std::abs((int)GetGValue(a) - (int)GetGValue(b))
         + std::abs((int)GetBValue(a) - (int)GetBValue(b));
}

// Body text and the state accent clear 4.5:1 on the page and on the transport
// bar. The text ladder is ordered by that contrast, and the steps between
// primary, secondary and dim are wide enough to see. Quality marks are
// small, so they clear 3:1 on the track page.
inline bool paletteAcceptable(const Palette& p) {
    auto body = [&](ColorRef fg) {
        return themeContrast(fg, p.bgMain) >= 4.5f
            && themeContrast(fg, p.bgTransport) >= 4.5f;
    };
    if (!body(p.textPrimary) || !body(p.textSecondary) || !body(p.textDim))
        return false;
    const float cp = themeContrast(p.textPrimary, p.bgMain);
    const float cs = themeContrast(p.textSecondary, p.bgMain);
    const float cd = themeContrast(p.textDim, p.bgMain);
    if (!(cp > cs && cs > cd)) return false;
    if (themeChannelDist(p.textPrimary, p.textSecondary) < 24) return false;
    if (themeChannelDist(p.textSecondary, p.textDim) < 18) return false;
    if (!body(p.accent) || !body(p.error) || !body(p.warning)) return false;
    auto mark = [&](ColorRef fg) {
        return themeContrast(fg, p.bgTrack) >= 3.0f;
    };
    return mark(p.qDsd) && mark(p.qDxd) && mark(p.qHires) && mark(p.qStandard);
}

// `t` is the fraction of `b`. 0 leaves `a`, 1 is `b`. Channels stay in range.
inline ColorRef mixTheme(ColorRef a, ColorRef b, float t) {
    auto ch = [t](int ca, int cb) {
        int v = (int)std::lround((double)ca + ((double)cb - (double)ca) * (double)t);
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        return v;
    };
    return RGB((uint8_t)ch(GetRValue(a), GetRValue(b)),
               (uint8_t)ch(GetGValue(a), GetGValue(b)),
               (uint8_t)ch(GetBValue(a), GetBValue(b)));
}

// Secondary fills step from the page toward the type, so a blue page stays
// blue and a paper page stays paper. A flat lift toward white vanishes on
// Paper and washes the hue out of every other page. Primary is the accent;
// its hover steps back toward the page, which darkens a bright accent and
// lightens a black one.
inline ColorRef themeButtonFill(bool primary, bool hover) {
    if (primary)
        return hover ? mixTheme(CLR_ACCENT, CLR_BG_MAIN, 0.18f) : CLR_ACCENT;
    return mixTheme(CLR_BG_MAIN, CLR_TEXT_PRIMARY, hover ? 0.24f : 0.14f);
}

// A hairline. A separator that already clears 1.2:1 on the page is that
// page's own rule (Matrix, Paper and Hyper all do, and stay pixel-identical).
// One that would vanish into the page is walked toward the dim text until it
// clears about 1.35:1. Search underlines stay on CLR_SEPARATOR: those are
// controls, and this is structure. Settings radios and switches are bitmap
// marks (pixel_marks.hh) and take their idle ink from the text ladder.
inline ColorRef themeRule() {
    if (themeContrast(CLR_SEPARATOR, CLR_BG_MAIN) >= 1.2f)
        return CLR_SEPARATOR;
    ColorRef best = CLR_SEPARATOR;
    for (int i = 1; i <= 20; ++i) {
        best = mixTheme(CLR_SEPARATOR, CLR_TEXT_DIM, i / 20.0f);
        if (themeContrast(best, CLR_BG_MAIN) >= 1.35f) break;
    }
    return best;
}

inline void applyPalette(const Palette& p) {
    CLR_BG_MAIN = p.bgMain;
    CLR_BG_TRACKPANEL = p.bgTrack;
    CLR_BG_SIDEBAR = p.bgSidebar;
    CLR_BG_TRANSPORT = p.bgTransport;
    CLR_TEXT_PRIMARY = p.textPrimary;
    CLR_TEXT_SECONDARY = p.textSecondary;
    CLR_TEXT_DIM = p.textDim;
    CLR_TEXT_ALBUM_TITLE = p.textAlbum;
    CLR_ACCENT = p.accent;
    CLR_HOVER = p.hover;
    CLR_SEPARATOR = p.separator;
    CLR_INPUT_BG = p.inputBg;
    CLR_TILE_PLACEHOLDER = p.placeholder;
    CLR_TILE_MORE_GREEN = p.tileMore;
    CLR_ERROR = p.error;
    CLR_WARNING = p.warning;
    CLR_QUALITY_DSD = p.qDsd;
    CLR_QUALITY_DXD = p.qDxd;
    CLR_QUALITY_HIRES = p.qHires;
    CLR_QUALITY_STANDARD = p.qStandard;
}

// Catalogue. UI thread only — reloadImportedThemes rewrites the vector the
// Themes page walks. An unknown id leaves the applied tokens alone and
// returns false. A built-in that fails the contrast rules asserts in debug;
// an import that fails is refused at load and never reaches applyTheme.
int themeCount();
const Palette& themeAt(int index);
int activeThemeIndex();
const char* activeThemeId();
bool applyTheme(const char* id);
void checkBuiltinThemes();

// One file's text. `fallbackId` is the filename stem, used when the text does
// not name itself. On failure `error` says why and `out` is left untouched.
bool parseThemeText(const std::string& text, const std::string& fallbackId,
                    Palette& out, std::string& error);

// Built-ins, then every .theme / .toml file in `dir` (created if it can be).
// Returns how many files joined the list. Applied tokens stay as they are;
// activeThemeIndex() is -1 when the applied id is no longer listed.
int reloadImportedThemes(const std::string& dir);

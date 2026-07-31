#pragma once
#include "color.hh"

// Shared theme colors — used by player_view.cc (grid/transport/sidebar) and
// panels/settings_panels.cc (the four settings panels). Kept in one place so
// the panels visually match the rest of the custom-rendered UI instead of
// drifting into their own palette.
static constexpr ColorRef CLR_BG_MAIN        = RGB(10, 10, 10);
static constexpr ColorRef CLR_BG_SIDEBAR     = RGB(18, 18, 18);
static constexpr ColorRef CLR_BG_TRANSPORT   = RGB(22, 22, 22);
static constexpr ColorRef CLR_BG_TRACKPANEL  = RGB(14, 14, 14);
// ── Text-color ladder — ORDERED, and it must stay ordered ──────────────────
// PRIMARY 242 (17.7:1 on CLR_BG_MAIN) > SECONDARY 170 (8.5:1) > DIM 128 (5.0:1).
//
// This was previously INVERTED: DIM sat at 140 and SECONDARY at 128, so badges,
// hints and placeholders read LOUDER than artist names and durations — the
// opposite of the intent. The cause was raising DIM 80->140 for WCAG without
// re-spacing SECONDARY around it. Both values already cleared AA; the bug was
// purely ordering. Check the whole ladder before changing any one of them.
static constexpr ColorRef CLR_TEXT_PRIMARY    = RGB(242, 242, 242);
static constexpr ColorRef CLR_TEXT_SECONDARY  = RGB(170, 170, 170);
// TIGHTEST CONTRAST IN THE APP: the transport bar's DSP badge draws DIM on
// CLR_BG_TRANSPORT (22), which is 4.58:1 — above WCAG AA (4.5:1) but with
// almost no margin. Do not lower this without re-checking that pair.
static constexpr ColorRef CLR_TEXT_DIM        = RGB(128, 128, 128);
static constexpr ColorRef CLR_ACCENT          = RGB(0, 200, 83);
static constexpr ColorRef CLR_HOVER           = RGB(38, 38, 38);
static constexpr ColorRef CLR_SEPARATOR       = RGB(36, 36, 36);
static constexpr ColorRef CLR_INPUT_BG        = RGB(24, 24, 24);   // search / text-field fill
static constexpr ColorRef CLR_TILE_PLACEHOLDER = RGB(28, 28, 28);
static constexpr ColorRef CLR_TEXT_ALBUM_TITLE = RGB(255, 255, 255);
// The mosaic tile's "and more" quadrant (§8.2): a remix group with more than
// four members fades its last quadrant from the tile background into this
// green instead of showing a fifth cover.
//
// DELIBERATELY NOT CLR_ACCENT. This says "there is more here" — information,
// not state — and design principle #4 reserves the accent green for state
// alone. A deeper, less saturated green reads unmistakably as green against
// the near-black tile while never being mistaken for "this is playing". Keep
// the two far enough apart that a glance can tell them apart; if you retune
// this, check it beside a now-playing tile, which is where they meet.
static constexpr ColorRef CLR_TILE_MORE_GREEN  = RGB(30, 104, 62);
static constexpr ColorRef CLR_ERROR           = RGB(220, 70, 70);  // reserved (error UI)
// Amber/yellow, distinct from CLR_ERROR (reserved for hard/fatal failures,
// unused today) and CLR_ACCENT green (reserved for state/selection). For
// non-blocking warnings: something didn't go as configured but the app
// keeps running. RGB(224,180,40) is ~9.8:1 on CLR_BG_MAIN, ~8.2:1 on
// CLR_BG_TRANSPORT — comfortably above WCAG AA.
static constexpr ColorRef CLR_WARNING         = RGB(224, 180, 40);

// ── Shape ───────────────────────────────────────────────────────────────────
// Uniform corner radius for interactive chrome (buttons, hover/selection
// pills, search fields). The app uses a fully-square look — this is the single
// knob that enforces it everywhere. The only rounded shapes left are the
// circular radio dot and the decorative multi-layer tile glows (both drawn with
// their own radii, independent of this token).
static constexpr float UI_CORNER_RADIUS = 0.0f;

// ── Spacing scale ────────────────────────────────────────────────────────────
// Base pixel rhythm for pads/gaps/margins, authored at the 1080 reference
// height. Callers pass these through UiMetrics::space() (see ui_metrics.hh)
// rather than multiplying by hand. Prefer them over ad-hoc literals so the
// layout keeps a consistent step. See docs/UI_DESIGN_SYSTEM.md.
//
// These are the old 4/8/12/20/40 scale times the pre-rigor-pass factor at
// 1080 (1.63389), truncated the way the original (int) casts truncated — so
// every call site renders exactly as it did before, and the numbers now say
// on the page what they actually draw.
static constexpr float SP_XS =  6.0f;
static constexpr float SP_SM = 13.0f;
static constexpr float SP_MD = 19.0f;
static constexpr float SP_LG = 32.0f;
static constexpr float SP_XL = 65.0f;

// Selection tint alpha for the accent-on-state pill (lists, nav, playing row).
static constexpr float UI_SELECT_TINT_ALPHA = 0.16f;

// ── Audio-quality color tiers ────────────────────────────────────────────────
// A second, deliberately scoped palette for OBJECTIVE audio-quality metadata
// (sample rate / DSD), shown as a border/"aura" on album art and track lists
// — never for UI state (state stays CLR_ACCENT-only, see design principle
// #4 in docs/UI_DESIGN_SYSTEM.md). Thresholds/colors ported verbatim from
// the sibling Android player's CategoryAdapter/GroupedFragment quality-tier
// logic (see docs/superpowers/specs/2026-07-27-release-type-and-quality-color-design.md).
static constexpr ColorRef CLR_QUALITY_DSD      = RGB(255, 255, 255);
static constexpr ColorRef CLR_QUALITY_DXD      = RGB(255, 165, 0);   // #FFA500, >=352.8kHz
static constexpr ColorRef CLR_QUALITY_HIRES    = RGB(0, 255, 255);   // #00FFFF, >=64kHz
static constexpr ColorRef CLR_QUALITY_STANDARD = RGB(255, 255, 0);   // >=44.1kHz (CD quality)

struct QualityColor {
    bool     hasColor = false;
    ColorRef color    = 0;
};

// sampleRate in Hz (e.g. 44100, not 44.1). isDsd wins over sampleRate tiers.
// Below 44.1kHz there's no tier — this deliberately mirrors the Android
// reference's TRANSPARENT ("no border") case, not an error.
//
// ONE consumer: the per-track quality mark in the album view's track list
// (UiIcon::Quality). The album grid used to carry a tier-colored frame per
// tile as well; it was dropped — a color per tile said nothing worth the noise
// on a grid already dense with artwork, and it gets less meaningful still once
// a tile stands for a set of quality/edition variants (see TODO.md).
inline QualityColor qualityColorFor(int sampleRate, bool isDsd) {
    if (isDsd)                return { true, CLR_QUALITY_DSD };
    if (sampleRate >= 352800) return { true, CLR_QUALITY_DXD };
    if (sampleRate >= 64000)  return { true, CLR_QUALITY_HIRES };
    if (sampleRate >= 44100)  return { true, CLR_QUALITY_STANDARD };
    return { false, 0 };
}

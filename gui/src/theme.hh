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
static constexpr ColorRef CLR_TEXT_PRIMARY    = RGB(242, 242, 242);
static constexpr ColorRef CLR_TEXT_SECONDARY  = RGB(128, 128, 128);
// 140 (was 80): rgb(80) on the rgb(10) background was ~2.4:1 contrast —
// below WCAG AA's 4.5:1 for the real information it carries (quality badge,
// REF EQ, hints). 140 ≈ 5.6:1, still clearly de-emphasized vs SECONDARY.
static constexpr ColorRef CLR_TEXT_DIM        = RGB(140, 140, 140);
static constexpr ColorRef CLR_ACCENT          = RGB(0, 200, 83);
static constexpr ColorRef CLR_HOVER           = RGB(38, 38, 38);
static constexpr ColorRef CLR_SEPARATOR       = RGB(36, 36, 36);
static constexpr ColorRef CLR_SEEKBAR_TRACK   = RGB(55, 55, 55);
static constexpr ColorRef CLR_SEEKBAR_FILL    = RGB(0, 200, 83);
static constexpr ColorRef CLR_TILE_PLACEHOLDER = RGB(28, 28, 28);
static constexpr ColorRef CLR_TEXT_ALBUM_TITLE = RGB(255, 255, 255);
static constexpr ColorRef CLR_ERROR           = RGB(220, 70, 70);

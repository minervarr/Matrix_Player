#pragma once
#include <cstdint>

// Portable replacement for Win32's COLORREF/RGB()/GetRValue() family — a
// plain 0x00BBGGRR-packed uint32_t, same bit layout COLORREF always used, so
// every existing `CLR_*` constant and `RGB(r,g,b)` call site is unchanged.
using ColorRef = uint32_t;

constexpr ColorRef RGB(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<ColorRef>(r) | (static_cast<ColorRef>(g) << 8) |
           (static_cast<ColorRef>(b) << 16);
}

constexpr uint8_t GetRValue(ColorRef c) { return static_cast<uint8_t>(c & 0xFF); }
constexpr uint8_t GetGValue(ColorRef c) { return static_cast<uint8_t>((c >> 8) & 0xFF); }
constexpr uint8_t GetBValue(ColorRef c) { return static_cast<uint8_t>((c >> 16) & 0xFF); }

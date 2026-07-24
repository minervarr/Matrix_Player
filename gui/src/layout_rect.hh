#pragma once

// Portable replacement for Win32's RECT — same four int members, same names,
// so every existing `.left`/`.top`/`.right`/`.bottom` read/write and
// `r.right - r.left`-style width computation throughout player_view.cc's
// layout/hit-test code is unchanged. Real window/monitor handles (HWND,
// HMONITOR) stay behind the Host interface (host.hh); this only replaces the
// four-int rectangle POD.
struct LayoutRect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};

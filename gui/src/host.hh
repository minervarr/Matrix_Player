#pragma once
#include "layout_rect.hh"
#include <string>
#include <memory>
#include <cstdint>

#ifdef _WIN32
#include <windows.h>
#endif

// vk_canvas platform seams (framework/vk_canvas/core/platform.hh) — already
// portable base classes, so Host doesn't need to re-abstract rendering at
// all, only window/monitor/input/message-pump concerns.
struct SurfaceProvider;
struct AssetReader;

class PlayerWindow;

// Complete: today's full browsing UI, true fullscreen. Essential: a minimal
// "now playing" widget, phone-shaped, for monitors too short for Complete.
// Defined here (not player_view.hh) because Host's window-sizing methods
// need it too.
enum class UiMode { Essential, Complete };

struct MonitorInfo {
    LayoutRect bounds;    // full monitor bounds, px
    LayoutRect workArea;  // bounds minus taskbar/panels (Windows only; equals bounds elsewhere)
};

// Cross-thread notifications PlayerWindow's background threads (art-decode
// worker, background scan thread, gapless coordinator) need serviced on the
// UI thread — the portable equivalent of PostMessageW(hwnd_, WM_APP_*, ...).
// Windows: maps to its own internal WM_APP+N numbering in windows_host.cc.
// Linux: queued and drained from LinuxHost::pump().
enum class AppEvent { TrackChange, ScanDone, ArtDecoded, RequestPlay };

// Single repeating timer id PlayerWindow uses (playback position updates).
// Windows: SetTimer/KillTimer. Linux: a timerfd polled alongside the Wayland
// display fd in LinuxHost::pump().
enum class TimerId { SeekUpdate };

// Owns the real OS window handle and drives every window/monitor/input/
// message-pump concern PlayerWindow never touches directly — see
// os/windows_host.cc (moved almost verbatim from the pre-reorg
// create()/wndProc/handleMsg) and os/linux_host.cc (new, built on vk_canvas's
// platform/linux Wayland backend).
//
// Windows and Linux genuinely differ here, not just in mechanism: Wayland
// clients cannot set their own window position or query a monitor's "work
// area" (no taskbar/panel concept is exposed to clients) — so edge-snap
// (snapToEdge) and cross-monitor re-fit (adaptToCurrentMonitor) are windowing-
// shell features with no Wayland equivalent, not just a different API to call
// them through. Both are real no-ops on Linux, documented at the call site,
// not silently-do-nothing bugs. Complete mode's "true fullscreen" DOES have a
// real Wayland equivalent (xdg_toplevel's set_fullscreen()) and uses it.
class Host {
public:
    virtual ~Host() = default;

    // Portable executable-relative directory, UTF-8, trailing separator.
    // (GetModuleFileNameW-derived on Windows, /proc/self/exe on Linux —
    // same discovery FileAssetReader's own per-platform impl already uses.)
    virtual std::string exeDir() const = 0;

    // Creates the native window sized/positioned for the initial UiMode
    // (queried via primaryMonitor()/kMinWindowContentH by the caller before
    // this is called). owner's on*()/handle*() methods are what pump()
    // dispatches into. Returns false on failure.
    virtual bool init(PlayerWindow* owner, UiMode initialMode) = 0;

    virtual SurfaceProvider& surfaceProvider() = 0;
    virtual AssetReader&     assetReader()     = 0;

    virtual void showWindow() = 0;

    virtual MonitorInfo primaryMonitor() const = 0;

    // Re-applies sizing/positioning for a UI mode change (toggleUiMode()) —
    // fullscreen request on Linux, monitor-sized rect + SetWindowPos on
    // Windows. Calls owner->onHostResized() if the renderer needs to know.
    virtual void applyUiMode(UiMode mode) = 0;

    // Re-fits the window if it moved to a different monitor since the last
    // call. No-op on Linux (see class comment) — Wayland has no client-side
    // "which monitor am I on, reposition to fit" capability.
    virtual void adaptToCurrentMonitor(UiMode mode) = 0;

    // Alt+F/J/C/U/G/H edge-snap. No-op on Linux (see class comment).
    virtual void snapToEdge(int hotkeyId) = 0;

    virtual void invalidate() = 0;  // schedule a repaint

    // Cross-thread wakeup: safe to call from any thread. Dispatches into the
    // matching owner->on*() method from the UI thread, inside pump().
    virtual void postAppEvent(AppEvent id, intptr_t p1 = 0, intptr_t p2 = 0) = 0;

    virtual void startTimer(TimerId id, int intervalMs) = 0;
    virtual void stopTimer(TimerId id) = 0;

    // One iteration of the platform's message/event pump. Blocks up to
    // ~timeoutMs if haveWork is false, else processes what's ready and
    // returns promptly. Dispatches input into owner's on*() methods.
    virtual void pump(bool haveWork) = 0;
    virtual bool quitRequested() const = 0;

    // Startup-failure fallback (Vulkan init failure, USB DAC not found).
    // Phase 7 replaces this with a real vk_canvas panel; until then this is
    // a native MessageBox on Windows, a stderr log line on Linux.
    virtual void showErrorMessage(const std::string& title, const std::string& msg) = 0;

#ifdef _WIN32
    // Only exists on Windows: player_view.cc's TRACKMOUSEEVENT (mouse-leave
    // detection) needs a real HWND — vk_canvas has no portable equivalent.
    // Not part of the portable surface.
    virtual HWND nativeHandle() const = 0;
#endif
};

std::unique_ptr<Host> make_host();

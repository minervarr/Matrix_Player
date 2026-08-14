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

// Pointer images the UI distinguishes. Kept to what a hit-test can honestly
// answer — "this is clickable", "you can type here" — and deliberately
// separate from vk_canvas's own CursorShape so the portable seam doesn't
// depend on the Wayland backend's header; linux_host.cc maps between them.
enum class CursorShape { Arrow, Hand, Text };

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

    // Creates the native window, sized to the monitor (true fullscreen).
    // owner's on*()/handle*() methods are what pump() dispatches into.
    // Returns false on failure.
    //
    // There is no mode argument any more: which LAYOUT the app draws is
    // derived from the window's own shape (see ui_orientation.hh) and is none
    // of the host's business. That removal is also what killed applyUiMode(),
    // whose Wayland implementation could only ask the compositor for a
    // fullscreen and wait for an asynchronous configure to come back.
    virtual bool init(PlayerWindow* owner) = 0;

    virtual SurfaceProvider& surfaceProvider() = 0;
    virtual AssetReader&     assetReader()     = 0;

    // Reads the app's READ-ONLY shipped data — today that is fonts/ and
    // nothing else — at paths relative to exeDir().
    //
    // Separate from assetReader(), which vk_canvas roots at <exe>/assets/ for
    // its shaders. On both desktops this is a plain filesystem read and the
    // two could have been one; on Android they genuinely differ in kind,
    // because the faces are not files at all — they live inside the APK and
    // come out through AAssetManager. Routing the font opens through here is
    // what lets PlayerWindow::create() load its typeface with no #ifdef and
    // no knowledge that an APK exists: exeDir() is "" on Android, so
    // exeDir() + "fonts/…" is exactly the asset name.
    //
    // NOT for the music library. Album art and audio files are ordinary
    // absolute paths on every platform, and go on reading through
    // FileByteReader directly.
    virtual AssetReader&     dataReader()      = 0;

    // Opaque per-platform handle so a second top-level window (ArtWindow)
    // can share the platform's connection instead of opening an
    // independent one of its own. Windows: unused (nullptr) — ArtWindow
    // builds a fully independent HWND from scratch, no dependency on Host
    // at all. Linux: the WaylandDisplay* the main window's WaylandWindow
    // was created on, so ArtWindow's second WaylandWindow shares the one
    // Wayland connection instead of opening a second (wasteful/wrong — one
    // process, one compositor connection).
    virtual void* secondaryWindowHandle() { return nullptr; }

    virtual void showWindow() = 0;

    virtual MonitorInfo primaryMonitor() const = 0;

    // Re-fits the window if it moved to a different monitor since the last
    // call. No-op on Linux (see class comment) — Wayland has no client-side
    // "which monitor am I on, reposition to fit" capability.
    virtual void adaptToCurrentMonitor() = 0;

    // Alt+F/J/C/U/G/H edge-snap. No-op on Linux (see class comment).
    virtual void snapToEdge(int hotkeyId) = 0;

    virtual void invalidate() = 0;  // schedule a repaint

    // Pointer image over the main window. Called from hit-testing every time
    // the hovered element changes; both backends collapse a repeat of the
    // shape already showing, so callers need not track it themselves.
    virtual void setCursor(CursorShape shape) = 0;

    // Hold the display awake (fullscreen artwork). SetThreadExecutionState on
    // Windows; a Wayland idle inhibitor on Linux, which needs the compositor
    // to expose zwp_idle_inhibit_manager_v1 — a no-op where it doesn't, since
    // nothing else can ask.
    virtual void setKeepAwake(bool on) = 0;

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

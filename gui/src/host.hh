#pragma once
#include "app_view.hh"
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

// Pointer images the UI distinguishes. Kept to what a hit-test can honestly
// answer — "this is clickable", "you can type here" — and deliberately
// separate from vk_canvas's own CursorShape so the portable seam doesn't
// depend on the Wayland backend's header; linux_host.cc maps between them.
enum class CursorShape { Arrow, Hand, Text };

struct MonitorInfo {
    LayoutRect bounds;    // full monitor bounds, px
    LayoutRect workArea;  // bounds minus taskbar/panels (Windows only; equals bounds elsewhere)
};

// Pixels along each edge of OUR OWN window that the app must not draw in,
// because the hardware or the system already owns them.
//
// Zero on both desktops, and that is not a stub — a desktop window is given a
// rectangle it owns completely. A phone is not: the camera is a hole punched
// through the glass in the middle of the display, and it is still there when
// every system bar is hidden. Nothing can be drawn under it and read.
//
// The distinction that makes this necessary rather than merely tidy: system
// bars are SOFTWARE and can be hidden, so they never belong here — hiding them
// is the right answer and an inset would be a worse one. The cutout is GLASS.
// Measured on a moto g06: hiding the bars extends the window to the full
// 720x1640 panel and puts the app's navigation rail under a 70 px camera
// notch, which is exactly the case this exists for.
struct SafeInsets {
    int top = 0, bottom = 0, left = 0, right = 0;
};

// ── Two vocabularies the host carries and never reads ────────────────────────
//
// Cross-thread notifications an app's background threads need serviced on the
// UI thread — the portable equivalent of PostMessageW(hwnd_, WM_APP_*, ...) —
// and the ids of the repeating timers it asks for.
//
// Both are the APPLICATION's enums, passed through as plain integers. A host
// queues an event and hands the same three numbers back to
// AppView::onAppEvent(); it starts a timer and hands the same id back to
// AppView::onTimer(). Neither ever branches on the value.
//
// They used to be enums declared right here — `AppEvent{TrackChange, ScanDone,
// ArtDecoded, RequestPlay}` and `TimerId{SeekUpdate}` — which is a music
// player's vocabulary sitting in the one file that is supposed to know nothing
// about music. Worse, each backend then had to switch on it, so the same
// dispatch was written out four times over.
//
// Windows maps the event id onto its own WM_APP+N range; Linux and Android
// queue it and drain it from pump(). Windows is also the only backend that
// currently distinguishes timer ids at all (SetTimer needs a real id); the
// other two have one timer fd and ignore the value, which is a narrowing to fix
// on the day a second timer exists, not before.

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
    virtual bool init(AppView* owner) = 0;

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

    // See SafeInsets. Defaulted rather than pure so the two desktop hosts and
    // the headless one need no implementation at all: a window they are given
    // is a window they own, and the honest answer is zero on every edge.
    // Re-read on every layout pass, never cached by the caller — a rotation or
    // a fold changes it without the app restarting.
    virtual SafeInsets safeInsets() const { return {}; }

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

    // Cross-thread wakeup: safe to call from any thread. The three integers
    // come back out of AppView::onAppEvent() unread, on the UI thread, inside
    // pump().
    virtual void postAppEvent(int id, intptr_t p1 = 0, intptr_t p2 = 0) = 0;

    virtual void startTimer(int id, int intervalMs) = 0;
    virtual void stopTimer(int id) = 0;

    // What the platform was launched WITH, if anything — Android reads it off
    // the activity's intent. Empty on both desktops, and empty is the honest
    // answer there rather than a stub: a desktop app is started by a user who
    // is about to tell it what to do.
    //
    // The host states the fact and stops. What it MEANS is the app's business,
    // decided in AppView::onHostReady(); this is the seam that replaced
    // AndroidHost calling the music player's own commitAddFolder().
    virtual std::string launchArgument() const { return {}; }

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

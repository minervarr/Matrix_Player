#pragma once
#include <android_native_app_glue.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "android_platform.hh"  // AndroidSurfaceProvider, AndroidAssetReader
#include "host.hh"              // gui/src: the seam PlayerWindow talks to the OS through

// The THIRD implementation of gui/src/host.hh's Host, beside WindowsHost and
// LinuxHost (and the headless one tools/ui_capture builds). Android therefore
// runs the REAL PlayerWindow — the same bars, grid, album view, guided search
// and settings panels the desktop draws, out of the same player_view.cc.
//
// This file used to say the opposite, and so did CLAUDE.md: Android had its
// own small AndroidPlayerView because "a touch phone UI is genuinely different
// from a sidebar-and-grid desktop one". What that produced was a lookalike
// that had to be rebuilt feature by feature, and it was dropped once the
// premise was actually checked — player_view.cc includes no OS header at all,
// and PlayerWindow::run() is a loop over host_->pump(). The app was already
// portable; only this class was missing.
//
// What Android genuinely cannot do is answered honestly rather than faked:
// there is no second monitor to snap a window to, no cursor to shape, and no
// window position a client may set. Those are no-ops here exactly as several
// of them already are on Wayland.
//
// Still missing, and NOT hidden: there is no keyboard, so onCharPortable() is
// never fed and the guided-search box can be seen but not typed into. Wiring
// the IME is separate work.
class AndroidHost : public Host {
public:
    // `launchExtraKey` names the intent extra launchArgument() should return
    // — "scan_root" for a music player, something else for something else, and
    // nullptr for an app that is not launched with an argument at all. It is a
    // constructor parameter rather than a constant here because naming it was
    // the LAST piece of one application's vocabulary left inside this host.
    // `fallback` is what to return when the extra is absent or empty.
    explicit AndroidHost(android_app* state,
                         const char* launchExtraKey = nullptr,
                         const char* fallback       = nullptr);
    ~AndroidHost() override;

    // ── Host ────────────────────────────────────────────────────────────────
    std::string exeDir() const override;

    // Pumps the looper until Android hands us a window, then builds the
    // surface/asset seams over it. Everything after that — Vulkan, the DB, the
    // fonts, the layout — is PlayerWindow::create()'s ordinary work, unchanged.
    bool init(AppView* owner) override;

    SurfaceProvider& surfaceProvider() override { return *surface_; }
    AssetReader&     assetReader()     override { return *assets_; }
    // On Android these two are NOT interchangeable in principle but ARE the
    // same object: both the shaders and the faces live inside the APK, behind
    // one AAssetManager. See host.hh for why the distinction exists at all.
    AssetReader&     dataReader()      override { return *assets_; }

    void showWindow() override {}
    MonitorInfo primaryMonitor() const override;
    // The display cutout, and ONLY the cutout — see the definition. The system
    // bars are hidden rather than avoided, so they contribute nothing here.
    SafeInsets safeInsets() const override;
    void adaptToCurrentMonitor() override {}   // one screen, and we do not place ourselves
    void snapToEdge(SnapEdge) override {}      // no window position to set
    void invalidate() override {}              // run()'s dirty flag already covers it
    void setCursor(CursorShape) override {}    // a finger has no shape to change
    void setKeepAwake(bool on) override;

    void postAppEvent(int id, intptr_t p1 = 0, intptr_t p2 = 0) override;
    void startTimer(int id, int intervalMs) override;
    void stopTimer(int id) override;
    std::string launchArgument() const override;

    void pump(bool haveWork) override;
    bool quitRequested() const override;

    void showErrorMessage(const std::string& title, const std::string& msg) override;

private:
    struct Event { int id; intptr_t p1, p2; };

    static void    handleAppCmd(android_app* app, int32_t cmd);
    static int32_t handleInputEvent(android_app* app, AInputEvent* event);

    void onWindowInit();
    void onWindowTerm();
    void onGainedFocus();
    void onResume();

    void dispatchAppEvent(const Event& e);
    void drainEvents();
    void drainTimer();

    // ── Touch, translated into the pointer events PlayerWindow expects ──────
    //
    // A finger is not a mouse, and the difference is not cosmetic: a drag must
    // scroll and must NOT press whatever was under it when it started. Below
    // the slop the gesture is a tap and arrives as onMouseMove + a click at
    // release; past the slop it becomes wheel deltas and the click is
    // cancelled for good, even if the finger comes back.
    void onTouchDown(float x, float y);
    void onTouchMove(float x, float y);
    void onTouchUp(float x, float y, bool cancelled);

    // Asks for "All files access" IF it is not already granted, and at most
    // once per process. Both halves matter — see the definition: the call it
    // replaces was unconditional and ran on every window init, which is a
    // loop, not a prompt.
    void ensureStoragePermission();
    // Makes the launch intent's scan root a music root in the DB the first
    // time it can, then lets PlayerWindow's own background scan do the work.
    void maybeSeedMusicRoot();

    android_app*  state_ = nullptr;
    AppView*    owner_          = nullptr;
    const char* launchKey_      = nullptr;
    const char* launchFallback_ = nullptr;
    int      timerId_ = 0;

    std::unique_ptr<AndroidSurfaceProvider> surface_;
    std::unique_ptr<AndroidAssetReader>     assets_;

    // False until the first pump(), which is the first moment
    // PlayerWindow::create() is known to have RETURNED. Before that the app is
    // half-built: it has no Renderer yet and, more dangerously, no open
    // database — `db_` is opened a few lines after host_->init() returns.
    //
    // This is not a precaution. Android delivers APP_CMD_RESUME while init()
    // is still pumping for its first window, and onResume() reaching into
    // PlayerWindow at that moment dereferenced a null sqlite3* and killed the
    // process before anything was ever drawn: SIGSEGV in Db::loadMusicRoots(),
    // three frames under AndroidHost::init(). Every path from a system
    // callback INTO the app must be gated on this.
    bool appReady_ = false;

    // Cross-thread events, exactly LinuxHost's shape: a queue plus an eventfd
    // registered on the looper, so a background thread finishing a scan or an
    // art decode wakes a blocked pump().
    std::mutex         eventsMu_;
    std::vector<Event> events_;
    int                eventFd_ = -1;
    int                timerFd_ = -1;

    // Touch state (one finger; this app has no pinch or two-finger gesture).
    float touchStartX_ = 0.0f, touchStartY_ = 0.0f;
    float touchLastY_  = 0.0f;
    bool  touchDragging_ = false;
    bool  touchDown_     = false;
    std::chrono::steady_clock::time_point lastTapTime_;
    float lastTapX_ = 0.0f, lastTapY_ = 0.0f;
    bool  lastTapValid_ = false;

    bool storageAsked_ = false;
    bool rootSeeded_   = false;

    // The cutout, cached because safeInsets() is called from every layout pass
    // and answering it costs a chain of five JNI calls. Refreshed where the
    // answer can actually change: window init, and every resize (which is what
    // a rotation and a fold both arrive as).
    SafeInsets cachedInsets_{};
    void refreshSafeInsets();
};

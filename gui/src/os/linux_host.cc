// Linux Host implementation — real Wayland window/surface/input via
// framework/vk_canvas/platform/linux (WaylandDisplay/WaylandWindow), no
// stubbing. See ../host.hh's class comment for what genuinely has no
// Wayland equivalent (edge-snap, cross-monitor re-fit) vs what does
// (Complete mode's fullscreen, via xdg_toplevel's set_fullscreen()).
#include "host.hh"
#include "app_paths.hh"
#include "player_view.hh"
#include "wayland_platform.hh"
#include "wayland_display.hh"
#include "wayland_window.hh"
#include "renderer.hh"

#include <sys/timerfd.h>
#include <unistd.h>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <chrono>
#include <mutex>
#include <vector>

namespace {
// Default surface size, used until the compositor's first configure lands and
// as the primaryMonitor() fallback when no output has been advertised yet
// (Wayland has no "work area" concept — see host.hh).
constexpr int kDefaultW = 1200;
constexpr int kDefaultH = 700;
}

class LinuxHost : public Host, public InputSink {
public:
    // The Wayland connection (and its output/monitor list) is established
    // here, not in init(), so primaryMonitor() can be answered before any
    // window exists — the same way Windows' MonitorFromPoint can.
    LinuxHost() : display_(std::make_unique<WaylandDisplay>()) {}

    ~LinuxHost() override {
        if (seekTimerFd_ >= 0) ::close(seekTimerFd_);
    }

    std::string exeDir() const override { return app_paths::exeDir(); }

    bool init(PlayerWindow* owner) override {
        owner_ = owner;
        if (!display_->valid()) {
            fprintf(stderr, "[LinuxHost] Failed to connect to Wayland display\n");
            return false;
        }

        window_ = std::make_unique<WaylandWindow>(*display_, "Matrix Player",
                                                   "matrix-player", kDefaultW, kDefaultH);
        if (!window_->valid()) {
            fprintf(stderr, "[LinuxHost] Failed to create Wayland window\n");
            return false;
        }
        display_->set_sink(window_->surface(), this);

        surfaceProvider_ = std::make_unique<WaylandSurfaceProvider>(*display_, *window_);

        // Fullscreen unconditionally: Complete was the only surviving mode, so
        // this is what the app has always done. The compositor answers with a
        // configure asynchronously (unlike Windows' synchronous SetWindowPos),
        // and pump() calls onHostResized() once that resize actually lands.
        window_->set_fullscreen(nullptr);

        seekTimerFd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        return true;
    }

    SurfaceProvider& surfaceProvider() override { return *surfaceProvider_; }
    AssetReader&     assetReader()     override { return assets_; }
    // fonts/ is a plain directory next to the executable here, so the trivial
    // filesystem reader is the whole implementation. Android is where this
    // and assetReader() stop being interchangeable — see host.hh.
    AssetReader&     dataReader()      override { return dataReader_; }

    void* secondaryWindowHandle() override { return display_.get(); }

    void showWindow() override {
        // Wayland windows are visible once the first frame commits — there
        // is no separate "make visible" call the way Windows' ShowWindow is.
    }

    MonitorInfo primaryMonitor() const override {
        MonitorInfo mi{};
        const auto& outputs = display_->outputs();
        if (!outputs.empty()) {
            // Wayland has no "primary" output concept; index 0 by
            // convention (see WaylandDisplay's own doc comment).
            const auto& o = outputs[0];
            mi.bounds = { o.x, o.y, o.x + o.width, o.y + o.height };
        } else {
            mi.bounds = { 0, 0, kDefaultW, kDefaultH };
        }
        mi.workArea = mi.bounds;  // no work-area concept on Wayland
        return mi;
    }

    void adaptToCurrentMonitor() override {
        // No-op: Wayland clients cannot query "which monitor am I nearest"
        // or reposition themselves — see host.hh's class comment.
    }

    void snapToEdge(int) override {
        // No-op: Wayland clients cannot set their own window position.
    }

    void invalidate() override {
        // The dirty-flag render loop (PlayerWindow::run()) already redraws
        // on its own next iteration; nothing platform-level to poke.
    }

    void setCursor(CursorShape shape) override {
        if (!window_) return;
        // Two enums on purpose: host.hh's is the portable one, WaylandDisplay's
        // belongs to the backend. This is the only place they meet.
        using WlShape = WaylandDisplay::CursorShape;
        WlShape want = WlShape::Arrow;
        switch (shape) {
        case CursorShape::Hand: want = WlShape::Hand; break;
        case CursorShape::Text: want = WlShape::Text; break;
        case CursorShape::Arrow: break;
        }
        display_->set_cursor_shape(window_->surface(), want);
    }

    void setKeepAwake(bool on) override {
        if (!window_) return;
        display_->set_idle_inhibited(window_->surface(), on);
    }

    void postAppEvent(AppEvent id, intptr_t p1, intptr_t p2) override {
        {
            std::lock_guard<std::mutex> lk(eventsMu_);
            events_.push_back({id, p1, p2});
        }
        display_->waker().wake();
    }

    void startTimer(TimerId, int intervalMs) override {
        // Only one timer id exists today (SeekUpdate) — see host.hh.
        itimerspec spec{};
        spec.it_value.tv_sec  = intervalMs / 1000;
        spec.it_value.tv_nsec = (intervalMs % 1000) * 1000000L;
        spec.it_interval = spec.it_value;
        timerfd_settime(seekTimerFd_, 0, &spec, nullptr);
    }

    void stopTimer(TimerId) override {
        itimerspec spec{};
        timerfd_settime(seekTimerFd_, 0, &spec, nullptr);
    }

    void pump(bool haveWork) override {
        if (window_->closed()) { owner_->shutdown(); quit_ = true; return; }

        if (!display_->dispatch(haveWork ? 0 : 250)) {
            owner_->shutdown();
            quit_ = true;
            return;
        }

        if (window_->take_resized()) owner_->onHostResized();

        // Drain the seek-update timerfd (armed by startTimer()).
        uint64_t expirations = 0;
        if (read(seekTimerFd_, &expirations, sizeof(expirations)) > 0)
            owner_->onTimer();

        // Drain cross-thread app events posted via postAppEvent().
        std::vector<Event> pending;
        {
            std::lock_guard<std::mutex> lk(eventsMu_);
            pending.swap(events_);
        }
        for (auto& e : pending) dispatchAppEvent(e);
    }

    bool quitRequested() const override { return quit_; }

    void showErrorMessage(const std::string& title, const std::string& msg) override {
        fprintf(stderr, "[%s] %s\n", title.c_str(), msg.c_str());
    }

    // ── InputSink (framework/vk_canvas/core/input.hh) ──────────────────────
    void onPointer(const PointerEvent& e) override {
        switch (e.action) {
        case PointerAction::Move:
            owner_->onMouseMove((int)e.x, (int)e.y);
            break;
        case PointerAction::Down:
            if (e.button == 0) {
                // Synthesize double-click (Wayland/InputSink has no native
                // dblclk event, unlike Win32's WM_LBUTTONDBLCLK) — same
                // ~400ms/small-radius heuristic most toolkits use.
                using Clock = std::chrono::steady_clock;
                auto now = Clock::now();
                bool isDouble = lastDownValid_ &&
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - lastDownTime_).count() < 400 &&
                    std::abs((int)e.x - lastDownX_) < 4 && std::abs((int)e.y - lastDownY_) < 4;
                lastDownTime_ = now;
                lastDownX_ = (int)e.x; lastDownY_ = (int)e.y;
                lastDownValid_ = true;
                dragStartX_ = (int)e.x; dragStartY_ = (int)e.y;
                dragValid_  = true;
                if (isDouble) {
                    owner_->onLButtonDblClk((int)e.x, (int)e.y);
                    lastDownValid_ = false;  // don't chain a third click into another dblclk
                } else {
                    owner_->onLButtonDown((int)e.x, (int)e.y);
                }
            } else if (e.button == 3) {
                owner_->onNavBack();      // the mouse's back button — see input.hh
            } else if (e.button == 4) {
                owner_->onNavForward();
            }
            break;
        case PointerAction::Up:
            // A drag that has ended. The 4 px is only "the pointer actually
            // moved" — whether the stroke was long enough to MEAN anything is
            // the app's question, not this file's (PlayerWindow::onDragEnd).
            if (e.button == 0 && dragValid_) {
                const int dx = (int)e.x - dragStartX_, dy = (int)e.y - dragStartY_;
                dragValid_ = false;
                if (std::abs(dx) > 4 || std::abs(dy) > 4)
                    owner_->onDragEnd(dx, dy);
            }
            break;
        case PointerAction::Leave:
            owner_->onMouseLeave();
            break;
        default:
            break;
        }
    }

    void onWheel(const WheelEvent& e) override {
        // WHEEL_DELTA (120) is the Win32 convention onMouseWheel's callers
        // already assume (see the old WM_MOUSEWHEEL path) — scale Wayland's
        // continuous axis value into the same units so onMouseWheel's
        // existing scroll-step math behaves identically on both platforms.
        owner_->onMouseWheel((int)e.x, (int)e.y, (int)(e.deltaY * 120.0f));
    }

    void onKey(const KeyEvent& e) override {
        if (e.keyCode == key::Alt) { altHeld_ = e.down; return; }
        if (!e.down) return;

        // Alt+F/J/C/U/G/H/L edge-snap/mode-toggle: Windows delivers these as
        // system-wide RegisterHotKey WM_HOTKEY messages; Wayland has no
        // cross-compositor equivalent, so this is a focused-window-only
        // check instead — a deliberate, documented behavior narrowing (see
        // host.hh's class comment), not a silent drop.
        if (altHeld_) {
            switch (e.keyCode) {
            case 'F': owner_->onHotkey(kHotkeySnapLeft);    return;
            case 'J': owner_->onHotkey(kHotkeySnapRight);   return;
            case 'C': owner_->onHotkey(kHotkeySnapBottom);  return;
            case 'U': owner_->onHotkey(kHotkeySnapTop);     return;
            case 'G': owner_->onHotkey(kHotkeySnapCenterG); return;
            case 'H': owner_->onHotkey(kHotkeySnapCenterH); return;
            case 'L': owner_->onHotkey(kHotkeyToggleOrientation);  return;
            }
        }
        owner_->onKeyDownPortable(e.keyCode);
    }

    void onChar(const CharEvent& e) override {
        owner_->onCharPortable(e.codepoint);
    }

private:
    struct Event { AppEvent id; intptr_t p1, p2; };

    void dispatchAppEvent(const Event& e) {
        switch (e.id) {
        case AppEvent::TrackChange: owner_->applyTrackMetadata((int)e.p1, (int)e.p2); break;
        case AppEvent::ScanDone:
            if (e.p1 == 1) owner_->startBackgroundScan(); else owner_->onScanDone();
            break;
        case AppEvent::ArtDecoded:  owner_->onArtDecoded(); break;
        case AppEvent::RequestPlay: owner_->onPlay(); break;
        }
    }

    PlayerWindow* owner_ = nullptr;
    std::unique_ptr<WaylandDisplay> display_;
    std::unique_ptr<WaylandWindow> window_;
    std::unique_ptr<WaylandSurfaceProvider> surfaceProvider_;
    FileAssetReader assets_;
    FileByteReader  dataReader_;
    bool quit_ = false;
    bool altHeld_ = false;
    int seekTimerFd_ = -1;

    std::mutex eventsMu_;
    std::vector<Event> events_;

    bool lastDownValid_ = false;
    std::chrono::steady_clock::time_point lastDownTime_;
    int lastDownX_ = 0, lastDownY_ = 0;
    // Where the button went down, for onDragEnd(). Separate from lastDown*_
    // above: those belong to the double-click heuristic and are cleared by it,
    // and a drag must not be cancelled by a preceding double-click.
    int  dragStartX_ = 0, dragStartY_ = 0;
    bool dragValid_  = false;
};

std::unique_ptr<Host> make_host() {
    return std::make_unique<LinuxHost>();
}

// ── Linux bootstrap (was main.cpp's WinMain) ────────────────────────────────
int matrix_player_main();  // gui_main.cc — portable env/self-test/PlayerWindow entry

namespace {

// No minidump equivalent on Linux — flush/close the log before re-raising so
// at least the log's last lines survive a crash, then let the default
// handler produce a core dump (or terminate) exactly as it would have
// otherwise. A real crash-report pipeline (core_pattern helper, etc.) is a
// documented gap, not attempted here.
void crashHandler(int sig) {
    fprintf(stderr, "[Crash][ERROR] Unhandled signal %d\n", sig);
    fflush(stdout);
    fflush(stderr);
    fclose(stdout);
    fclose(stderr);
    signal(sig, SIG_DFL);
    raise(sig);
}

void openLogFile() {
    // app_paths::stateDir() (not the exe's own directory) so a read-only
    // install still gets a log — see app_paths.hh. Identical to the old path
    // unless MATRIX_STATE_HOME was defined at build time. This is the FIRST
    // thing main() does, which is why stateDir() has to be safe to call before
    // anything else exists: it creates the directory itself and falls back to
    // the exe's directory rather than failing.
    std::string logPath = app_paths::stateDir() + "matrix_player.log";
    freopen(logPath.c_str(), "w", stdout);
    // stderr must share stdout's DESCRIPTOR, not merely its path. Opening the
    // file twice gives the two streams independent offsets: stdout starts at 0
    // and grows, walking straight over everything stderr appended at EOF. The
    // whole audio engine logs to stderr — [AlsaSink], [JackSink], every
    // snd_strerror string — so a log that looked complete was silently missing
    // exactly the lines that explain a backend failure. That is why a crash
    // switching JACK -> ALSA left "not even a message".
    dup2(fileno(stdout), fileno(stderr));
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
}

} // namespace

int main() {
    openLogFile();
    signal(SIGSEGV, crashHandler);
    signal(SIGABRT, crashHandler);

    return matrix_player_main();
}

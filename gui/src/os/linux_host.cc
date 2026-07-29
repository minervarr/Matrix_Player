// Linux Host implementation — real Wayland window/surface/input via
// framework/vk_canvas/platform/linux (WaylandDisplay/WaylandWindow), no
// stubbing. See ../host.hh's class comment for what genuinely has no
// Wayland equivalent (edge-snap, cross-monitor re-fit) vs what does
// (Complete mode's fullscreen, via xdg_toplevel's set_fullscreen()).
#include "host.hh"
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
// Default windowed size when Essential mode can't derive one from a real
// monitor query (Wayland has no "work area" concept — see host.hh).
constexpr int kDefaultW = 1200;
constexpr int kDefaultH = 700;
}

class LinuxHost : public Host, public InputSink {
public:
    // The Wayland connection (and its output/monitor list) is established
    // here, not in init() — PlayerWindow::create() queries primaryMonitor()
    // BEFORE calling init() (to decide the initial UiMode), the same way
    // Windows' MonitorFromPoint works without any window existing yet.
    LinuxHost() : display_(std::make_unique<WaylandDisplay>()) {}

    ~LinuxHost() override {
        if (seekTimerFd_ >= 0) ::close(seekTimerFd_);
    }

    std::string exeDir() const override {
        char buf[PATH_MAX];
        ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n <= 0) return "./";
        buf[n] = '\0';
        std::string path(buf);
        auto slash = path.rfind('/');
        return slash == std::string::npos ? "./" : path.substr(0, slash + 1);
    }

    bool init(PlayerWindow* owner, UiMode initialMode) override {
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

        if (initialMode == UiMode::Complete) window_->set_fullscreen(nullptr);

        seekTimerFd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        return true;
    }

    SurfaceProvider& surfaceProvider() override { return *surfaceProvider_; }
    AssetReader&     assetReader()     override { return assets_; }

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

    void applyUiMode(UiMode mode) override {
        if (mode == UiMode::Complete) window_->set_fullscreen(nullptr);
        else                          window_->unset_fullscreen();
        // The actual resize is reported asynchronously by the compositor's
        // next configure (checked in pump() via take_resized()), unlike
        // Windows' synchronous SetWindowPos — so onHostResized() isn't
        // called here directly; pump() calls it once the resize lands.
    }

    void adaptToCurrentMonitor(UiMode) override {
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
                if (isDouble) {
                    owner_->onLButtonDblClk((int)e.x, (int)e.y);
                    lastDownValid_ = false;  // don't chain a third click into another dblclk
                } else {
                    owner_->onLButtonDown((int)e.x, (int)e.y);
                }
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
            case 'L': owner_->onHotkey(kHotkeyToggleMode);  return;
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
    bool quit_ = false;
    bool altHeld_ = false;
    int seekTimerFd_ = -1;

    std::mutex eventsMu_;
    std::vector<Event> events_;

    bool lastDownValid_ = false;
    std::chrono::steady_clock::time_point lastDownTime_;
    int lastDownX_ = 0, lastDownY_ = 0;
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
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    std::string dir = "./";
    if (n > 0) {
        buf[n] = '\0';
        std::string path(buf);
        auto slash = path.rfind('/');
        if (slash != std::string::npos) dir = path.substr(0, slash + 1);
    }
    std::string logPath = dir + "matrix_player.log";
    freopen(logPath.c_str(), "w", stdout);
    freopen(logPath.c_str(), "a", stderr);
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

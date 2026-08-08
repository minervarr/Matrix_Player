// Windows Host implementation — window creation, message loop, monitor
// rects, all moved (behavior-preserving, not rewritten) from what used to be
// PlayerWindow::create()/run()/wndProc()/handleMsg()/computeCompleteWindowRect()/
// computeEssentialWindowRect()/adaptToCurrentMonitor()/snapToEdge() before the
// host abstraction (see ../host.hh).
#include "host.hh"
#include "app_paths.hh"
#include "player_view.hh"
#include "win32_platform.hh"
#include "renderer.hh"

#include <windowsx.h>
#include <dbghelp.h>
#include <mmsystem.h>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "dbghelp.lib")

int matrix_player_main();  // gui_main.cc — portable env/self-test/PlayerWindow entry

namespace {

const wchar_t* kMainClass = L"MatrixPlayerMain";

// Borderless: no title bar/icon/min-max-close buttons at all. Both UI modes
// are fixed sizes the app sets itself (see PlayerWindow::toggleUiMode()), so
// there's no resize/maximize to offer anyway; positioning is via the
// Alt+F/J/C/U edge-snap hotkeys instead of title-bar dragging. Taskbar/
// Alt-Tab presence (and the app icon there) still comes from
// WS_EX_APPWINDOW — only the on-window chrome is gone. Alt+F4 and the
// taskbar icon's right-click "Close window"/single-click-to-minimize still
// work; neither is tied to WS_CAPTION/WS_SYSMENU.
constexpr DWORD kFixedWindowStyle = WS_POPUP;
constexpr DWORD kFixedWindowExStyle = WS_EX_APPWINDOW;

LayoutRect toLayoutRect(const RECT& r) {
    return { r.left, r.top, r.right, r.bottom };
}

RECT computeCompleteWindowRect(HMONITOR mon) {
    // True fullscreen: the window covers the monitor's entire resolution,
    // including over the taskbar (rcMonitor, not rcWork) — sized to whichever
    // monitor the caller asks about, so this stays correct after the window
    // (or its whole desktop) moves to a different display (see
    // WindowsHost::adaptToCurrentMonitor()). kFixedWindowStyle is WS_POPUP
    // (no title bar, no borders), so the window rect and the monitor rect
    // are the same thing — no AdjustWindowRectEx needed.
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    return mi.rcMonitor;
}

RECT computeEssentialWindowRect(HMONITOR mon) {
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    int monW = mi.rcWork.right - mi.rcWork.left;
    int monH = mi.rcWork.bottom - mi.rcWork.top;

    // Anchor to whichever dimension is the monitor's constraint, phone-shaped
    // (9:16) on the OPPOSITE axis from the monitor's own orientation — a
    // portrait "phone" on a landscape monitor, a landscape one on a portrait
    // monitor. Self-fitting by construction; see host.hh's UiMode comment.
    int contentW, contentH;
    if (monW >= monH) {
        contentH = monH;
        contentW = (int)std::lround(contentH * 9.0 / 16.0);
    } else {
        contentW = monW;
        contentH = (int)std::lround(contentW * 9.0 / 16.0);
    }

    RECT r = { 0, 0, contentW, contentH };
    AdjustWindowRectEx(&r, kFixedWindowStyle, FALSE, kFixedWindowExStyle);
    int w = std::min((int)(r.right - r.left), monW);
    int h = std::min((int)(r.bottom - r.top), monH);
    int x = mi.rcWork.left + (monW - w) / 2;
    int y = mi.rcWork.top + (monH - h) / 2;
    return { x, y, x + w, y + h };
}

} // namespace

class WindowsHost : public Host {
public:
    std::string exeDir() const override { return app_paths::exeDir(); }

    bool init(PlayerWindow* owner, UiMode initialMode) override {
        owner_ = owner;
        hInst_ = GetModuleHandleW(nullptr);

        WNDCLASSEXW wc = {};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_DBLCLKS;
        wc.lpfnWndProc   = wndProcThunk;
        wc.hInstance     = hInst_;
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.lpszClassName = kMainClass;
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon         = LoadIconW(hInst_, L"IDI_APPICON");
        wc.hIconSm       = LoadIconW(hInst_, L"IDI_APPICON");
        RegisterClassExW(&wc);

        HMONITOR primaryMon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
        RECT startRect = (initialMode == UiMode::Complete)
            ? computeCompleteWindowRect(primaryMon)
            : computeEssentialWindowRect(primaryMon);

        hwnd_ = CreateWindowExW(kFixedWindowExStyle, kMainClass, L"Matrix Player",
            kFixedWindowStyle, startRect.left, startRect.top,
            startRect.right - startRect.left, startRect.bottom - startRect.top,
            nullptr, nullptr, hInst_, this);
        if (!hwnd_) return false;
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, (LONG_PTR)this);
        lastMonitor_ = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);

        // Edge-snap hotkeys — the window-move mechanism now that there's no
        // title bar to drag: Alt+F/J snap to the left/right edge (horizontal
        // monitor use), Alt+C/U snap to the bottom/top edge (vertical monitor
        // use), Alt+G/H re-center. MOD_NOREPEAT so holding the keys doesn't
        // spam WM_HOTKEY.
        RegisterHotKey(hwnd_, kHotkeySnapLeft,    MOD_ALT | MOD_NOREPEAT, 'F');
        RegisterHotKey(hwnd_, kHotkeySnapRight,   MOD_ALT | MOD_NOREPEAT, 'J');
        RegisterHotKey(hwnd_, kHotkeySnapBottom,  MOD_ALT | MOD_NOREPEAT, 'C');
        RegisterHotKey(hwnd_, kHotkeySnapTop,     MOD_ALT | MOD_NOREPEAT, 'U');
        RegisterHotKey(hwnd_, kHotkeySnapCenterG, MOD_ALT | MOD_NOREPEAT, 'G');
        RegisterHotKey(hwnd_, kHotkeySnapCenterH, MOD_ALT | MOD_NOREPEAT, 'H');
        // Alt+L toggles Essential/Complete — keyboard only.
        RegisterHotKey(hwnd_, kHotkeyToggleMode,  MOD_ALT | MOD_NOREPEAT, 'L');

        vkSurface_ = std::make_unique<Win32SurfaceProvider>(hwnd_);
        return true;
    }

    SurfaceProvider& surfaceProvider() override { return *vkSurface_; }
    AssetReader&     assetReader()     override { return assets_; }

    void showWindow() override {
        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);
    }

    MonitorInfo primaryMonitor() const override {
        HMONITOR mon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoW(mon, &mi);
        return { toLayoutRect(mi.rcMonitor), toLayoutRect(mi.rcWork) };
    }

    void applyUiMode(UiMode mode) override {
        HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
        RECT r = (mode == UiMode::Complete)
            ? computeCompleteWindowRect(mon)
            : computeEssentialWindowRect(mon);
        SetWindowPos(hwnd_, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        // The window just jumped straight to its new size in one step
        // (Essential <-> Complete, the latter now fullscreen) rather than
        // resizing gradually — the renderer only discovers a resize lazily
        // otherwise, so the very next frame would render at the stale
        // swapchain extent and come out corrupted/torn.
        owner_->onHostResized();
    }

    void adaptToCurrentMonitor(UiMode mode) override {
        // A minimized window's rect is Windows' off-screen placeholder
        // (conventionally around (-32000,-32000)), not a real position —
        // fitting against "whichever monitor is nearest that" is
        // meaningless, and calling SetWindowPos on a minimized window here
        // would fight the user's own minimize (each reposition re-fires
        // WM_WINDOWPOSCHANGED, which could re-trigger this and thrash
        // instead of settling, tanking the frame rate). Skip entirely while
        // minimized; the real position is re-checked as soon as it's restored.
        if (IsIconic(hwnd_)) return;

        HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
        lastMonitor_ = mon;  // set first: avoids re-entering via the
                             // WM_WINDOWPOSCHANGED this SetWindowPos triggers

        RECT r = (mode == UiMode::Complete)
            ? computeCompleteWindowRect(mon)
            : computeEssentialWindowRect(mon);

        RECT cur{};
        GetWindowRect(hwnd_, &cur);
        if (cur.left == r.left && cur.top == r.top &&
            cur.right - cur.left == r.right - r.left &&
            cur.bottom - cur.top == r.bottom - r.top)
            return;  // already correct for this monitor — nothing to do

        SetWindowPos(hwnd_, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        owner_->onHostResized();
    }

    void snapToEdge(int hotkeyId) override {
        RECT wr{};
        GetWindowRect(hwnd_, &wr);
        int w = wr.right - wr.left, h = wr.bottom - wr.top;

        HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoW(mon, &mi);
        int monW = mi.rcWork.right - mi.rcWork.left;
        int monH = mi.rcWork.bottom - mi.rcWork.top;

        int x, y;
        switch (hotkeyId) {
        case kHotkeySnapLeft:
            x = mi.rcWork.left;
            y = mi.rcWork.top + std::max(0, (monH - h) / 2);
            break;
        case kHotkeySnapRight:
            x = mi.rcWork.right - w;
            y = mi.rcWork.top + std::max(0, (monH - h) / 2);
            break;
        case kHotkeySnapBottom:
            x = mi.rcWork.left + std::max(0, (monW - w) / 2);
            y = mi.rcWork.bottom - h;
            break;
        case kHotkeySnapTop:
            x = mi.rcWork.left + std::max(0, (monW - w) / 2);
            y = mi.rcWork.top;
            break;
        case kHotkeySnapCenterG:
        case kHotkeySnapCenterH:
            x = mi.rcWork.left + std::max(0, (monW - w) / 2);
            y = mi.rcWork.top + std::max(0, (monH - h) / 2);
            break;
        default:
            return;
        }
        SetWindowPos(hwnd_, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void invalidate() override {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void setCursor(CursorShape shape) override {
        LPCWSTR id = IDC_ARROW;
        switch (shape) {
        case CursorShape::Hand: id = IDC_HAND; break;
        case CursorShape::Text: id = IDC_IBEAM; break;
        case CursorShape::Arrow: break;
        }
        if (id == cursorId_) return;               // already showing
        cursorId_ = id;
        cursor_   = LoadCursorW(nullptr, id);
        // Win32 re-asserts the class cursor on every mouse move via
        // WM_SETCURSOR, so setting it here only holds until the next one —
        // wndProc answers that message from cursor_ (see WM_SETCURSOR).
        // Set it now too, or the change waits for the pointer to move.
        SetCursor(cursor_);
    }

    void setKeepAwake(bool on) override {
        // ES_CONTINUOUS makes the state sticky until the next call; without
        // it the request would apply to this one instant and lapse. Passing
        // ES_CONTINUOUS alone is how the flag is cleared.
        SetThreadExecutionState(on ? (ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED)
                                   : ES_CONTINUOUS);
    }

    void postAppEvent(AppEvent id, intptr_t p1, intptr_t p2) override {
        PostMessageW(hwnd_, WM_APP + (int)id, (WPARAM)p1, (LPARAM)p2);
    }

    void startTimer(TimerId id, int intervalMs) override {
        SetTimer(hwnd_, timerWinId(id), intervalMs, nullptr);
    }
    void stopTimer(TimerId id) override {
        KillTimer(hwnd_, timerWinId(id));
    }

    void pump(bool haveWork) override {
        MsgWaitForMultipleObjects(0, nullptr, FALSE, haveWork ? 0 : INFINITE, QS_ALLINPUT);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { quit_ = true; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    bool quitRequested() const override { return quit_; }

    void showErrorMessage(const std::string& title, const std::string& msg) override {
        MessageBoxA(hwnd_, msg.c_str(), title.c_str(), MB_OK | MB_ICONERROR);
    }

    HWND nativeHandle() const override { return hwnd_; }

private:
    static UINT_PTR timerWinId(TimerId id) { return 1 + (UINT_PTR)id; }

    static LRESULT CALLBACK wndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_CREATE) {
            auto* cs = (CREATESTRUCTW*)lp;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        }
        auto* self = (WindowsHost*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        if (self) return self->handleMsg(msg, wp, lp);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    LRESULT handleMsg(UINT msg, WPARAM wp, LPARAM lp) {
        // WM_APP_* range: dispatch cross-thread completions posted via
        // postAppEvent() back into owner_'s on*() methods.
        if (msg >= WM_APP && msg < WM_APP + 16) {
            switch ((AppEvent)(msg - WM_APP)) {
            case AppEvent::TrackChange: owner_->applyTrackMetadata((int)wp, (int)lp); return 0;
            case AppEvent::ScanDone:
                if (wp == 1) owner_->startBackgroundScan(); else owner_->onScanDone();
                return 0;
            case AppEvent::ArtDecoded:  owner_->onArtDecoded(); return 0;
            case AppEvent::RequestPlay: owner_->onPlay(); return 0;
            }
            return 0;
        }

        switch (msg) {
        case WM_SIZE:
            // Routine resize (also fires as a side effect of applyUiMode()'s
            // own SetWindowPos, which already called notifyResized() itself
            // directly — not repeated here to avoid telling the renderer
            // "resized" twice for one logical change).
            owner_->onHostLayoutInvalidated();
            return 0;

        case WM_HOTKEY:
            owner_->onHotkey((int)wp);
            return 0;

        case WM_DISPLAYCHANGE:
            // Some monitor's resolution/topology changed. The window's own
            // HMONITOR handle may not change even if its work area did (a
            // resolution change on the same physical monitor), so re-fit
            // unconditionally rather than gating on a monitor-handle diff.
            owner_->adaptToCurrentMonitor();
            return 0;

        case WM_WINDOWPOSCHANGED: {
            // Covers the window ending up on a different monitor by any
            // means (Win+Shift+Arrow, dragging a secondary monitor's taskbar
            // icon, etc.) — not just snapToEdge()/toggleUiMode(), which stay
            // on the current monitor by construction anyway.
            if (!IsIconic(hwnd_)) {
                HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
                if (mon != lastMonitor_) owner_->adaptToCurrentMonitor();
            }
            break;  // let DefWindowProc still generate WM_SIZE/WM_MOVE as usual
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_SETCURSOR:
            // Windows re-asks on every move over the client area, and the
            // window class's own hCursor would win otherwise — answering here
            // is what makes setCursor() stick. Only the client area: the
            // frame's resize arrows are DefWindowProc's to draw.
            if (LOWORD(lp) == HTCLIENT) {
                SetCursor(cursor_ ? cursor_ : LoadCursorW(nullptr, IDC_ARROW));
                return TRUE;
            }
            break;

        case WM_PAINT: {
            // Vulkan (drawFrame(), driven from PlayerWindow::run() while a
            // frame is pending) owns presentation — just validate the update
            // region so Windows doesn't keep resending WM_PAINT. Windows
            // sends this whenever the window is exposed (uncovered by
            // another window, alt-tab, etc.), independent of our own
            // invalidate() call sites, so it must still mark a frame dirty.
            PAINTSTRUCT ps;
            BeginPaint(hwnd_, &ps);
            EndPaint(hwnd_, &ps);
            owner_->onHostExposed();
            return 0;
        }

        case WM_TIMER:
            if (wp == timerWinId(TimerId::SeekUpdate)) owner_->onTimer();
            return 0;

        case WM_COMMAND:
            return 0;  // legacy WM_COMMAND path no longer used (see AppEvent::RequestPlay)

        case WM_MOUSEMOVE:
            owner_->onMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;

        case WM_MOUSELEAVE:
            owner_->onMouseLeave();
            return 0;

        case WM_LBUTTONDOWN:
            owner_->onLButtonDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;

        case WM_LBUTTONDBLCLK:
            owner_->onLButtonDblClk(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;

        // The two thumb buttons. Unlike every other mouse message these are
        // multiplexed onto one WM_, with the button in the HIWORD of wParam,
        // and they want TRUE rather than 0 returned (see the XBUTTON docs).
        case WM_XBUTTONDOWN: {
            WORD btn = GET_XBUTTON_WPARAM(wp);
            if (btn == XBUTTON1)      owner_->onNavBack();
            else if (btn == XBUTTON2) owner_->onNavForward();
            return TRUE;
        }

        case WM_MOUSEWHEEL: {
            // WM_MOUSEWHEEL delivers SCREEN coordinates (the one mouse
            // message that does) — convert to client before hit-testing,
            // matching every other mouse message onMouseWheel's siblings
            // already receive.
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ScreenToClient(hwnd_, &pt);
            owner_->onMouseWheel(pt.x, pt.y, GET_WHEEL_DELTA_WPARAM(wp));
            return 0;
        }

        case WM_CHAR:
            owner_->onCharPortable((uint32_t)wp);
            return 0;

        case WM_KEYDOWN:
            owner_->onKeyDownPortable((int)wp);
            return 0;

        case WM_DESTROY:
            UnregisterHotKey(hwnd_, kHotkeySnapLeft);
            UnregisterHotKey(hwnd_, kHotkeySnapRight);
            UnregisterHotKey(hwnd_, kHotkeySnapBottom);
            UnregisterHotKey(hwnd_, kHotkeySnapTop);
            UnregisterHotKey(hwnd_, kHotkeySnapCenterG);
            UnregisterHotKey(hwnd_, kHotkeySnapCenterH);
            UnregisterHotKey(hwnd_, kHotkeyToggleMode);

            owner_->shutdown();

            // Destroy the Vulkan surface while hwnd_ is still valid, rather
            // than waiting for WindowsHost's own destructor (which runs
            // after DestroyWindow has fully torn down the HWND).
            vkSurface_.reset();

            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }

    PlayerWindow* owner_ = nullptr;
    HWND      hwnd_  = nullptr;
    HINSTANCE hInst_ = nullptr;
    HMONITOR  lastMonitor_ = nullptr;
    bool      quit_ = false;
    // Current pointer image, re-asserted from WM_SETCURSOR. cursorId_ is kept
    // only to collapse repeat calls — LoadCursorW's shared handles are not
    // owned, so neither field needs releasing.
    HCURSOR   cursor_   = nullptr;
    LPCWSTR   cursorId_ = nullptr;

    std::unique_ptr<Win32SurfaceProvider> vkSurface_;
    FileAssetReader                       assets_;
};

std::unique_ptr<Host> make_host() {
    return std::make_unique<WindowsHost>();
}

// ── Windows bootstrap (was main.cpp's WinMain) ──────────────────────────────
namespace {

// windows.h only declares these behind a high enough _WIN32_WINNT/NTDDI_VERSION,
// which isn't explicitly set anywhere in this project's CMakeLists — define
// them ourselves if missing so this compiles regardless of default SDK target.
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
typedef HANDLE DPI_AWARENESS_CONTEXT;
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif

// Without this, a non-DPI-aware process gets its whole window bitmap-stretched
// by DWM on any scaled display (125%/150%/200% — the default on most Windows
// laptops), blurring otherwise-crisp MSDF text. Resolved dynamically (rather
// than via manifest) so it works regardless of the CMake/Ninja build not
// embedding one.
void enableDpiAwareness() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        using SetCtxFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
        auto setCtx = (SetCtxFn)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (setCtx && setCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return;
    }
    SetProcessDPIAware();
}

// app_paths hands back UTF-8; every file call below stays WIDE. The narrow
// _s variants would go through the ANSI codepage and mangle any non-ASCII
// component — and stateDir() can now sit under a user's home directory, where
// that is a great deal more likely than it ever was beside the executable.
std::wstring utf8ToWide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

void openLogFile() {
    // app_paths::stateDir() (not the exe's own directory) so a read-only
    // install still gets a log — see app_paths.hh. Identical to the old path
    // unless MATRIX_STATE_HOME was defined at build time.
    std::wstring logPath = utf8ToWide(app_paths::stateDir()) + L"matrix_player.log";
    FILE* outFp = nullptr; _wfreopen_s(&outFp, logPath.c_str(), L"w", stdout);
    FILE* errFp = nullptr; _wfreopen_s(&errFp, logPath.c_str(), L"a", stderr);
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
}

// Previously, an unhandled exception just killed the process with nothing
// recorded anywhere — matrix_player.log would simply stop mid-stream, no
// exception code, no clue which thread or address. This writes a minidump
// (loadable in WinDbg/Visual Studio for a full multi-thread stack trace)
// next to the log, and logs the exception code/address as the log's last
// line, before letting the crash proceed exactly as it did before.
LONG WINAPI crashHandler(EXCEPTION_POINTERS* info) {
    printf("[Crash][ERROR] Unhandled exception 0x%08X at address %p\n",
           (unsigned)info->ExceptionRecord->ExceptionCode,
           info->ExceptionRecord->ExceptionAddress);
    fflush(stdout);

    // Beside the log, for the same reason — the exe's directory may be
    // read-only, and a crash dump that cannot be written is no dump at all.
    std::wstring dumpPath = utf8ToWide(app_paths::stateDir()) + L"matrix_player_crash.dmp";

    HANDLE hFile = CreateFileW(dumpPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{};
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = info;
        mei.ClientPointers = FALSE;
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile,
                           MiniDumpNormal, &mei, nullptr, nullptr);
        CloseHandle(hFile);
        printf("[Crash][ERROR] Minidump written to %ls\n", dumpPath.c_str());
        fflush(stdout);
    }

    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    enableDpiAwareness();
    openLogFile();
    SetUnhandledExceptionFilter(crashHandler);
    // Raise system timer resolution to 1 ms for the app lifetime so any
    // Sleep()/WaitForSingleObject() in audio paths (notably the pre-buffer
    // wait in PlayerWindow::onPlay) doesn't get rounded to ~15.6 ms.
    timeBeginPeriod(1);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    int rc = matrix_player_main();

    CoUninitialize();
    timeEndPeriod(1);
    return rc;
}

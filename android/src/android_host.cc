#include "android_host.hh"

#include <android/input.h>
#include <android/log.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <thread>

#include "app_paths.hh"          // gui/src: the exeDir()/stateDir() seam itself
#include "app_paths_android.hh"  // and this platform's one-time setter for it
#include "fullscreen.hh"  // vce::platform::enable_immersive/query_nav_bar_height
#include "launch_intent.hh"
#include "player_view.hh"  // gui/src: the app itself
#include "safe_area.hh"
#include "storage_permission.hh"

#define LOG_TAG "AndroidHost"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

// Looper identifiers for the two descriptors this host adds. LOOPER_ID_MAIN
// and LOOPER_ID_INPUT (1 and 2) belong to native_app_glue; start above them.
constexpr int kLooperIdEvents = 3;
constexpr int kLooperIdTimer  = 4;

// Past this many pixels a touch is a scroll, and stops being a tap. 24 px is
// roughly a finger's own wobble on a modern phone — small enough that a
// deliberate tap never scrolls, large enough that resting a thumb does not
// count as a press.
constexpr float kTouchSlopPx = 24.0f;

// The wheel unit onMouseWheel()'s callers assume (Win32's WHEEL_DELTA), and
// the same scaling LinuxHost applies to Wayland's continuous axis. A touch
// drag is already in pixels, and every scroll consumer subtracts the delta
// from its scroll offset — so passing the finger's own displacement gives
// 1:1 content tracking, which is the only thing that feels right on a touch
// screen.
constexpr float kWheelPerPixel = 1.0f;

// ── Make the app's own diagnostics visible ───────────────────────────────────
//
// player_view.cc, the scanner and both audio backends report through printf on
// every platform: "[Audio] signal path: …", "[Scan] Done: N scanned", the USB
// driver's errors. On a desktop those land on a terminal or in
// matrix_player.log. On Android stdout goes to /dev/null, so the entire
// diagnostic channel of a 7000-line app was being silently discarded — which
// is exactly the channel you want the first time something misbehaves on a
// phone.
//
// A pipe with a reader thread is the standard fix: dup2 both descriptors onto
// its write end and forward whole lines to logcat under the tag
// "matrix_player". Costs one idle thread, and is what makes `adb logcat` tell
// the same story a terminal tells on Linux.
void redirect_stdio_to_logcat() {
    static bool installed = false;
    if (installed) return;
    installed = true;

    // Unbuffered, or a crash takes the last (most interesting) lines with it.
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    int fds[2];
    if (pipe(fds) != 0) return;
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    close(fds[1]);

    std::thread([readFd = fds[0]] {
        std::string line;
        char buf[512];
        for (;;) {
            ssize_t n = read(readFd, buf, sizeof(buf));
            if (n <= 0) return;   // pipe closed: the process is going away
            for (ssize_t i = 0; i < n; ++i) {
                if (buf[i] == '\n') {
                    __android_log_write(ANDROID_LOG_INFO, "matrix_player", line.c_str());
                    line.clear();
                } else if (buf[i] != '\r') {
                    line.push_back(buf[i]);
                }
            }
            // A partial line is HELD, not flushed: splitting on the read
            // boundary would tear messages in half at arbitrary points.
        }
    }).detach();
}

}  // namespace

// Host's platform factory, which on Android cannot do its job: an AndroidHost
// needs the android_app* that only android_main() is ever handed, so the host
// is INJECTED (src/main.cc) rather than manufactured. This exists because
// PlayerWindow::create() names the symbol on the branch it does not take here;
// create() checks for a null host and refuses, so a future caller that forgets
// to inject one fails at startup with a log line instead of a null dereference.
std::unique_ptr<Host> make_host() {
    LOGE("make_host(): Android has no ambient host — inject one (see main.cc)");
    return nullptr;
}

AndroidHost::AndroidHost(android_app* state) : state_(state) {
    // First thing, before any of the app's own code can print: everything
    // player_view.cc and the engine report is otherwise thrown away here.
    redirect_stdio_to_logcat();
    state_->userData     = this;
    state_->onAppCmd     = handleAppCmd;
    state_->onInputEvent = handleInputEvent;
}

AndroidHost::~AndroidHost() {
    if (eventFd_ >= 0) close(eventFd_);
    if (timerFd_ >= 0) close(timerFd_);
}

std::string AndroidHost::exeDir() const {
    // Empty, and that is the whole trick: PlayerWindow builds its font paths
    // as exeDir() + "fonts/…", which on Android is exactly the asset name
    // AAssetManager wants. See Host::dataReader().
    return app_paths::exeDir();
}

bool AndroidHost::init(AppView* owner) {
    owner_ = owner;

    // internalDataPath is a plain field on ANativeActivity — no JNI needed.
    // Set BEFORE anything opens the database, which create() does immediately
    // after this returns.
    app_paths::setAndroidPaths("", state_->activity->internalDataPath);

    ALooper* looper = ALooper_forThread();
    eventFd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    timerFd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (looper && eventFd_ >= 0)
        ALooper_addFd(looper, eventFd_, kLooperIdEvents, ALOOPER_EVENT_INPUT, nullptr, nullptr);
    if (looper && timerFd_ >= 0)
        ALooper_addFd(looper, timerFd_, kLooperIdTimer, ALOOPER_EVENT_INPUT, nullptr, nullptr);

    // Android gives us a window when it is ready to, which is some time after
    // android_main() starts. Pump until it arrives; everything the app does
    // next needs a surface to exist.
    while (!state_->window && !state_->destroyRequested) {
        int events;
        android_poll_source* source = nullptr;
        if (ALooper_pollOnce(-1, nullptr, &events, reinterpret_cast<void**>(&source)) >= 0) {
            if (source) source->process(state_, source);
        }
    }
    if (!state_->window) return false;   // destroyed before it ever appeared

    LOGI("window ready: %dx%d", ANativeWindow_getWidth(state_->window),
         ANativeWindow_getHeight(state_->window));
    return surface_ != nullptr && assets_ != nullptr;
}

MonitorInfo AndroidHost::primaryMonitor() const {
    MonitorInfo mi{};
    if (state_->window) {
        mi.bounds = { 0, 0, ANativeWindow_getWidth(state_->window),
                            ANativeWindow_getHeight(state_->window) };
    }
    // No taskbar/panel concept, so the two are the same rectangle — the same
    // answer LinuxHost gives, for the same reason.
    mi.workArea = mi.bounds;
    return mi;
}

// The display cutout, and DELIBERATELY NOT the navigation bar, even though
// query_nav_bar_height() reports one (84 px on the test device).
//
// The two are different kinds of obstacle and take different answers. A system
// bar is SOFTWARE: enable_immersive() hides it, and the screen is then really
// ours — insetting for a bar that is not on screen would leave a permanent
// empty strip for nothing. A cutout is GLASS. It is still there with every bar
// hidden, and no API removes it; the only thing an app can do is not put
// anything under it.
//
// Which is also why hiding the bars CREATED this: while the status bar is
// shown, Android keeps the window clear of the cutout by itself, because the
// bar is what covers it. Hide the bar and the window goes edge to edge — 720 x
// 1640 on the test device, the whole panel — and the navigation rail lands
// under a 70 px camera notch.
SafeInsets AndroidHost::safeInsets() const { return cachedInsets_; }

void AndroidHost::refreshSafeInsets() {
    const SafeAreaInsets cut = query_safe_area_insets(state_);
    cachedInsets_ = { cut.top, cut.bottom, cut.left, cut.right };
}

void AndroidHost::setKeepAwake(bool on) {
    // Deliberately a no-op for now. It has a real Android equivalent
    // (FLAG_KEEP_SCREEN_ON), but its only caller is the fullscreen artwork
    // window, and ArtWindow declines to open on Android — so wiring it would
    // be code with no path that reaches it. See art_view.hh.
    (void)on;
}

// ── The app command stream ───────────────────────────────────────────────────

void AndroidHost::handleAppCmd(android_app* app, int32_t cmd) {
    auto* self = reinterpret_cast<AndroidHost*>(app->userData);
    switch (cmd) {
        case APP_CMD_INIT_WINDOW:   if (app->window) self->onWindowInit(); break;
        case APP_CMD_TERM_WINDOW:   self->onWindowTerm();  break;
        case APP_CMD_GAINED_FOCUS:  self->onGainedFocus(); break;
        case APP_CMD_RESUME:        self->onResume();      break;
        case APP_CMD_WINDOW_RESIZED:
        case APP_CMD_CONFIG_CHANGED:
            // A rotation arrives here as an ordinary resize, which is exactly
            // what ui_orientation.hh assumes: the layout is derived from the
            // window's own shape and nothing ever asks Android which way up
            // the device is. Confirmed on a moto g06 — turning the phone
            // delivers CONFIG_CHANGED, then WINDOW_RESIZED with the window
            // already reporting 1640x720, and the app lays out horizontal from
            // that alone.
            // The cutout moves with the screen: what was a 70 px top inset in
            // portrait is a side inset in landscape. Re-queried here rather
            // than cached from startup, exactly as safe_area.hh asks.
            self->refreshSafeInsets();
            if (self->owner_ && self->appReady_) self->owner_->onHostResized();
            break;
        default: break;
    }
}

// APP_CMD_INIT_WINDOW is NOT "the app started". It fires again every time
// another activity covers this one and the listener comes back — treating it
// as startup is what produced the permission loop this file used to have.
void AndroidHost::onWindowInit() {
    surface_ = std::make_unique<AndroidSurfaceProvider>(state_->window);
    if (!assets_)
        assets_ = std::make_unique<AndroidAssetReader>(state_->activity->assetManager);

    vce::platform::enable_immersive(state_, vce::platform::ImmersiveMode::kFullImmersive);

    // What the OS actually handed us, versus what it says is in the way. The
    // goal is for these three numbers to make the inset arithmetic
    // unnecessary: if the window we get already excludes the bars and the
    // cutout, PlayerWindow needs no notion of a safe area at all — the
    // rectangle IS the safe area. Logged rather than assumed because every
    // handset places its cutout and its navigation differently, and a height
    // we compute ourselves is a height that is wrong on the next phone.
    refreshSafeInsets();
    LOGI("geometry: window %dx%d | cutout t=%d b=%d l=%d r=%d | navbar=%d (hidden, not inset)",
         ANativeWindow_getWidth(state_->window), ANativeWindow_getHeight(state_->window),
         cachedInsets_.top, cachedInsets_.bottom, cachedInsets_.left, cachedInsets_.right,
         vce::platform::query_nav_bar_height(state_));

    // Before run() takes over, PlayerWindow::create() is still building its
    // own Renderer over this surface — telling it to rebuild one here would
    // be a second Renderer for the same window.
    if (appReady_ && owner_) {
        if (!owner_->onSurfaceRecreated())
            LOGE("could not rebuild the renderer on returning to the app");
    }
}

void AndroidHost::onWindowTerm() {
    // The Renderer, the swapchain and every texture belong to the surface
    // that is going away. PlayerWindow keeps its database, its library and
    // its playback: the listener left the app, they did not restart it.
    if (owner_) owner_->onSurfaceLost();
    surface_.reset();
}

void AndroidHost::onGainedFocus() {
    // The system clears immersive flags on focus loss (e.g. a permission
    // dialog) — fullscreen.hh's documented contract requires re-applying on
    // every APP_CMD_GAINED_FOCUS, not just at startup.
    if (state_->window)
        vce::platform::enable_immersive(state_, vce::platform::ImmersiveMode::kFullImmersive);
}

void AndroidHost::onResume() {
    // Coming back from the system Settings screen is the ONE moment the
    // storage answer can have changed, and it is the moment
    // storage_permission.hh's own comment says to re-check on.
    maybeSeedMusicRoot();
}

// ── Storage permission, asked at most once ───────────────────────────────────
//
// This used to be two unconditional startActivity() calls in onWindowInit(),
// and that is a LOOP rather than a prompt: launching the Settings screen
// covers this activity, which destroys its window; coming back recreates the
// window and fires APP_CMD_INIT_WINDOW again, which asked again. Granting the
// permission did not break the cycle, because nothing ever checked — the
// request did not depend on the answer.
//
// The once-per-process flag is the second half, and it is not redundant: a
// listener who DECLINES leaves has_all_files_access() false forever, so the
// check alone would re-ask on every resume — a slower loop, but the same one.
//
// show_folder_picker_hint() is deliberately NOT called. Its result cannot be
// read without a Java onActivityResult override (see its own comment), so it
// contributed nothing but a second modal — and it is the one that literally
// asks the listener to select a folder.
void AndroidHost::ensureStoragePermission() {
    if (storageAsked_) return;
    storageAsked_ = true;
    if (has_all_files_access(state_)) {
        LOGI("storage: all-files access already granted");
        return;
    }
    LOGI("storage: requesting all-files access (once)");
    request_all_files_access(state_);
}

// Storage, then a nudge to the app — and NOT a decision.
//
// This used to read the intent's "scan_root" extra and call PlayerWindow's own
// commitAddFolder() with it, which is a host reaching into an application's
// domain: the one place a message pump knew what music was. Now it does the two
// things that are genuinely a phone's business — get permission, and be sure
// the app is actually built — and then says onHostReady(). What a launch
// argument MEANS is answered in AppView::onHostReady(); the string itself comes
// out of launchArgument() below.
void AndroidHost::maybeSeedMusicRoot() {
    // appReady_, not just owner_. owner_ is set at the TOP of init(), while
    // the database it is about to be asked about is opened after init()
    // RETURNS — and Android fires APP_CMD_RESUME inside that window. Asking
    // then is a null sqlite3* dereference and an instant, silent process
    // death. The guard lives here rather than in each caller because the
    // requirement belongs to this function, not to the callers who happen to
    // exist today.
    if (rootSeeded_ || !appReady_ || !owner_) return;
    ensureStoragePermission();
    if (!has_all_files_access(state_)) {
        LOGI("storage: no access yet -- deferring the music root");
        return;
    }
    rootSeeded_ = true;
    owner_->onHostReady();
}

// What this activity was launched WITH. Empty on both desktops (see host.hh);
// here it is the intent's "scan_root" extra, stated and not interpreted.
std::string AndroidHost::launchArgument() const {
    return read_scan_root_extra(state_);
}

// ── Touch ────────────────────────────────────────────────────────────────────

int32_t AndroidHost::handleInputEvent(android_app* app, AInputEvent* event) {
    auto* self = reinterpret_cast<AndroidHost*>(app->userData);
    if (!self->owner_ || AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION) return 0;

    const float x = AMotionEvent_getX(event, 0);
    const float y = AMotionEvent_getY(event, 0);
    switch (AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK) {
        case AMOTION_EVENT_ACTION_DOWN:   self->onTouchDown(x, y);        return 1;
        case AMOTION_EVENT_ACTION_MOVE:   self->onTouchMove(x, y);        return 1;
        case AMOTION_EVENT_ACTION_UP:     self->onTouchUp(x, y, false);   return 1;
        case AMOTION_EVENT_ACTION_CANCEL: self->onTouchUp(x, y, true);    return 1;
        default: return 0;
    }
}

void AndroidHost::onTouchDown(float x, float y) {
    touchStartX_ = x;
    touchStartY_ = y;
    touchLastY_  = y;
    touchDragging_ = false;
    touchDown_     = true;
    // Hover follows the finger so the app can light what is under it. It is
    // the only hover a touch screen has, and it is honest: the desktop's
    // hover means "the pointer is here", and here it is.
    owner_->onMouseMove((int)x, (int)y);
}

void AndroidHost::onTouchMove(float x, float y) {
    if (!touchDown_) return;
    if (!touchDragging_) {
        const float dx = x - touchStartX_, dy = y - touchStartY_;
        if (std::sqrt(dx * dx + dy * dy) <= kTouchSlopPx) return;   // still a tap
        touchDragging_ = true;
    }
    // Past the slop the gesture belongs to scrolling, for good. The wheel is
    // fed the finger's own displacement since the last event, so content
    // tracks the finger rather than stepping.
    const int delta = (int)std::lround((y - touchLastY_) * kWheelPerPixel);
    touchLastY_ = y;
    if (delta != 0) owner_->onMouseWheel((int)x, (int)y, delta);
}

void AndroidHost::onTouchUp(float x, float y, bool cancelled) {
    const bool wasTap  = touchDown_ && !touchDragging_ && !cancelled;
    const bool wasDrag = touchDown_ &&  touchDragging_ && !cancelled;
    const float dx = x - touchStartX_, dy = y - touchStartY_;
    touchDown_     = false;
    touchDragging_ = false;

    // A finished drag is reported as such, in addition to the wheel deltas it
    // already produced along the way. The app uses it for gestures that are
    // about the WHOLE stroke rather than its increments (the artwork's swipe);
    // the slop that decided this was a drag at all stays here, because it is a
    // property of a touch screen and not of the app.
    if (wasDrag) owner_->onDragEnd((int)dx, (int)dy);
    if (!wasTap) return;

    // A press is delivered at RELEASE, not at contact. That is what makes a
    // drag able to change its mind: the finger has to come off in the same
    // place for anything to be pressed at all.
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::now();
    const bool isDouble =
        lastTapValid_ &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTapTime_).count() < 400 &&
        std::fabs(x - lastTapX_) < kTouchSlopPx && std::fabs(y - lastTapY_) < kTouchSlopPx;

    lastTapTime_ = now;
    lastTapX_ = x; lastTapY_ = y;
    lastTapValid_ = true;

    owner_->onMouseMove((int)x, (int)y);
    if (isDouble) {
        owner_->onLButtonDblClk((int)x, (int)y);
        lastTapValid_ = false;   // don't chain a third tap into another double
    } else {
        owner_->onLButtonDown((int)x, (int)y);
    }
}

// ── Cross-thread events and the seek timer ───────────────────────────────────

void AndroidHost::postAppEvent(int id, intptr_t p1, intptr_t p2) {
    {
        std::lock_guard<std::mutex> lk(eventsMu_);
        events_.push_back({id, p1, p2});
    }
    // Wakes a pump() blocked in ALooper_pollOnce. The counter's VALUE is never
    // read — the queue above is the payload; this is only the doorbell.
    if (eventFd_ >= 0) {
        uint64_t one = 1;
        ssize_t ignored = write(eventFd_, &one, sizeof(one));
        (void)ignored;
    }
}

void AndroidHost::drainEvents() {
    if (eventFd_ >= 0) {
        uint64_t sink = 0;
        while (read(eventFd_, &sink, sizeof(sink)) > 0) {}
    }
    std::vector<Event> pending;
    {
        std::lock_guard<std::mutex> lk(eventsMu_);
        pending.swap(events_);
    }
    for (auto& e : pending) dispatchAppEvent(e);
}

void AndroidHost::dispatchAppEvent(const Event& e) {
    if (!owner_) return;
    owner_->onAppEvent(e.id, e.p1, e.p2);
}

void AndroidHost::startTimer(int id, int intervalMs) {
    timerId_ = id;  // one timer fd; the id is remembered, not honoured
    if (timerFd_ < 0) return;
    itimerspec spec{};
    spec.it_value.tv_sec  = intervalMs / 1000;
    spec.it_value.tv_nsec = (intervalMs % 1000) * 1000000L;
    spec.it_interval = spec.it_value;
    timerfd_settime(timerFd_, 0, &spec, nullptr);
}

void AndroidHost::stopTimer(int) {
    if (timerFd_ < 0) return;
    itimerspec spec{};
    timerfd_settime(timerFd_, 0, &spec, nullptr);
}

void AndroidHost::drainTimer() {
    if (timerFd_ < 0 || !owner_) return;
    uint64_t expirations = 0;
    if (read(timerFd_, &expirations, sizeof(expirations)) > 0) owner_->onTimer(timerId_);
}

// ── The pump ─────────────────────────────────────────────────────────────────

void AndroidHost::pump(bool haveWork) {
    if (!appReady_) {
        appReady_ = true;
        // First tick after create(): the app exists now, so this is the
        // earliest honest moment to ask for storage and hand over the root.
        maybeSeedMusicRoot();
    }

    // Blocking when there is nothing to draw is what keeps a phone's battery
    // out of this: with no pending frame the process sleeps in the kernel
    // until a touch, a timer, or a background thread's eventfd wakes it.
    int events;
    android_poll_source* source = nullptr;
    const int ident = ALooper_pollOnce(haveWork ? 0 : -1, nullptr, &events,
                                       reinterpret_cast<void**>(&source));
    if (ident >= 0) {
        if (source) source->process(state_, source);
        else if (ident == kLooperIdEvents) drainEvents();
        else if (ident == kLooperIdTimer)  drainTimer();
    }

    // Both are drained unconditionally as well: several descriptors can be
    // ready in one wake-up and pollOnce reports only one of them, so relying
    // on the ident alone loses whichever it did not name.
    drainEvents();
    drainTimer();
}

bool AndroidHost::quitRequested() const {
    return state_->destroyRequested != 0;
}

void AndroidHost::showErrorMessage(const std::string& title, const std::string& msg) {
    // A log line, exactly as on Linux. The app's own on-screen notice
    // (audioNotice_) is the report a listener actually sees; this is its
    // companion for whoever is holding a logcat.
    LOGE("[%s] %s", title.c_str(), msg.c_str());
}

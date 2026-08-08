#include "android_host.hh"

#include <android/input.h>
#include <android/log.h>
#include <chrono>

#include "android_player_view.hh"
#include "app_paths_android.hh"
#include "canvas.hh"
#include "fullscreen.hh"  // vce::platform::enable_immersive/query_nav_bar_height
#include "launch_intent.hh"
#include "renderer.hh"
#include "safe_area.hh"
#include "storage_permission.hh"

#define LOG_TAG "AndroidHost"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

AndroidHost::AndroidHost(android_app* state) : state_(state) {
    state_->userData    = this;
    state_->onAppCmd    = handleAppCmd;
    state_->onInputEvent = handleInputEvent;
}

AndroidHost::~AndroidHost() {
    onWindowTerm();
}

void AndroidHost::run() {
    while (true) {
        int events;
        android_poll_source* source;
        while (ALooper_pollOnce(renderer_ ? 0 : -1, nullptr, &events,
                                reinterpret_cast<void**>(&source)) >= 0) {
            if (source) source->process(state_, source);
            if (state_->destroyRequested) return;
        }
        if (renderer_) drawFrame();
    }
}

void AndroidHost::handleAppCmd(android_app* app, int32_t cmd) {
    auto* self = reinterpret_cast<AndroidHost*>(app->userData);
    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            if (app->window) self->onWindowInit();
            break;
        case APP_CMD_TERM_WINDOW:
            self->onWindowTerm();
            break;
        case APP_CMD_GAINED_FOCUS:
            self->onGainedFocus();
            break;
        case APP_CMD_RESUME:
            self->onResume();
            break;
        case APP_CMD_PAUSE:
            self->onPause();
            break;
        default:
            break;
    }
}

int32_t AndroidHost::handleInputEvent(android_app* app, AInputEvent* event) {
    auto* self = reinterpret_cast<AndroidHost*>(app->userData);
    if (!self->view_) return 0;
    if (AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION) return 0;

    const float x = AMotionEvent_getX(event, 0);
    const float y = AMotionEvent_getY(event, 0);
    switch (AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK) {
        case AMOTION_EVENT_ACTION_DOWN:
            self->view_->onTouchDown(x, y);
            return 1;
        case AMOTION_EVENT_ACTION_MOVE:
            self->view_->onTouchMove(x, y);
            return 1;
        case AMOTION_EVENT_ACTION_UP:
        case AMOTION_EVENT_ACTION_CANCEL:
            self->view_->onTouchUp(x, y);
            return 1;
        default:
            return 0;
    }
}

void AndroidHost::onWindowInit() {
    surface_  = std::make_unique<AndroidSurfaceProvider>(state_->window);
    assets_   = std::make_unique<AndroidAssetReader>(state_->activity->assetManager);
    renderer_ = std::make_unique<Renderer>(*surface_, *assets_);
    LOGI("Vulkan initialised (%ux%u)", renderer_->width(), renderer_->height());

    // internalDataPath is a plain field on ANativeActivity — no JNI call
    // needed (see the design doc's "app_paths for Android" section).
    app_paths::setAndroidPaths("", state_->activity->internalDataPath);

    request_all_files_access(state_);
    show_folder_picker_hint(state_);

    if (view_) {
        view_->startScan(read_scan_root_extra(state_));
    }

    vce::platform::enable_immersive(state_, vce::platform::ImmersiveMode::kFullImmersive);

    orientation_.start();
    orientation_.enable();

    lastFrameNs_ = std::chrono::steady_clock::now().time_since_epoch().count();
}

void AndroidHost::onWindowTerm() {
    orientation_.disable();
    orientation_.stop();
    renderer_.reset();
    surface_.reset();
    assets_.reset();
}

void AndroidHost::onGainedFocus() {
    // The system clears immersive flags on focus loss (e.g. a permission
    // dialog) — fullscreen.hh's documented contract requires re-applying on
    // every APP_CMD_GAINED_FOCUS, not just at startup.
    if (state_->window) {
        vce::platform::enable_immersive(state_, vce::platform::ImmersiveMode::kFullImmersive);
    }
}

void AndroidHost::onResume() {
    orientation_.enable();
}

void AndroidHost::onPause() {
    orientation_.disable();  // battery: only listen while foregrounded
}

void AndroidHost::drawFrame() {
    const int64_t nowNs = std::chrono::steady_clock::now().time_since_epoch().count();
    const float   dt    = lastFrameNs_ > 0
                             ? static_cast<float>(nowNs - lastFrameNs_) * 1e-9f
                             : 0.0f;
    lastFrameNs_ = nowNs;

    std::vector<float> curves;
    // Insets are handled by AndroidPlayerView itself (see below), not by
    // Canvas's own inset params — passing zero here lets Canvas cover the
    // full screen and keeps the inset math in one place.
    Canvas canvas(curves, renderer_->width(), renderer_->height(), /*font=*/nullptr,
                 0.0f, 0.0f, 0.0f, 0.0f);

    if (view_) {
        const SafeAreaInsets cutout = query_safe_area_insets(state_);
        const int navBarH = vce::platform::query_nav_bar_height(state_);

        view_->onFrame(dt);
        view_->draw(canvas, static_cast<float>(renderer_->width()),
                   static_cast<float>(renderer_->height()),
                   static_cast<float>(cutout.top),
                   static_cast<float>(cutout.bottom + navBarH),
                   static_cast<float>(cutout.left),
                   static_cast<float>(cutout.right));
    }

    renderer_->draw(curves, /*overlay_rotation_deg=*/0);
}

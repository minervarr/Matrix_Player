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
#include "ui_fonts.hh"    // gui/src: the ONE place the face paths live
#include "ui_metrics.hh"  // gui/src: the five type roles to bake at

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
    initFonts();

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

// ── The UI typeface, out of the APK ──────────────────────────────────────────
//
// This did not exist, and its absence was invisible: Canvas took font=nullptr,
// Canvas::emitText_ fell through to the engine's built-in stroke font, and text
// appeared -- just not the app's. Sharing the LAYOUT with the desktop would
// never have made the two look alike on its own.
//
// Everything here is the desktop path (PlayerWindow::create) with one
// substitution: an AndroidAssetReader instead of a filesystem reader, because
// the faces live inside the APK rather than beside an executable. The face
// paths themselves come from gui/src/ui_fonts.hh, so the two platforms cannot
// drift onto different typefaces.
void AndroidHost::initFonts() {
    if (fontsReady_ || !assets_) return;

    // The vector fallback. Assets are not files, so this is loadFromMemory()
    // rather than load() -- the one real difference from the desktop path.
    std::vector<uint8_t> regular;
    if (assets_->read(ui_fonts::regular(), regular) && !regular.empty())
        uiFont_.loadFromMemory(regular.data(), regular.size());

    if (!msdfFont_.open(*assets_, ui_fonts::regular())) {
        LOGI("MTSDF font failed to open (%s) -- falling back to vector text",
             ui_fonts::regular());
        return;
    }
    // Bold for headers and titles, Italic for secondary text, and the Math
    // slot repurposed as monospace for numeric readouts that must not jitter
    // as digits change -- the same three the desktop registers.
    msdfFont_.addStyle(*assets_, ui_fonts::bold(),   FontStyle::Bold);
    msdfFont_.addStyle(*assets_, ui_fonts::italic(), FontStyle::Italic);
    msdfFont_.addStyle(*assets_, ui_fonts::mono(),   FontStyle::Math);
    // The icon glyphs share the atlas, as an OVERRIDE: their codepoints sit in
    // a private range the text faces do not cover.
    msdfFont_.addOverride(*assets_, ui_fonts::icons());

    // Bake Latin-1 at the five type roles. The rest of the library's scripts
    // arrive through the miss path the first time something asks for them,
    // exactly as on the desktop -- there is no library scanned yet at this
    // point anyway.
    const UiMetrics m = computeUiMetrics(
        (float)std::min(renderer_->width(), renderer_->height()));
    glyphSizes_ = { (int)(m.text.caption + 0.5f), (int)(m.text.secondary + 0.5f),
                    (int)(m.text.body    + 0.5f), (int)(m.text.title     + 0.5f),
                    (int)(m.text.header  + 0.5f) };
    std::vector<uint32_t> cps;
    for (uint32_t cp = 0x0020; cp <= 0x00FF; cp++) cps.push_back(cp);

    // ensureGlyphs() returns how many cells it placed, and it is [[nodiscard]]
    // for a reason: only a non-zero result (or an atlas the renderer has not
    // seen yet) means the GPU image has to be rebuilt. Same test the desktop
    // makes in refreshGlyphs().
    if (msdfFont_.ensureGlyphs(cps, glyphSizes_) > 0 || !renderer_->msdfReady())
        renderer_->initMsdf(msdfFont_);
    fontsReady_ = msdfFont_.valid();
    LOGI("UI fonts ready: msdf=%d, roles=%zu", (int)fontsReady_, glyphSizes_.size());
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
    msdfQuads_.clear();
    Canvas canvas(curves, renderer_->width(), renderer_->height(), &uiFont_,
                 0.0f, 0.0f, 0.0f, 0.0f);
    // MTSDF text when the atlas is up; the vector face above is the fallback
    // for the frames before it is, and for any glyph the atlas lacks.
    if (fontsReady_) canvas.useMsdf(&msdfFont_, &msdfQuads_);

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

    // Bake whatever this frame asked for and the atlas did not have yet, then
    // submit. Same order the desktop uses: a miss baked after the draw would
    // show as a blank glyph for one frame.
    if (fontsReady_ && msdfFont_.hasMisses() && msdfFont_.bakeMisses() > 0)
        renderer_->initMsdf(msdfFont_);
    renderer_->draw(curves, /*overlay_rotation_deg=*/0, {}, {}, msdfQuads_, {});
}

#include "art_view.hh"
#include "art_texture.hh"
#include "ui_min_text_size.gen.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <algorithm>

static const wchar_t* ART_CLASS = L"MatrixArtWindow";

bool ArtWindow::create(HINSTANCE hInst) {
    WNDCLASSEXW wc = {};
    wc.cbSize       = sizeof(wc);
    wc.style        = CS_DBLCLKS;
    wc.lpfnWndProc  = wndProc;
    wc.hInstance    = hInst;
    wc.hbrBackground= (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName= ART_CLASS;
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(WS_EX_APPWINDOW, ART_CLASS, L"Album Art",
        WS_POPUP, 0, 0, 800, 800, nullptr, nullptr, hInst, this);
    if (!hwnd_) return false;
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, (LONG_PTR)this);

    // Own Vulkan surface/renderer — a second, independent Renderer instance
    // targeting this HWND. Nothing in Renderer assumes singleton state.
    vkSurface_ = std::make_unique<Win32SurfaceProvider>(hwnd_);
    try {
        // 3 swapchain images (desktop MAILBOX) — see PlayerWindow::create().
        renderer_ = std::make_unique<Renderer>(*vkSurface_, vkAssets_,
                                               /*desiredSwapchainImages=*/3);
    } catch (const std::exception&) {
        return false;
    }

    wchar_t exePathW[MAX_PATH];
    GetModuleFileNameW(nullptr, exePathW, MAX_PATH);
    std::wstring exeDirW = exePathW;
    exeDirW = exeDirW.substr(0, exeDirW.rfind(L'\\') + 1);

    auto toUtf8Path = [&](const wchar_t* rel) -> std::string {
        std::wstring wpath = exeDirW + rel;
        int len = WideCharToMultiByte(CP_UTF8, 0, wpath.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string path(len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wpath.c_str(), -1, path.data(), len, nullptr, nullptr);
        if (!path.empty() && path.back() == '\0') path.pop_back();
        return path;
    };

    std::string fontPath = toUtf8Path(L"fonts\\lm\\lmroman10-regular.otf");
    uiFont_.load(fontPath.c_str());

    // Generate MSDF atlas (cached to disk, shared with PlayerWindow's own
    // MsdfFont instance — same cache file, so whichever window initializes
    // second gets a cache hit instead of re-rasterizing).
    std::string cachePath = toUtf8Path(L"fonts\\lmroman10-regular.msdf.cache");
    FileByteReader loader;
    if (msdfFont_.generate(loader, fontPath.c_str(), cachePath.c_str())) {
        bool addedStyle = false;
        if (!msdfFont_.hasStyle(FontStyle::Bold))
            addedStyle |= msdfFont_.addStyle(loader, toUtf8Path(L"fonts\\lm\\lmroman10-bold.otf").c_str(), FontStyle::Bold);
        if (!msdfFont_.hasStyle(FontStyle::Italic))
            addedStyle |= msdfFont_.addStyle(loader, toUtf8Path(L"fonts\\lm\\lmroman10-italic.otf").c_str(), FontStyle::Italic);
        if (!msdfFont_.hasStyle(FontStyle::Math))
            addedStyle |= msdfFont_.addStyle(loader, toUtf8Path(L"fonts\\lm\\lmmono10-regular.otf").c_str(), FontStyle::Math);
        if (addedStyle) msdfFont_.saveCache(cachePath.c_str());

        renderer_->initMsdf(msdfFont_);
    }

    return true;
}

void ArtWindow::show(const std::string& imagePath, HMONITOR preferMonitor) {
    currentPath_ = imagePath;

    // Pick monitor — use preferMonitor or primary if null
    HMONITOR mon = preferMonitor;
    if (!mon) mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(mon, &mi);

    int w = mi.rcMonitor.right  - mi.rcMonitor.left;
    int h = mi.rcMonitor.bottom - mi.rcMonitor.top;

    if (artTex_ != kInvalidTexture) { renderer_->destroy_texture(artTex_); artTex_ = kInvalidTexture; }
    // Fullscreen view: target the monitor's own resolution so quality is
    // unaffected by the resolution-matching done for the small thumbnails.
    if (renderer_) {
        FileByteReader reader;
        artTex_ = createTextureFromImageFile(*renderer_, reader, imagePath.c_str(), w, h,
                                             &artTexW_, &artTexH_);
    }

    SetWindowPos(hwnd_, HWND_TOP,
        mi.rcMonitor.left, mi.rcMonitor.top, w, h, SWP_SHOWWINDOW);

    // Rebuild the swapchain now, synchronously, rather than letting drawFrame()
    // discover the size jump lazily on its next call — the window just leapt
    // from its hidden 800x800 size straight to the monitor's full resolution,
    // and drawing even one frame against the stale swapchain extent shows up
    // as a corrupted/skewed frame.
    if (renderer_) renderer_->notifyResized();
    markDirty();
}

void ArtWindow::updateImage(const std::string& imagePath) {
    // Follow the now-playing album while fullscreen: called whenever the
    // transport art path changes (track/album/single boundary). Reloads the
    // texture in place — no repositioning, no swapchain churn.
    if (!isVisible() || imagePath == currentPath_) return;
    currentPath_ = imagePath;
    if (artTex_ != kInvalidTexture) { renderer_->destroy_texture(artTex_); artTex_ = kInvalidTexture; }
    if (renderer_ && !imagePath.empty()) {
        FileByteReader reader;
        artTex_ = createTextureFromImageFile(*renderer_, reader, imagePath.c_str(),
                                             (int)renderer_->width(), (int)renderer_->height(),
                                             &artTexW_, &artTexH_);
    }
    markDirty();
}

void ArtWindow::markDirty() {
    // See PlayerWindow::markDirty() — same "one frame per swapchain image"
    // reasoning, but this window owns an independent Renderer/swapchain.
    pendingFrames_ = renderer_ ? renderer_->swapchainImageCount() + 1 : 1;
}

void ArtWindow::renderIfDirty() {
    if (pendingFrames_ == 0) return;
    drawFrame();
    pendingFrames_--;
}

void ArtWindow::hide() {
    ShowWindow(hwnd_, SW_HIDE);
}

bool ArtWindow::isVisible() const {
    return hwnd_ && IsWindowVisible(hwnd_);
}

void ArtWindow::drawFrame() {
    if (!renderer_) return;
    frameCurves_.clear();
    frameShapes_.clear();
    frameImages_.clear();
    msdfQuads_.clear();
    Canvas canvas(frameCurves_, renderer_->width(), renderer_->height(),
                  &uiFont_, 0.0f, 0.0f, 0.0f, 0.0f);
    canvas.useImages(&frameImages_);
    canvas.useShapes(&frameShapes_);  // SDF shape fast path (see PlayerWindow::drawFrame)
    if (msdfFont_.valid())
        canvas.useMsdf(&msdfFont_, &msdfQuads_);
    if (artTex_ != kInvalidTexture && artTexW_ > 0 && artTexH_ > 0) {
        // No canvas.clear() here: it's an opaque vector rect that composites
        // AFTER images (see Phase 1's lesson) and would hide the art drawn
        // below — the render pass's own black clear covers the letterbox.
        // Scale to fit the monitor, preserving aspect ratio, centered —
        // matches the old GDI+ behavior. GPU bilinear sampling handles the
        // actual resample via the destination rect.
        float scale = std::min(canvas.w() / (float)artTexW_, canvas.h() / (float)artTexH_);
        float dstW = artTexW_ * scale, dstH = artTexH_ * scale;
        canvas.image(artTex_, (canvas.w() - dstW) * 0.5f, (canvas.h() - dstH) * 0.5f, dstW, dstH);
    } else {
        canvas.clear(col::bg);
        // Floored at the geometric minimum (ui_min_text_size.gen.h) like every
        // other text size in the app — see player_window.h for the full
        // window-relative system this fullscreen fallback text doesn't need
        // (this window is either fullscreen art or this one rarely-shown
        // placeholder string, not a dense UI needing per-role scaling).
        canvas.textCentered("No artwork", canvas.w() * 0.5f, canvas.h() * 0.5f,
                            std::max(18.0f, kMinReadableTextSizePx), col::dim);
    }
    renderer_->draw(frameCurves_, /*overlay_rotation_deg=*/0, frameImages_, {}, msdfQuads_,
                    frameShapes_);
}

LRESULT CALLBACK ArtWindow::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    ArtWindow* self = (ArtWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
        case WM_PAINT: {
            // Vulkan (renderIfDirty(), pumped from PlayerWindow::run()) owns
            // presentation — just validate the region. WM_PAINT fires whenever
            // Windows exposes this window (uncovered, etc.) independent of our
            // own markDirty() call sites, so it must still arm a redraw.
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            if (self) self->markDirty();
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE && self) self->hide();
            return 0;
        case WM_LBUTTONDBLCLK:
            if (self) self->hide();
            return 0;
        case WM_SETCURSOR:
            // Fullscreen artwork is a viewing surface — no cursor over it.
            SetCursor(nullptr);
            return TRUE;
        case WM_DESTROY:
            if (self) { self->renderer_.reset(); self->vkSurface_.reset(); }
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

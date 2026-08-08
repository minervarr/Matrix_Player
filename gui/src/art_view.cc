#include "art_view.hh"
#include "app_paths.hh"
#include "ui_fonts.hh"
#include "art_texture.hh"
#include "ui_metrics.hh"
#include "theme.hh"
#include "host.hh"
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <cmath>   // both platforms: drawFrame()'s whole-pixel floor()

// Draw from the app palette (theme.hh), not the framework's bluer col:: default.
static Color toColor(ColorRef c) {
    return { GetRValue(c) / 255.0f, GetGValue(c) / 255.0f, GetBValue(c) / 255.0f, 1.0f };
}

#ifdef _WIN32

static const wchar_t* ART_CLASS = L"MatrixArtWindow";

bool ArtWindow::create(Host*, const RasterFont* shareFontsWith) {
    HINSTANCE hInst = GetModuleHandleW(nullptr);
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

    auto toUtf8Path = [&](const char* rel) -> std::string {
        // rel is always a plain ASCII relative path (see ui_fonts.hh) — safe
        // to widen char-for-char rather than pull in a full MultiByteToWideChar
        // round trip for what's never anything but ASCII.
        std::wstring wpath = exeDirW + std::wstring(rel, rel + std::strlen(rel));
        int len = WideCharToMultiByte(CP_UTF8, 0, wpath.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string path(len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wpath.c_str(), -1, path.data(), len, nullptr, nullptr);
        if (!path.empty() && path.back() == '\0') path.pop_back();
        return path;
    };

    std::string fontPath = toUtf8Path(ui_fonts::regular());
    uiFont_.load(fontPath.c_str());

    openFonts(toUtf8Path(ui_fonts::bold()), toUtf8Path(ui_fonts::italic()),
              toUtf8Path(ui_fonts::mono()), toUtf8Path(ui_fonts::icons()),
              toUtf8Path("fonts/"), fontPath, shareFontsWith);

    return true;
}

void ArtWindow::show(const std::string& imagePath) {
    currentPath_ = imagePath;

    // Always the primary monitor — no call site has ever asked for a
    // specific one (the old preferMonitor param was never passed).
    HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(mon, &mi);

    int w = mi.rcMonitor.right  - mi.rcMonitor.left;
    int h = mi.rcMonitor.bottom - mi.rcMonitor.top;

    // Sized against the monitor's own resolution, before the window moves
    // there — renderer_->width() is still the hidden 800x800 at this point.
    loadArtTexture(w, h);

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

void ArtWindow::hide() {
    ShowWindow(hwnd_, SW_HIDE);
}

bool ArtWindow::isVisible() const {
    return hwnd_ && IsWindowVisible(hwnd_);
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

#else  // !_WIN32 — real Wayland second window, sharing the main window's
       // WaylandDisplay connection (see Host::secondaryWindowHandle()).

bool ArtWindow::create(Host* host, const RasterFont* shareFontsWith) {
    display_ = host ? static_cast<WaylandDisplay*>(host->secondaryWindowHandle()) : nullptr;
    if (!display_ || !display_->valid()) return false;

    window_ = std::make_unique<WaylandWindow>(*display_, "Matrix Player — Album Art",
                                               "matrix-player-art", 800, 800);
    if (!window_->valid()) return false;

    // Register for input on our own surface — this class is its own
    // InputSink (see onPointer()/onKey() below).
    display_->set_sink(window_->surface(), this);

    // Fullscreen artwork is a viewing surface — no cursor over it. Matches
    // the Windows branch's WM_SETCURSOR/SetCursor(nullptr) above. The policy
    // is per-surface and never changes, so registering it once here is enough:
    // the pointer gets the normal arrow back the moment it re-enters the main
    // window's surface (including when this one unmaps on hide()).
    display_->set_cursor_hidden(window_->surface(), true);

    surfaceProvider_ = std::make_unique<WaylandSurfaceProvider>(*display_, *window_);
    try {
        // 3 swapchain images, matching the Windows branch's own choice.
        renderer_ = std::make_unique<Renderer>(*surfaceProvider_, vkAssets_,
                                               /*desiredSwapchainImages=*/3);
    } catch (const std::exception&) {
        return false;
    }

    // Font loading mirrors the Windows branch (its own uiFont_/msdfFont_) via
    // the portable Host::exeDir() instead of GetModuleFileNameW — no
    // wide-char conversion needed on this platform.
    std::string exeDir = host->exeDir();
    std::string fontPath = exeDir + ui_fonts::regular();
    uiFont_.load(fontPath.c_str());

    openFonts(exeDir + ui_fonts::bold(), exeDir + ui_fonts::italic(),
              exeDir + ui_fonts::mono(), exeDir + ui_fonts::icons(),
              exeDir + "fonts/", fontPath, shareFontsWith);

    return true;
}

void ArtWindow::show(const std::string& imagePath) {
    currentPath_ = imagePath;
    if (!window_) return;

    // Compositor-chosen output (nullptr) — deliberately mirrors the Windows
    // branch's "always primary, no dual-monitor smarts" simplicity (that's
    // genuinely all Windows does today too — MonitorFromWindow(...,
    // MONITOR_DEFAULTTOPRIMARY), its old preferMonitor param was removed as
    // unused). set_fullscreen() blocks internally (wl_display_roundtrip)
    // until the compositor's configure lands, so window_/renderer_ extent
    // is already correct right after this call — no extra polling needed.
    window_->set_fullscreen(nullptr);
    window_->take_resized();   // drain; we call notifyResized() explicitly next

    // Rebuild the swapchain at the new (post-fullscreen) extent FIRST — only
    // then is renderer_->width()/height() below correct. This order is a
    // deliberate divergence from the Windows branch (which sizes the texture
    // off an independently-queried monitor rect before resizing the window);
    // on Linux the decode-size source only becomes correct after
    // notifyResized(), so don't reorder this to "match" Windows.
    if (renderer_) renderer_->notifyResized();

    if (renderer_)
        loadArtTexture((int)renderer_->width(), (int)renderer_->height());
    visible_ = true;
    markDirty();
}

void ArtWindow::hide() {
    if (!window_) return;
    window_->hide();             // unmap (NULL-buffer attach+commit) — otherwise the
                                  // last frame stays visible at some windowed size
    window_->unset_fullscreen();
    window_->take_resized();     // drain; irrelevant while hidden
    visible_ = false;
}

bool ArtWindow::isVisible() const { return visible_; }

void ArtWindow::onPointer(const PointerEvent& e) {
    if (e.action != PointerAction::Down || e.button != 0) return;
    // Synthesize double-click (Wayland/InputSink has no native dblclk event,
    // unlike Win32's WM_LBUTTONDBLCLK) — same ~400ms/small-radius heuristic
    // LinuxHost::onPointer() uses for the main window, duplicated locally.
    auto now = std::chrono::steady_clock::now();
    bool isDouble = lastDownValid_ &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - lastDown_).count() < 400 &&
        std::abs(e.x - lastDownX_) < 4 && std::abs(e.y - lastDownY_) < 4;
    lastDown_ = now;
    lastDownX_ = e.x; lastDownY_ = e.y;
    lastDownValid_ = true;
    if (isDouble) {
        hide();
        lastDownValid_ = false;  // don't chain a third click into another dblclk
    }
}

void ArtWindow::onKey(const KeyEvent& e) {
    if (e.down && e.keyCode == key::Escape) hide();
}

#endif  // _WIN32

// Open the faces and seed the glyph cache. Both platform branches of create()
// call this; ArtWindow keeps its OWN RasterFont rather than sharing
// PlayerWindow's, because the two windows have separate Renderers and an atlas
// texture belongs to one device queue.
//
// Under MTSDF the two shared an on-disk cache file so the second window to
// start got a cache hit instead of re-rasterizing a ~67 MB atlas. There is no
// cache now and none is wanted: this window draws a handful of labels, so its
// atlas is a few dozen cells that bake in well under a millisecond.
void ArtWindow::openFonts(const std::string& boldPath, const std::string& italicPath,
                          const std::string& monoPath, const std::string& iconPath,
                          const std::string& fontsDir, const std::string& regularPath,
                          const RasterFont* shareFontsWith) {
    // Only the FACE OPENING is shareable. The seeding at the bottom is not: it
    // bakes into this window's own atlas, on this window's own device, at this
    // window's own body size — which is the part that genuinely cannot be
    // borrowed (see RasterFont::openSharedWith).
    if (!shareFontsWith || !msdfFont_.openSharedWith(*shareFontsWith)) {
        FileByteReader loader;
        if (!msdfFont_.open(loader, regularPath.c_str())) return;

        msdfFont_.addStyle(loader, boldPath.c_str(),   FontStyle::Bold);
        msdfFont_.addStyle(loader, italicPath.c_str(), FontStyle::Italic);
        msdfFont_.addStyle(loader, monoPath.c_str(),   FontStyle::Math);

        msdfFont_.addOverride(loader, iconPath.c_str());
        // Same serif chain and the same matched Bold cuts as PlayerWindow — see
        // there for why these faces and not the sans/calligraphic ones bundled
        // beside them.
        msdfFont_.addFallback(loader, (fontsDir + "fandol/FandolSong-Regular.otf").c_str());
        msdfFont_.addFallback(loader, (fontsDir + "haranoaji/HaranoAjiMincho-Regular.otf").c_str());
        msdfFont_.addFallback(loader, (fontsDir + "unfonts-core/UnBatang.ttf").c_str());
        msdfFont_.addFallback(loader, (fontsDir + "fandol/FandolSong-Bold.otf").c_str(),
                              FontStyle::Bold);
        msdfFont_.addFallback(loader, (fontsDir + "haranoaji/HaranoAjiMincho-Bold.otf").c_str(),
                              FontStyle::Bold);
        msdfFont_.addFallback(loader, (fontsDir + "unfonts-core/UnBatangBold.ttf").c_str(),
                              FontStyle::Bold);
    }

    // Seed ASCII at the body size this window uses, so the very first frame has
    // something to draw. Everything else — other sizes, non-Latin track titles
    // — arrives through the miss path in drawFrame().
    std::vector<uint32_t> cps;
    for (uint32_t cp = 0x0020; cp <= 0x00FF; cp++) cps.push_back(cp);
    const int body = (int)(computeUiMetrics((float)renderer_->height()).text.body + 0.5f);
    if (msdfFont_.ensureGlyphs(cps, {body}) > 0 || !renderer_->msdfReady())
        renderer_->initMsdf(msdfFont_);
}


// ── Portable (both platforms): updateImage / markDirty / renderIfDirty / drawFrame ──

void ArtWindow::loadArtTexture(int boxW, int boxH) {
    if (artTex_ != kInvalidTexture) { renderer_->destroy_texture(artTex_); artTex_ = kInvalidTexture; }
    artTexW_ = artTexH_ = 0;
    if (!renderer_ || currentPath_.empty() || boxW <= 0 || boxH <= 0) return;

    // ImageFit::kContain + mips=false is the whole quality story for this view.
    // kContain returns the art resampled (Magic Kernel Sharp — see
    // img_decode.hh) to exactly the pixels it will occupy inside boxW x boxH,
    // so drawFrame() can blit it 1:1 and no GPU filter ever scales it. The
    // alternative — upload it at some other size and let VK_FILTER_LINEAR sort
    // it out — is a 2x2 bilinear tap per pixel, plus mip blending if a chain
    // exists, and that is exactly what made this view read softer than a
    // dedicated image viewer showing the same file. mips=false follows: at 1:1
    // level 0 is the only level the sampler can want.
    FileByteReader reader;
    artTex_ = createTextureFromImageFile(*renderer_, reader, currentPath_.c_str(),
                                         boxW, boxH, &artTexW_, &artTexH_,
                                         /*mips=*/false, ImageFit::kContain);
}

void ArtWindow::updateImage(const std::string& imagePath) {
    // Follow the now-playing album while fullscreen: called whenever the
    // transport art path changes (track/album/single boundary). Reloads the
    // texture in place — no repositioning, no swapchain churn.
    if (!isVisible() || imagePath == currentPath_) return;
    currentPath_ = imagePath;
    if (renderer_)
        loadArtTexture((int)renderer_->width(), (int)renderer_->height());
    markDirty();
}

void ArtWindow::markDirty() {
    // See PlayerWindow::markDirty() — same "one frame per swapchain image"
    // reasoning, but this window owns an independent Renderer/swapchain.
    pendingFrames_ = renderer_ ? renderer_->swapchainImageCount() + 1 : 1;
}

void ArtWindow::renderIfDirty() {
#ifndef _WIN32
    // Nothing else polls this window's resize/closed state (LinuxHost::pump()
    // only knows about the main window) — PlayerWindow::run() already calls
    // this unconditionally every loop tick while isVisible(), so it's the
    // natural place to drive it, keeping LinuxHost fully ignorant of ArtWindow.
    if (window_) {
        if (window_->take_resized() && renderer_) {
            renderer_->notifyResized();
            // The texture is sized for one exact view size (see
            // loadArtTexture); once that changes it is no longer a 1:1 blit,
            // so rebuild it rather than letting the GPU scale the old one.
            loadArtTexture((int)renderer_->width(), (int)renderer_->height());
            markDirty();
        }
        if (window_->closed()) hide();  // defensive parity with Windows' WM_DESTROY;
                                         // unlikely in a kiosk/fullscreen compositor
    }
#endif
    if (pendingFrames_ == 0) return;
    drawFrame();
    pendingFrames_--;
}

void ArtWindow::drawFrame() {
    if (!renderer_) return;

    // Before any quad is built, not after — see PlayerWindow::drawFrame() for
    // why the ordering matters (a bake between quad-building and the draw
    // re-normalises the sheet under quads that already carry old UVs).
    if (msdfFont_.hasMisses() && msdfFont_.bakeMisses() > 0) {
        renderer_->initMsdf(msdfFont_);
        markDirty();
    }

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
        // loadArtTexture() already resampled the art to fit this view, so
        // scale is 1 and this is a straight 1:1 blit. The min() stays as the
        // safety net for the one frame where a resize has landed but the
        // rebuilt texture has not (and for a load failure mid-resize).
        //
        // Whole-pixel destination: at 1:1 a half-pixel offset would put every
        // sample exactly between two texels, and bilinear would hand back the
        // average — undoing the entire point of resampling on the CPU. Both
        // axes floor for the same reason, so the pixel grids stay locked.
        float scale = std::min(canvas.w() / (float)artTexW_, canvas.h() / (float)artTexH_);
        float dstW = std::floor(artTexW_ * scale), dstH = std::floor(artTexH_ * scale);
        canvas.image(artTex_, std::floor((canvas.w() - dstW) * 0.5f),
                     std::floor((canvas.h() - dstH) * 0.5f), dstW, dstH);
    } else {
        canvas.clear(toColor(CLR_BG_MAIN));
        // One rarely-shown placeholder string, so this window doesn't need the
        // main UI's full per-role scale — but it does need to SCALE. The old
        // max(18.0f, kMinReadableTextSizePx) was always 18.29px (the floor
        // always won), which is nearly invisible on the 4K display a fullscreen
        // album-art window actually exists for. computeUiMetrics() is a free
        // function, so this window can use the same scale without a PlayerWindow.
        canvas.textCentered("No artwork", canvas.w() * 0.5f, canvas.h() * 0.5f,
                            computeUiMetrics(canvas.h()).text.body, toColor(CLR_TEXT_DIM));
    }
    renderer_->draw(frameCurves_, /*overlay_rotation_deg=*/0, frameImages_, {}, msdfQuads_,
                    frameShapes_);
}

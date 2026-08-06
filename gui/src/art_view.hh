#pragma once
#include <string>
#include <cstdint>
#include <vector>
#include <memory>

#ifdef _WIN32
#include "win32_platform.hh"
#else
#include "wayland_platform.hh"
#include "wayland_display.hh"
#include "wayland_window.hh"
#include "input.hh"
#include "keys.hh"
#include <chrono>
#endif
#include "renderer.hh"
#include "canvas.hh"
#include "texture.hh"
#include "font.hh"
#include "raster_font.hh"

class Host;  // forward decl only — see create(Host*), never dereferenced here

// Separate fullscreen artwork window — mirrors Android's ArtworkActivity.
// Own Vulkan Renderer (second surface/swapchain in the same process); driven
// from PlayerWindow::run()'s single per-thread message loop via drawFrame()
// when visible — there is no second message pump.
//
// Windows: own HWND + Vulkan swapchain, always opens on the primary monitor
// (no per-call monitor targeting — the old preferMonitor param was removed
// as unused). Linux: own WaylandWindow (second xdg_toplevel) sharing the
// main window's WaylandDisplay connection (see Host::secondaryWindowHandle),
// fullscreen on whichever output the compositor chooses — the same "no
// per-monitor smarts" simplicity as the Windows branch, not a lesser port.
// TODO(style): Add keep-screen-on (SetThreadExecutionState) like ArtworkActivity.
#ifdef _WIN32
class ArtWindow {
#else
class ArtWindow : public InputSink {
#endif
public:
    bool create(Host* host);
    void show(const std::string& imagePath);
    // Swap the displayed image in place while visible (now-playing album
    // changed). No-op when hidden or if the path is unchanged.
    void updateImage(const std::string& imagePath);
    void hide();
    bool isVisible() const;
    void drawFrame();

    // Dirty-flag render-on-demand, independent of PlayerWindow's (this window
    // owns its own Renderer/swapchain). markDirty() arms enough pending
    // frames to reach every swapchain image; renderIfDirty() draws and
    // consumes one only while a frame is pending, so PlayerWindow::run() can
    // drive it unconditionally every loop tick without over-rendering.
    void markDirty();
    void renderIfDirty();
    bool hasPendingFrames() const { return pendingFrames_ > 0; }

#ifdef _WIN32
    HWND hwnd() const { return hwnd_; }
#else
    // InputSink overrides — double-click or Escape closes the fullscreen art
    // surface itself, independent of (and in addition to) the transport
    // thumbnail's own double-click-to-close (see PlayerWindow::onLButtonDblClk).
    void onPointer(const PointerEvent&) override;
    void onWheel(const WheelEvent&) override {}
    void onKey(const KeyEvent&) override;
#endif

private:
    // (Re)builds artTex_ from currentPath_, resampled to exactly the pixels it
    // will occupy inside a boxW x boxH view — see the definition. Callers pass
    // the box explicitly because Windows sizes the texture before the window
    // moves to the monitor, when renderer_->width() is still the stale 800x800.
    void loadArtTexture(int boxW, int boxH);

#ifdef _WIN32
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    HWND hwnd_ = nullptr;
    std::unique_ptr<Win32SurfaceProvider> vkSurface_;
#else
    WaylandDisplay* display_ = nullptr;   // borrowed from Host, not owned
    std::unique_ptr<WaylandWindow> window_;
    std::unique_ptr<WaylandSurfaceProvider> surfaceProvider_;
    bool visible_ = false;
    // Double-click synthesis (Wayland/InputSink has no native dblclk event) —
    // same ~400ms/4px heuristic as LinuxHost::onPointer(), duplicated locally.
    std::chrono::steady_clock::time_point lastDown_;
    float lastDownX_ = 0, lastDownY_ = 0;
    bool lastDownValid_ = false;
#endif
    std::string currentPath_;
    uint32_t pendingFrames_ = 0;

    FileAssetReader                       vkAssets_;
    std::unique_ptr<Renderer>             renderer_;
    std::vector<float>                    frameCurves_;
    std::vector<float>                    frameShapes_;  // SDF shape quads
    std::vector<ImageDraw>                frameImages_;
    TextureHandle artTex_ = kInvalidTexture;
    int artTexW_ = 0, artTexH_ = 0;
    Font uiFont_;

    // Opens the faces and seeds the glyph cache; both platform branches of
    // create() call it. See its definition for why this window keeps its own
    // RasterFont rather than sharing PlayerWindow's.
    void openFonts(const std::string& boldPath, const std::string& italicPath,
                   const std::string& monoPath, const std::string& iconPath,
                   const std::string& fontsDir, const std::string& regularPath);

    RasterFont msdfFont_;
    std::vector<float> msdfQuads_;
};

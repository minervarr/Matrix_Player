#pragma once
#include <string>
#include <cstdint>
#include <vector>
#include <memory>

#ifdef _WIN32
#include "win32_platform.hh"
#else
#include "wayland_platform.hh"
#endif
#include "renderer.hh"
#include "canvas.hh"
#include "texture.hh"
#include "font.hh"
#include "msdf.hh"

// Separate fullscreen artwork window — mirrors Android's ArtworkActivity.
// Works great on dual-monitor setups: show art on one screen, controls on the
// other. Own Vulkan Renderer (second surface/swapchain in the same process);
// driven from PlayerWindow::run()'s single per-thread message loop via
// drawFrame() when visible — there is no second message pump.
//
// Windows: fully implemented (own HWND + Vulkan swapchain). Linux: NOT YET
// PORTED — a second xdg_toplevel is straightforward, but Wayland has no
// "show this window on monitor N" API a client can call the way Windows'
// HMONITOR-targeted CreateWindowExW allows, so the dual-monitor art-on-one-
// screen behavior needs a different design, not just a mechanical port.
// isVisible() always returns false and every other call is a no-op on
// Linux — a tracked gap, not a silently faked feature.
// TODO(style): Add keep-screen-on (SetThreadExecutionState) like ArtworkActivity.
class ArtWindow {
public:
    bool create();
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
#endif

private:
#ifdef _WIN32
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    HWND hwnd_ = nullptr;
    std::unique_ptr<Win32SurfaceProvider> vkSurface_;
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
    MsdfFont msdfFont_;
    std::vector<float> msdfQuads_;
};

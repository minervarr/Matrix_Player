// matrix_ui_capture — headless screenshots of the real UI, no window, no
// compositor, no DAC. Debug-only dev tooling (see gui/CMakeLists.txt).
//
// vk_canvas already had every piece of this (core/headless.hh's
// VK_EXT_headless_surface provider, Renderer::readbackLastFrame,
// core/capture/'s PNG runner); what was missing was the app-side half, because
// Matrix Player's UI is not a set of free functions that draw into a Canvas —
// it is PlayerWindow, which owns the DB, the font atlas, the layout and the
// hit-testing. So instead of vkc::capture_main's Scenario list (which suits an
// app whose states are pure draw functions), this tool gives PlayerWindow a
// Host whose window is a headless surface, and then drives the app itself:
// clicks go through the ordinary onLButtonDown(), frames through the ordinary
// drawFrame(). What lands in the PNG is what the app draws, not a replica.
//
//   ./matrix_ui_capture                       # every state -> ./ui-shots/
//   ./matrix_ui_capture --out DIR --frame 2560x1440 --only eq --list
//
// Deterministic given the same matrix_player.db sitting next to the binary —
// the shots show the real library, because a UI review of an empty grid is a
// review of nothing.

#include "player_view.hh"
#include "app_paths.hh"
#include "host.hh"
#include "headless.hh"
#include "wayland_platform.hh"   // FileAssetReader (exe-relative assets/)

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>
#include <unistd.h>   // readlink, for the exe-relative asset/DB directory
#include <mutex>
#include <thread>
#include <chrono>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace {

// ── The headless Host ───────────────────────────────────────────────────────
//
// Every method is either "answer honestly from the capture frame size" or "do
// nothing, there is no window". The two that matter:
//   - surfaceProvider() hands back a HeadlessSurfaceProvider, so Renderer's
//     swapchain/render-pass/pipeline path runs completely unchanged.
//   - secondaryWindowHandle() stays nullptr, which is exactly what makes
//     ArtWindow::create() decline on Linux — no second window is wanted here.
class HeadlessHost : public Host {
public:
    HeadlessHost(int w, int h) : w_(w), h_(h), surface_((uint32_t)w, (uint32_t)h) {}

    std::string exeDir() const override { return app_paths::exeDir(); }

    bool init(PlayerWindow*, UiMode) override { return true; }

    // Background completions. A real Host wakes its message pump and dispatches
    // these on the UI thread; there is no pump here, so they queue and main()
    // drains them between captures. Dropping them is not an option: albums
    // restored from the DB carry no tracks until the scan's onScanDone()
    // reattaches them (the DB keys albums by folder name and tracks by album
    // name), so a capture taken before that shows every album view empty.
    void postAppEvent(AppEvent id, intptr_t p1, intptr_t p2) override {
        std::lock_guard<std::mutex> lk(mu_);
        queued_.push_back({ id, p1, p2 });
    }

    // Returns true if a ScanDone passed through. UI-thread only, by contract:
    // main() is the only caller. RequestPlay is deliberately swallowed — a
    // screenshot session must not start playing music.
    bool drain(PlayerWindow& p) {
        std::vector<Ev> evs;
        {
            std::lock_guard<std::mutex> lk(mu_);
            evs.swap(queued_);
        }
        bool scanned = false;
        for (const Ev& e : evs) {
            switch (e.id) {
            case AppEvent::ScanDone:    p.onScanDone(); scanned = true;             break;
            case AppEvent::ArtDecoded:  p.onArtDecoded();                           break;
            case AppEvent::TrackChange: p.applyTrackMetadata((int)e.p1, (int)e.p2); break;
            case AppEvent::RequestPlay: break;
            }
        }
        return scanned;
    }

    SurfaceProvider& surfaceProvider() override { return surface_; }
    AssetReader&     assetReader()     override { return assets_; }

    void showWindow() override {}

    MonitorInfo primaryMonitor() const override {
        LayoutRect r{ 0, 0, w_, h_ };
        return { r, r };
    }

    void applyUiMode(UiMode) override {}
    void adaptToCurrentMonitor(UiMode) override {}
    void snapToEdge(int) override {}
    void invalidate() override {}
    void setCursor(CursorShape) override {}
    void setKeepAwake(bool) override {}
    void startTimer(TimerId, int) override {}
    void stopTimer(TimerId) override {}
    void pump(bool) override {}
    bool quitRequested() const override { return false; }

    void showErrorMessage(const std::string& title, const std::string& msg) override {
        fprintf(stderr, "[capture][ERROR] %s: %s\n", title.c_str(), msg.c_str());
    }

private:
    struct Ev { AppEvent id; intptr_t p1, p2; };

    int w_, h_;
    HeadlessSurfaceProvider surface_;
    FileAssetReader         assets_;
    std::mutex              mu_;
    std::vector<Ev>         queued_;
};

// The states captureGoTo() knows how to reach. Names double as file names, so
// they follow vk_canvas's NN-label convention and sort into reading order.
const char* kStates[] = {
    "10-grid-albums",
    "11-grid-eps",
    "12-grid-singles",
    "13-grid-compilations",
    "14-grid-live",
    "15-grid-remixes",
    "16-grid-hover",
    "17-grid-multiscript",
    "20-album-view",
    "21-album-view-multiscript",
    "30-settings",
    "31-manage-folders",
    "32-audio-settings",
    "33-eq-settings",
    "34-eq-all-profiles",
    "35-folder-picker",
    "36-playlists",
    "37-playlists-heavy-rotation",
    "38-playlists-forgotten-favourites",
    "39-playlists-never-heard",
    "3a-playlists-row-hover",
    "40-search",
    "41-search-suggest",
    "42-search-chips",
};

} // namespace

// PlayerWindow::create() still references make_host() on the path this tool
// never takes (it always injects its own). linux_host.cc, which defines it, is
// deliberately out of this target's source list — it owns main().
std::unique_ptr<Host> make_host() { return nullptr; }

int main(int argc, char** argv) {
    std::string out = "ui-shots";
    std::string only;
    int frameW = 2560, frameH = 1440;
    bool listOnly = false;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", what); exit(2); }
            return argv[++i];
        };
        if      (a == "--out")   out  = next("--out");
        else if (a == "--only")  only = next("--only");
        else if (a == "--list")  listOnly = true;
        else if (a == "--frame") {
            const char* v = next("--frame");
            if (sscanf(v, "%dx%d", &frameW, &frameH) != 2) {
                fprintf(stderr, "--frame wants WxH, got '%s'\n", v);
                return 2;
            }
        } else {
            fprintf(stderr,
                "usage: %s [--out DIR] [--frame WxH] [--only SUBSTR] [--list]\n", argv[0]);
            return 2;
        }
    }

    if (listOnly) {
        for (const char* s : kStates) printf("%s\n", s);
        return 0;
    }

    if (!headless_surface_supported()) {
        fprintf(stderr,
            "[capture] VK_EXT_headless_surface not advertised by this Vulkan loader/ICD.\n"
            "          (Mesa's lavapipe has it; so does radv/anv on a normal install.)\n");
        return 1;
    }

    auto hostOwned = std::make_unique<HeadlessHost>(frameW, frameH);
    HeadlessHost* host = hostOwned.get();

    PlayerWindow player;
    if (!player.create(std::move(hostOwned))) {
        fprintf(stderr, "[capture] PlayerWindow::create() failed\n");
        return 1;
    }

    // create() kicks off the background library scan, and until it reports in,
    // albums restored from the DB have no tracks hanging off them (see
    // HeadlessHost::drain). Waiting is what makes an album view show its
    // tracklist instead of an empty column.
    for (int i = 0; i < 600 && !host->drain(player); i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::filesystem::create_directories(out);

    int written = 0, failed = 0;
    for (const char* state : kStates) {
        if (!only.empty() && std::string(state).find(only) == std::string::npos) continue;

        host->drain(player);   // take delivery of anything the last capture kicked off
        if (!player.captureGoTo(state)) {
            fprintf(stderr, "[capture] %-22s SKIPPED (state unreachable — empty library?)\n", state);
            failed++;
            continue;
        }

        std::vector<uint8_t> rgba;
        uint32_t w = 0, h = 0;
        if (!player.captureFrame(rgba, w, h)) {
            fprintf(stderr, "[capture] %-22s readback failed\n", state);
            failed++;
            continue;
        }

        std::string png = out + "/" + state + ".png";
        if (!stbi_write_png(png.c_str(), (int)w, (int)h, 4, rgba.data(), (int)w * 4)) {
            fprintf(stderr, "[capture] %-22s PNG write failed\n", state);
            failed++;
            continue;
        }
        printf("[capture] %-22s %ux%u -> %s\n", state, w, h, png.c_str());
        written++;
    }

    player.shutdown();
    printf("[capture] %d written, %d failed\n", written, failed);
    return failed ? 1 : 0;
}

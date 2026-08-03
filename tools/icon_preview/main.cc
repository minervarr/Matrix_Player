// icon_preview — a standalone window that draws ONLY the UI icons, at a ladder
// of sizes, so icon work can be judged without launching the whole player.
//
// Why it exists: icons are MTSDF atlas glyphs, and an MTSDF bake is only sharp
// within a size range around its baked density. Judging that needs the icons
// side by side at the sizes the app actually draws them — which is exactly
// what this shows, with the real renderer and the real atlas.
//
// Build:  scripts/linux/build.sh --debug     (Debug-only, Linux-only)
// Run:    ./build/linux_debug/gui/icon_preview
//
// Keys:   wheel / Up / Down / PageUp / PageDown / Home / End   scroll
//         G  toggle the size-label / grid chrome
//         B  cycle background (app bg / black / white) — thin features read
//            very differently on light backgrounds
//         Esc / Q  quit
//    Resizing the window rescales the bottom "actual app sizes" row, so you
//    can see how the icons hold up from 1080p through 8K.

#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "canvas.hh"
#include "font.hh"
#include "input.hh"
#include "layout.hh"
#include "keys.hh"
#include "msdf.hh"
#include "renderer.hh"
#include "wayland_display.hh"
#include "wayland_platform.hh"
#include "wayland_window.hh"

#include "app_paths.hh"
#include "layout_rect.hh"
#include "theme.hh"
#include "ui_fonts.hh"
#include "ui_icons.hh"
#include "ui_metrics.hh"

namespace {

Color toColor(ColorRef c) {
    return { ((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f,
             (c & 0xFF) / 255.0f, 1.0f };
}

// keys.hh names only the letters it needed; letters sit at ASCII uppercase.
constexpr int kQ = 'Q', kG = 'G', kB = 'B';

struct IconEntry { UiIcon icon; const char* name; };
const IconEntry kIcons[] = {
    { UiIcon::Play,     "play"     },
    { UiIcon::Stop,     "stop"     },
    { UiIcon::Prev,     "prev"     },
    { UiIcon::Next,     "next"     },
    { UiIcon::Settings, "settings" },
    { UiIcon::Warning,  "warning"  },
    { UiIcon::Quality,  "quality"  },
};

// The sizes the app actually draws icons at, authored at the 1080 reference
// (see player_view.cc). Scaled by the live window height, so dragging the
// window bigger previews 4K/8K behaviour directly.
struct AppSize { const char* label; float px; };
const AppSize kAppSizes[] = {
    { "sidebar gear 30",  30.0f },
    { "warn strip 37",    37.0f },
    { "transport 71",     71.0f },
    { "essential 150",   150.0f },
};

// A geometric ladder, independent of the app, for spotting where a bake starts
// to break down.
const float kLadder[] = { 16, 24, 32, 48, 64, 96, 128, 192, 256 };

class Preview : public InputSink {
public:
    bool create() {
        display_ = std::make_unique<WaylandDisplay>();
        if (!display_->valid()) {
            std::fprintf(stderr, "icon_preview: no Wayland display\n");
            return false;
        }
        window_ = std::make_unique<WaylandWindow>(*display_, "Matrix Player — Icon Preview",
                                                  "matrix-player-icon-preview", 1400, 900);
        if (!window_->valid()) return false;
        display_->set_sink(window_->surface(), this);

        surfaceProvider_ = std::make_unique<WaylandSurfaceProvider>(*display_, *window_);
        try {
            renderer_ = std::make_unique<Renderer>(*surfaceProvider_, vkAssets_, 3);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "icon_preview: renderer failed: %s\n", e.what());
            return false;
        }

        // Same fonts, same cache, same bake as the player — this must preview
        // the real atlas, not a private one.
        const std::string dir = exeDir();
        const std::string fontPath = dir + ui_fonts::regular();
        uiFont_.load(fontPath.c_str());
        const std::string cachePath = app_paths::stateDir() + ui_fonts::cacheFile();

        FileByteReader loader;
        if (!msdfFont_.generate(loader, fontPath.c_str(), cachePath.c_str())) {
            std::fprintf(stderr, "icon_preview: font atlas failed\n");
            return false;
        }
        std::vector<uint32_t> cps(std::begin(kIconCodepoints), std::end(kIconCodepoints));
        const std::string iconPath = dir + ui_fonts::icons();
        if (msdfFont_.bakeCodepoints(loader, iconPath.c_str(), cps) > 0)
            msdfFont_.saveCache(cachePath.c_str());

        for (unsigned cp : kIconCodepoints) {
            if (!msdfFont_.hasCodepoint(cp)) {
                std::fprintf(stderr,
                             "icon_preview: U+%04X missing — showing PRIMITIVE fallback\n", cp);
                fallbackActive_ = true;
            }
        }
        std::printf("icon_preview: atlas %ux%u, icons %s\n", msdfFont_.atlasW(),
                    msdfFont_.atlasH(), fallbackActive_ ? "MISSING (fallback)" : "baked");

        renderer_->initMsdf(msdfFont_);
        msdfFont_.releaseAtlasPixels();
        return true;
    }

    void run() {
        while (!window_->closed() && !quit_) {
            if (!display_->dispatch(/*timeout_ms=*/16)) break;  // compositor gone
            if (window_->take_resized()) renderer_->notifyResized();
            draw();
        }
    }

    // ── InputSink ────────────────────────────────────────────────────────
    void onKey(const KeyEvent& e) override {
        if (!e.down) return;
        const float page = renderer_ ? renderer_->height() * 0.9f : 600.0f;
        if (e.keyCode == key::Escape || e.keyCode == kQ) quit_ = true;
        else if (e.keyCode == kG)            chrome_ = !chrome_;
        else if (e.keyCode == kB)            bg_ = (bg_ + 1) % 3;
        else if (e.keyCode == key::Down)     scrollY_ += 60.0f;
        else if (e.keyCode == key::Up)       scrollY_ -= 60.0f;
        else if (e.keyCode == key::PageDown) scrollY_ += page;
        else if (e.keyCode == key::PageUp)   scrollY_ -= page;
        else if (e.keyCode == key::Home)     scrollY_ = 0.0f;
        else if (e.keyCode == key::End)      scrollY_ = contentH_;  // clamped in draw()
    }
    void onPointer(const PointerEvent&) override {}
    // deltaY is positive scrolling up/away, so subtract to move the view down.
    void onWheel(const WheelEvent& e) override { scrollY_ -= e.deltaY * 60.0f; }

private:
    static std::string exeDir() {
        char buf[4096];
        ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n <= 0) return "./";
        buf[n] = '\0';
        std::string p(buf);
        return p.substr(0, p.find_last_of('/') + 1);
    }

    ColorRef background() const {
        switch (bg_) {
        case 1:  return 0x000000;
        case 2:  return 0xF0F0F0;
        default: return CLR_BG_MAIN;
        }
    }
    ColorRef foreground() const { return bg_ == 2 ? 0x101010 : CLR_TEXT_PRIMARY; }

    void draw() {
        curves_.clear(); shapes_.clear(); images_.clear(); quads_.clear();
        Canvas c(curves_, renderer_->width(), renderer_->height(), &uiFont_,
                 0.0f, 0.0f, 0.0f, 0.0f);
        c.useImages(&images_);
        c.useShapes(&shapes_);
        if (msdfFont_.valid()) c.useMsdf(&msdfFont_, &quads_);

        c.clear(toColor(background()));
        const UiMetrics m = computeUiMetrics(c.h());
        const Color fg = toColor(foreground());
        const Color dim = toColor(bg_ == 2 ? 0x606060 : CLR_TEXT_DIM);

        const float pad = m.space(24.0f);
        // Content is much taller than any window, so everything below is laid
        // out in content space and shifted by the scroll offset. contentH_ is
        // measured by this same pass and used to clamp on the NEXT frame —
        // immediate mode, so it self-corrects in one frame.
        scrollY_ = clampScroll(scrollY_, contentH_, c.h());
        const float top = pad - scrollY_;
        float y = top;

        if (chrome_) {
            c.textStyled("Icon preview — wheel/PgUp/PgDn scroll · G grid/labels · "
                         "B background · Esc quit",
                         pad, y, m.text.caption, dim, FontStyle::Italic);
        }
        y += m.space(28.0f);

        // ── Row 1: the geometric ladder ──────────────────────────────────
        if (chrome_) {
            c.textStyled("size ladder", pad, y, m.text.caption, dim, FontStyle::Bold);
            y += m.space(22.0f);
        }
        for (const IconEntry& e : kIcons) {
            float x = pad;
            const float rowH = kLadder[sizeof(kLadder) / sizeof(kLadder[0]) - 1];
            for (float s : kLadder) {
                LayoutRect rc{ (int)x, (int)(y + (rowH - s)), (int)(x + s), (int)(y + rowH) };
                drawIcon(c, rc, e.icon, fg);
                if (chrome_) {
                    char buf[16];
                    std::snprintf(buf, sizeof(buf), "%d", (int)s);
                    c.text(buf, x, y + rowH + 2.0f, m.text.caption * 0.8f, dim);
                }
                x += s + m.space(14.0f);
            }
            if (chrome_)
                c.textStyled(e.name, x + m.space(8.0f), y + rowH * 0.5f,
                             m.text.caption, dim, FontStyle::Italic);
            y += rowH + m.space(30.0f);
        }

        // ── Row 2: the sizes the app really uses, scaled to this window ──
        y += m.space(10.0f);
        if (chrome_) {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "actual app sizes  ×%.2f  (window %dpx tall — resize to preview 4K/8K)",
                          m.scale, (int)c.h());
            c.textStyled(buf, pad, y, m.text.caption, dim, FontStyle::Bold);
            y += m.space(24.0f);
        }
        float x = pad;
        for (const AppSize& a : kAppSizes) {
            const float s = m.space(a.px);
            float colX = x;
            for (const IconEntry& e : kIcons) {
                LayoutRect rc{ (int)colX, (int)y, (int)(colX + s), (int)(y + s) };
                drawIcon(c, rc, e.icon, fg);
                colX += s + m.space(10.0f);
            }
            if (chrome_)
                c.text(a.label, x, y + s + m.space(4.0f), m.text.caption * 0.85f, dim);
            y += s + m.space(34.0f);
        }

        // Measure what this pass actually laid out (content space, not screen).
        contentH_ = (y - top) + pad;

        // Scroll position indicator — without it there's no cue that anything
        // is below the fold, which is exactly how the first version misled.
        if (contentH_ > c.h()) {
            const float trackW = m.stroke(4.0f);
            const float trackX = c.w() - trackW - m.space(6.0f);
            c.rect(trackX, 0.0f, trackW, c.h(), toColor(CLR_SEPARATOR));
            const float frac = c.h() / contentH_;
            const float thumbH = std::max(m.space(30.0f), c.h() * frac);
            const float maxScroll = contentH_ - c.h();
            const float t = maxScroll > 0.0f ? scrollY_ / maxScroll : 0.0f;
            c.rect(trackX, t * (c.h() - thumbH), trackW, thumbH, toColor(CLR_ACCENT));
        }

        renderer_->draw(curves_, 0, images_, {}, quads_, shapes_);
    }

    // Mirrors player_view.cc's dispatcher: glyph first, primitives as fallback.
    // Kept trivial on purpose — this tool previews the glyph path.
    static void drawIcon(Canvas& c, const LayoutRect& rc, UiIcon icon, Color col) {
        if (drawUiIconGlyph(c, rc, icon, col)) return;
        // No primitive fallback linked in here: a blank cell is the honest
        // signal that the glyph is missing (create() already warned).
    }

    std::unique_ptr<WaylandDisplay>         display_;
    std::unique_ptr<WaylandWindow>          window_;
    std::unique_ptr<WaylandSurfaceProvider> surfaceProvider_;
    std::unique_ptr<Renderer>               renderer_;
    FileAssetReader                         vkAssets_;
    Font                                    uiFont_;
    MsdfFont                                msdfFont_;
    std::vector<float> curves_, shapes_, quads_;
    std::vector<ImageDraw> images_;
    bool  chrome_ = true, quit_ = false, fallbackActive_ = false;
    int   bg_ = 0;
    float scrollY_ = 0.0f;    // px, clamped against contentH_ each frame
    float contentH_ = 0.0f;   // measured by the previous draw() pass
};

}  // namespace

int main() {
    Preview p;
    if (!p.create()) return 1;
    p.run();
    return 0;
}

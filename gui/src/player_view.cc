#include "player_view.hh"
#include "app_paths.hh"
#include "ui_text.hh"
#include "log_util.h"
#include "ui_fonts.hh"
#include "ui_icons.hh"
#include "art_texture.hh"
#include "img_decode.hh"
#include "text_util.hh"
#include "utf8.hh"
#include <cstdio>
#include <cstdlib>
#include <climits>
#include <cmath>
#include <chrono>
#include <vector>
#include <algorithm>
#include <stdexcept>
#include <filesystem>
#include <fstream>

#include <soxr.h>
#include "core/dsp/dither.h"   // ae::TpdfQuantizer — Reference EQ output stage
#include "core/facets.h"       // facets::albumYear — Album carries no year field

static int pickOutputRate(int inRate, const std::vector<int>& supported) {
    for (int r : supported)
        if (r % inRate == 0) return r;
    for (int r : supported)
        if (r > inRate) return r;
    return supported.empty() ? 48000 : supported.back();
}

// TPDF dither + the single final quantize now live in the engine
// (framework/audio_engine/core/include/core/dsp/dither.h) so that the engine's
// dsp_null_test can assert their output against a frozen reference. The state
// is file-static here for the same reason it always was: the dither sequence
// must continue across callbacks, not restart every buffer.
static ae::TpdfQuantizer s_quantizer;

// First-order noise-shaped dither, used only for 16-bit output (see below) —
// the depth where the dither/quantization step is largest relative to the
// signal and shaping's noise-floor benefit is actually audible. 24/32-bit
// keep using the flat TpdfQuantizer above, unchanged: 32-bit skips dither
// entirely (error already below any DAC's noise floor) and 24-bit's error is
// small enough that shaping isn't worth its own state/oracle for this app.
static ae::NoiseShapedQuantizer s_shapedQuantizer;

static void ditherAndQuantize(const double* in, int32_t* out, int n, int bits, int channels) {
    if (bits == 16) s_shapedQuantizer.process(in, out, n, bits, channels);
    else            s_quantizer.process(in, out, n, bits);
}

#ifdef _WIN32
// Only used for wasapiDeviceId_ (WASAPI device IDs are wchar_t at the OS
// boundary) — everything else uses UTF-8 std::string directly (see
// currentTitle_/currentArtist_ and the Audio Settings panel).
static std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    return w;
}
static std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}
#endif

// Encode one Unicode codepoint as UTF-8, appended to out. Shared by
// onCharPortable() (album search) and onPanelChar() (EQ profile search) —
// both receive codepoints the same way (see onCharPortable's own comment on
// why no wide-char detour is needed here).
static void appendUtf8(std::string& out, uint32_t codepoint) {
    char u8[4]; int n = 0;
    if (codepoint < 0x80) {
        u8[n++] = (char)codepoint;
    } else if (codepoint < 0x800) {
        u8[n++] = (char)(0xC0 | (codepoint >> 6));
        u8[n++] = (char)(0x80 | (codepoint & 0x3F));
    } else if (codepoint < 0x10000) {
        u8[n++] = (char)(0xE0 | (codepoint >> 12));
        u8[n++] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        u8[n++] = (char)(0x80 | (codepoint & 0x3F));
    } else {
        u8[n++] = (char)(0xF0 | (codepoint >> 18));
        u8[n++] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
        u8[n++] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        u8[n++] = (char)(0x80 | (codepoint & 0x3F));
    }
    out.append(u8, n);
}

// Portable home directory, for the folder-picker panel's initial location.
static std::string userHomeDir() {
#ifdef _WIN32
    const char* h = getenv("USERPROFILE");
#else
    const char* h = getenv("HOME");
#endif
    return h ? std::string(h) : std::string("/");
}

static const char* backendDisplayName(AudioBackend b) {
    switch (b) {
    case AudioBackend::Usb:    return "USB Direct";
    case AudioBackend::Wasapi: return "WASAPI";
    case AudioBackend::Alsa:   return "ALSA";
    case AudioBackend::Jack:   return "JACK";
    }
    return "?";
}

// ── GDI -> Canvas bridges (Phase 6) ──────────────────────────────────────────
// recalcLayout() keeps computing the same int LayoutRects it always did;
// these just let drawFrame() consume them (and the existing ColorRef
// palette) without re-deriving layout math or a parallel color table.
static Rect toRect(const LayoutRect& r) {
    return { (float)r.left, (float)r.top, (float)(r.right - r.left), (float)(r.bottom - r.top) };
}
static LayoutRect toLayoutRect(const Rect& r) {
    return { (int)r.x, (int)r.y, (int)(r.x + r.w), (int)(r.y + r.h) };
}
static Color toColor(ColorRef c, float a = 1.0f) {
    return { GetRValue(c) / 255.0f, GetGValue(c) / 255.0f, GetBValue(c) / 255.0f, a };
}

// The app's palette/selection language for the reusable vk_canvas widgets
// (radio rows + scroll lists), defined once so every settings panel shares one
// look. Defaults in the framework reproduce vk_canvas's own col:: theme; these
// override them with Matrix Player's green accent + bottom-border selection.
static widgets::RadioStyle matrixRadioStyle() {
    widgets::RadioStyle s;
    s.dotOn   = toColor(CLR_ACCENT);   s.dotOff  = toColor(CLR_SEPARATOR);
    s.textOn  = toColor(CLR_ACCENT);   s.textOff = toColor(CLR_TEXT_PRIMARY);
    s.hoverBg = toColor(CLR_HOVER);              // grey pill behind a hovered row
    s.selBg   = toColor(CLR_ACCENT, 0.16f);      // accent-tint pill behind the selected row
    s.selBar  = toColor(CLR_ACCENT);             // thin green left bar on the selected row
    s.radius  = UI_CORNER_RADIUS;
    return s;
}
static widgets::ScrollListStyle matrixListStyle() {
    widgets::ScrollListStyle s;
    s.background  = toColor(CLR_BG_MAIN);         // invisible against the page bg
    s.rowText     = toColor(CLR_TEXT_PRIMARY);
    s.hoverBg     = toColor(CLR_HOVER);           // grey pill on hover
    s.selection   = widgets::ListSelectionStyle::Pill;
    s.pillColor   = toColor(CLR_ACCENT, 0.16f);   // accent-tint pill on the selected row
    s.pillText    = toColor(CLR_ACCENT);          // green text on the selected row
    s.selectedBar = toColor(CLR_ACCENT);          // thin green left bar
    s.radius      = UI_CORNER_RADIUS;
    s.fitWidth    = true;                          // hug each row's text (matches radio rows)
    return s;
}

// One text-input field, shared by the sidebar album search and the EQ-profile
// search: CLR_INPUT_BG fill at the uniform radius, a bottom underline that
// turns accent on focus, dim placeholder when empty+unfocused, and a caret
// while focused. Keeps both searches visually identical (see UI_DESIGN_SYSTEM).
static void drawSearchField(Canvas& canvas, const LayoutRect& rc, const std::string& text,
                            bool focused, const char* placeholder, float textSize) {
    Rect s = toRect(rc);
    canvas.rect(s.x, s.y, s.w, s.h, toColor(CLR_INPUT_BG), UI_CORNER_RADIUS);
    canvas.rect(s.x, s.y + s.h - 1, s.w, 1, toColor(focused ? CLR_ACCENT : CLR_SEPARATOR));
    bool empty = text.empty() && !focused;
    std::string shown = empty ? placeholder : text;
    ColorRef clr = empty ? CLR_TEXT_DIM : CLR_TEXT_PRIMARY;
    std::string fit = truncateToWidth(canvas, shown, s.w - 16 - 8, textSize, FontStyle::Roman);
    if (focused) fit += "|";
    canvas.text(fit, s.x + 8, s.y + s.h * 0.5f - textSize * 0.5f, textSize, toColor(clr));
}

// truncateToWidth / splitTwoLines / wrapText / stripHtmlToPlain moved into
// vk_canvas (core/text_util.hh) so other apps can reuse them.

// splitNameModifier() and its two helpers used to live here, file-static.
// They moved to core/src/variants.cpp when album-variant grouping arrived:
// the grouping and this file MUST split names identically, or a group's key
// disagrees with the tile that represents it. See core/include/core/variants.h.

// ── Window creation ──────────────────────────────────────────────────────────
// MAIN_CLASS, kFixedWindowStyle/kFixedWindowExStyle, and the window-creation/
// message-loop/monitor-rect logic that used to live in this section now live
// in os/windows_host.cc (WindowsHost) / os/linux_host.cc (LinuxHost) — see
// host.hh. Hotkey IDs are in hotkey_ids.hh (shared by both hosts).

// Defined further down, next to the icon bake it exists to protect.
static void pruneStaleCaches(const std::string& dir, const std::string& keepPath);

bool PlayerWindow::create(std::unique_ptr<Host> injectedHost) {
    host_ = injectedHost ? std::move(injectedHost) : make_host();

    // Fixed, non-resizable window — both UI modes are fixed sizes the app
    // itself sets on toggle (toggleUiMode()), never left to interactive
    // resize/maximize. Starting mode: Complete (true fullscreen) if the
    // primary monitor is tall enough to clear kMinWindowContentH, the font's
    // geometric legibility floor (see player_view.hh); otherwise Essential,
    // which always fits by construction (its size is *derived* from the
    // monitor's own dimensions — see Host::applyUiMode()'s implementation).
    MonitorInfo primaryMon = host_->primaryMonitor();
    int monitorH = primaryMon.bounds.bottom - primaryMon.bounds.top;
    uiMode_ = (monitorH >= (int)std::ceil(kMinWindowContentH))
        ? UiMode::Complete : UiMode::Essential;

    if (!host_->init(this, uiMode_)) return false;

    // Vulkan rendering (vk_canvas). Must come after host_->init() (the
    // surface provider wraps the now-created native window) and before the
    // window is shown, so the first frame presents as soon as it's visible.
    try {
        // 3 swapchain images: enough for MAILBOX on desktop (the default 4
        // is an Android compositor-hitch allowance) — saves one full-screen
        // RGBA8 image (~8 MB at 1080p).
        renderer_ = std::make_unique<Renderer>(host_->surfaceProvider(), host_->assetReader(),
                                               /*desiredSwapchainImages=*/3);
    } catch (const std::exception& e) {
        host_->showErrorMessage("Vulkan initialization failed", e.what());
        return false;
    }

    // (UI icons are drawn as native vector shapes each frame — see
    // drawUiIcon(). No SVG rasterization or icon textures at startup.)

    // Open DB and restore the library BEFORE font setup below — baking
    // script-fallback glyphs (Cyrillic/Greek/CJK/…) needs to scan the
    // library's actual track/album/artist text for which non-Latin
    // codepoints it must cover.
    std::string exeDir = host_->exeDir();
    db_.open(app_paths::stateDir() + "matrix_player.db");

    // Restore library from DB
    {
        auto raw = db_.loadAlbums();
        for (auto& a : raw) {
            bool dup = false;
            for (auto& ex : albums_)
                if (ex.name == a.name && ex.artist == a.artist) { dup = true; break; }
            if (!dup) albums_.push_back(std::move(a));
        }
        if (!albums_.empty()) {
            auto allTracks = db_.loadTracks();
            for (auto& t : allTracks)
                for (auto& a : albums_)
                    if (a.name == t.album) { a.tracks.push_back(t); break; }

            // Re-derive the structured fields (displayName/quality/mode/
            // country) from each album's folder. Rows written before those
            // columns existed carry the raw name as displayName, and the
            // incremental rescan skips unchanged files so it would never
            // heal them — parse here instead (cheap: regex + path split per
            // album). The next saveAlbums() persists the result.
            auto roots = db_.loadMusicRoots();
            for (auto& a : albums_) {
                if (a.tracks.empty()) continue;
                const std::string& fp = a.tracks[0].filePath;
                size_t sep = fp.find_last_of("\\/");
                if (sep == std::string::npos) continue;
                std::string folder = fp.substr(0, sep);
                std::string root;
                for (auto& r : roots)
                    if (folder.rfind(r, 0) == 0 && r.size() > root.size()) root = r;
                parseAlbumFolder(folder, root, a);
            }
        }
        rebuildAlbumGroups();   // must precede rebuildGridIndices — it feeds it
        rebuildGridIndices();
    }

    // UI font (fonts/ is copied next to the exe by the CMake build step).
    {
        auto toUtf8Path = [&](const char* rel) -> std::string {
            return exeDir + rel;
        };

        std::string fontPath = toUtf8Path(ui_fonts::regular());
        uiFont_.load(fontPath.c_str());
        fontsDir_ = toUtf8Path("fonts/");

        // Sweep the MTSDF atlas caches this build no longer writes. They are
        // ~45-67 MB each, and the raster path has no disk cache at all — a
        // rasterized glyph costs 10-50us against MTSDF's 1-10ms, so the whole
        // few-thousand-cell set is baked at startup faster than a cache file
        // could be read. That deletes the version word, the fingerprinted
        // filename, and every way a stale bake could be served.
        pruneStaleCaches(app_paths::stateDir(), std::string());

        // The faces. Order matters only for the fallbacks: a codepoint the
        // primary face lacks is served by the first fallback that has it.
        FileByteReader loader;
        if (msdfFont_.open(loader, fontPath.c_str())) {
            // Bold (headers/titles), Italic (artist/secondary text), and Mono
            // (repurposing the unused Math slot — this app never renders math)
            // for numeric readouts so digits don't jitter as they change.
            msdfFont_.addStyle(loader, toUtf8Path(ui_fonts::bold()).c_str(),   FontStyle::Bold);
            msdfFont_.addStyle(loader, toUtf8Path(ui_fonts::italic()).c_str(), FontStyle::Italic);
            msdfFont_.addStyle(loader, toUtf8Path(ui_fonts::mono()).c_str(),   FontStyle::Math);

            // Icons first, then the scripts New Computer Modern does not cover.
            // Under MTSDF this order was load-bearing — the sheet had a hard
            // 4096px ceiling and whatever came last was silently dropped, which
            // is precisely how Japanese and Korean ended up baking nothing.
            // Here it only decides which face wins a codepoint two of them
            // both have, but the order is kept because it is still the right
            // priority.
            msdfFont_.addOverride(loader, (exeDir + ui_fonts::icons()).c_str());

            // Song / Mincho / Batang: the Ming-Mincho-Myeongjo serif tradition,
            // with the stroke contrast and terminal serifs that make them read
            // as one family with New Computer Modern's serif Latin. The sans
            // cuts bundled beside them (Hei, Gothic, Dotum) and the
            // calligraphic ones (Kai, Fang, Gungseo, Pilgi) are deliberately
            // NOT registered — whichever face won a codepoint would decide the
            // look, and a track list would come out in mixed handwriting.
            //
            // Chinese -> Japanese -> Korean, and the order matters: all three
            // cover Han and Kana, and only the Korean face has Hangul.
            msdfFont_.addFallback(loader, (fontsDir_ + "fandol/FandolSong-Regular.otf").c_str());
            msdfFont_.addFallback(loader, (fontsDir_ + "haranoaji/HaranoAjiMincho-Regular.otf").c_str());
            msdfFont_.addFallback(loader, (fontsDir_ + "unfonts-core/UnBatang.ttf").c_str());

            // The matched Bold cuts. Grid titles, page headers and album titles
            // are all drawn FontStyle::Bold, so without these a Korean or
            // Chinese title renders at regular weight beside bold Latin — the
            // text is there, at the wrong weight, which is easy to miss.
            // Italic and Mono get no chain on purpose: these scripts have no
            // italic tradition, so they resolve to the regular face and share
            // its cells (see RasterFont::keyForStyle).
            msdfFont_.addFallback(loader, (fontsDir_ + "fandol/FandolSong-Bold.otf").c_str(),
                                  FontStyle::Bold);
            msdfFont_.addFallback(loader, (fontsDir_ + "haranoaji/HaranoAjiMincho-Bold.otf").c_str(),
                                  FontStyle::Bold);
            msdfFont_.addFallback(loader, (fontsDir_ + "unfonts-core/UnBatangBold.ttf").c_str(),
                                  FontStyle::Bold);
        }
    }

    // Glyph rasterization in compute. Brought up before the first bake so the
    // cache knows which path it is on from the start; if it fails, nothing is
    // enabled and everything stays on FreeType.
    // Glyph rasterization in compute — CORRECT, and currently SLOWER, so it is
    // off by default.
    //
    // Measured end to end at 8K (startup bake only, `--only zzzz`): FreeType
    // ~945 ms, compute ~1450 ms. The earlier "213 -> 159 ms" claim was not a
    // like-for-like measurement — that timer lives inside ensureGlyphs() and
    // stops before runGlyphBaker() is even called, so in GPU mode it timed
    // outline extraction and none of the GPU work.
    //
    // It is not shader throughput: the Intel iGPU and the RTX 3050 come out the
    // same to within noise, which rules out compute speed. The cost is
    // structural — the atlas is baked TWICE at 8K because growing it recreates
    // the image, plus a per-batch vkQueueWaitIdle and a serial per-edge upload.
    // Raising the batch budget to make it one dispatch was tried and made it
    // worse (1950 ms), so batch count is not it either.
    //
    // MATRIX_GPU_GLYPHS=1 turns it on. Quality is not the issue and never was:
    // area_raster_test holds the two rasterizers to within 4/255 on identical
    // geometry, and the rendered difference is 0.07% of pixels.
    if (std::getenv("MATRIX_GPU_GLYPHS") && renderer_ &&
        glyphBaker_.init(renderer_->vkDevice(),
                                      renderer_->vkPhysicalDevice(),
                                      host_->assetReader(),
                                      renderer_->vkCommandPool(),
                                      renderer_->vkQueue())) {
        msdfFont_.useGpuBake(true);
    }

    // NOT created here. See ensureArtWindow() — it opens a second Vulkan
    // device and (before sharing) re-read ~39 MB of faces, at launch, for a
    // window most sessions never open.
    recalcLayout();
    // Needs metrics_, which recalcLayout() just computed: the sizes to bake at
    // ARE the type roles. Anything drawn at a size not in that set (icon boxes,
    // the art window) is picked up by the miss path after the first frame that
    // asks for it — see refreshGlyphs().
    refreshGlyphs();

    // Last-played album (grid indicator, Fix C): stored as "name\x1fartist"
    // rather than an index, since the album list's order/indices shift
    // across rescans — matched by name+artist at draw time instead.
    {
        std::string lastPlayed = db_.loadSetting("last_played_album");
        size_t sep = lastPlayed.find('\x1f');
        if (sep != std::string::npos) {
            lastPlayedAlbumName_  = lastPlayed.substr(0, sep);
            lastPlayedArtistName_ = lastPlayed.substr(sep + 1);
        }
    }

    // NO resume point is read here, deliberately — see savePlaybackStateNow()
    // and the design law it serves: a record is heard from its start, because
    // the player exists to carry the artist's intended development, not to
    // return you to the middle of it. Dropping back in where you left off is
    // the same convenience as a scrubber, and this app has no scrubber and no
    // pause for the same reason.
    //
    // playback_state itself is left in place (db.cpp's SCHEMA) and still
    // records WHICH file was last playing; only the position is neither
    // written nor honoured.

    // Load EQ profiles — on a thread, because nothing needs them yet.
    //
    // This is ~150 ms of a ~730 ms startup: 8666 profiles out of a 4.8 MB JSON,
    // parsed and sorted, on the thread that is trying to put a window on screen.
    // Nothing drawn before the listener touches anything reads it — the sidebar's
    // DRIVER'S AUTOEQ block is fed from the eq_headphones TABLE, not from here —
    // and the first real reader is applyDeviceEq() at track start.
    //
    // See ensureEqProfiles() for the rule that makes this safe.
    eqProfilesThread_ = std::thread([this, exeDir] {
        eqProfiles_.load(exeDir + "eq_profiles.json");
    });

    setupWatchers();
    startBackgroundScan();

    // Load audio mode
    bitperfectMode_.store(db_.loadSetting("audio_mode") == "bitperfect");

    // Load audio backend. Default (nothing saved yet) is WASAPI Exclusive on
    // Windows / ALSA on Linux — never USB. USB is bit-perfect and the primary
    // path once chosen, but probing for it unconditionally on a fresh install
    // means a listener with no DAC plugged in sees a driver error as their
    // very first impression. The USB-open block below only ever runs when
    // audioBackend_ == Usb, so this default is what keeps that probe (and the
    // libusbK/Zadig check it implies) tied to an explicit opt-in.
    {
        std::string backend = db_.loadSetting("audio_backend");
#ifdef _WIN32
        audioBackend_ = AudioBackend::Wasapi;
        if (backend == "usb") audioBackend_ = AudioBackend::Usb;
#else
#ifdef MATRIX_HAVE_ALSA
        audioBackend_ = AudioBackend::Alsa;
#else
        audioBackend_ = AudioBackend::Usb;
#endif
        if (backend == "usb") audioBackend_ = AudioBackend::Usb;
#ifdef MATRIX_HAVE_ALSA
        if (backend == "alsa") audioBackend_ = AudioBackend::Alsa;
#endif
#ifdef MATRIX_HAVE_JACK
        if (backend == "jack") audioBackend_ = AudioBackend::Jack;
#endif
#endif
    }
#ifdef _WIN32
    wasapiMode_ = (db_.loadSetting("wasapi_mode") == "shared")
                  ? WasapiMode::Shared : WasapiMode::Exclusive;
    auto devIdUtf8 = db_.loadSetting("wasapi_device_id");
    wasapiDeviceId_ = utf8ToWide(devIdUtf8);
#else
#ifdef MATRIX_HAVE_ALSA
    {
        auto saved = db_.loadSetting("alsa_device_id");
        alsaDeviceId_ = saved.empty() ? "default" : saved;
    }
#endif
#ifdef MATRIX_HAVE_JACK
    jackStartPort_ = db_.loadSetting("jack_port");
#endif
#endif

    // Headphone switcher state. Loaded here — after audioBackend_ is settled,
    // since getActiveDeviceKey() reads it — so the sidebar block shows the
    // right pair before anything has played. Nothing is applied to EqManager
    // yet; onPlay() does that through applyDeviceEq() once a rate is known.
    reloadEqHeadphones();
    {
        EqAssignment assign;
        if (db_.loadEqAssignment(getActiveDeviceKey(), assign) ||
            db_.loadEqAssignment("global", assign)) {
            eqCurrent_ = assign;
            eqCurrentTentative_ = !isKnownHeadphone(assign);
        }
    }

    if (audioBackend_ == AudioBackend::Usb) {
        auto vidStr = db_.loadSetting("usb_vid");
        auto pidStr = db_.loadSetting("usb_pid");
        uint16_t vid = vidStr.empty() ? (uint16_t)0x32BB : (uint16_t)strtoul(vidStr.c_str(), nullptr, 16);
        uint16_t pid = pidStr.empty() ? (uint16_t)0x0004 : (uint16_t)strtoul(pidStr.c_str(), nullptr, 16);

        usbOpen_ = usbDriver_.open(vid, pid);
        if (usbOpen_) {
            usbDriver_.parseDescriptors();
            auto rates = usbDriver_.getOutputRates();
            printf("[USB] DAC opened (VID=%04X PID=%04X). Supported rates:", vid, pid);
            for (int r : rates) printf(" %d", r);
            printf("\n");
        } else {
            printf("[USB][ERROR] Failed to open DAC VID=%04X PID=%04X\n", vid, pid);
            char msgBuf[96];
            snprintf(msgBuf, sizeof(msgBuf),
                "USB DAC not found (VID=%04X PID=%04X) \xE2\x80\x94 check Audio Settings.",
                vid, pid);
            audioNotice_ = msgBuf;
        }
        output_ = std::make_unique<UsbAudioOutput>(usbDriver_);
    }
#ifdef _WIN32
    else if (audioBackend_ == AudioBackend::Wasapi) {
        output_ = std::make_unique<WasapiOutput>(wasapiDeviceId_, wasapiMode_);
        printf("[Audio] WASAPI backend selected (%s mode)\n",
               wasapiMode_ == WasapiMode::Exclusive ? "exclusive" : "shared");
    }
#else
#ifdef MATRIX_HAVE_ALSA
    else if (audioBackend_ == AudioBackend::Alsa) {
        output_ = std::make_unique<AlsaOutput>(alsaDeviceId_);
        printf("[Audio] ALSA backend selected (device=%s)\n", alsaDeviceId_.c_str());
    }
#endif
#ifdef MATRIX_HAVE_JACK
    else if (audioBackend_ == AudioBackend::Jack) {
        output_ = std::make_unique<JackOutput>(jackStartPort_);
        printf("[Audio] JACK backend selected (start port=%s)\n",
               jackStartPort_.empty() ? "auto" : jackStartPort_.c_str());
    }
#endif
#endif

    // Arm the first frame explicitly rather than relying on an initial
    // resize/configure event to do it: this window is fixed-size and
    // non-resizable, so on Wayland no such event is guaranteed to arrive
    // before the compositor is itself waiting on our first buffer commit to
    // map the surface (a real deadlock otherwise — confirmed by tracing
    // WAYLAND_DEBUG=1 output: the surface never gets attach()/commit() and
    // the window never appears, even though the process runs and logs look
    // healthy). Windows happens to get an implicit initial WM_SIZE that
    // covers this, but that's a fragile assumption to depend on there too.
    invalidate();

    host_->showWindow();
    return true;
}

// Delete atlas caches written for a superseded icon set or font. Each is
// ~45 MB, and the fingerprinted filename (ui_fonts.hh) means a new one appears
// on every icon change — without this they would pile up silently.
static void pruneStaleCaches(const std::string& dir, const std::string& keepPath) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const std::string keep = fs::path(keepPath).filename().string();
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (ec) return;
        if (!e.is_regular_file(ec)) continue;
        const std::string n = e.path().filename().string();
        if (n == keep) continue;
        const std::string suf = ui_fonts::cacheSuffix();
        if (n.size() < suf.size() || n.compare(n.size() - suf.size(), suf.size(), suf) != 0)
            continue;
        if (fs::remove(e.path(), ec))
            printf("[Fonts] pruned stale atlas cache %s\n", n.c_str());
    }
}

// Bake every codepoint this app can draw, at every size it draws them at,
// then push the grown atlas to the GPU. Idempotent: cells already present are
// skipped, so calling it again after a rescan costs only what is genuinely new.
//
// This one function replaces the MTSDF path's bakeIconGlyphs() +
// bakeFallbackGlyphs() pair and their disk-cache dance (re-hydrate the CPU
// atlas, append, save, release). A raster atlas is never released and never
// written, because there is nothing expensive to avoid recomputing.
void PlayerWindow::refreshGlyphs() {
    if (!renderer_) return;

    // ── What to draw ────────────────────────────────────────────────────────
    std::vector<uint32_t> cps;

    // ASCII + Latin-1, Greek and Cyrillic: cheap, bounded, and enough to cover
    // most European-language metadata outright with no library scan involved.
    for (uint32_t cp = 0x0020; cp <= 0x00FF; cp++) cps.push_back(cp);
    for (uint32_t cp = 0x0370; cp <= 0x04FF; cp++) cps.push_back(cp);

    // Latin Extended-A/B — Polish, Czech, Turkish, Vietnamese base letters.
    for (uint32_t cp = 0x0100; cp <= 0x024F; cp++) cps.push_back(cp);

    // General punctuation this UI's own prose and real metadata actually use.
    // An em dash coming out as a blank gap is worse than a wrong glyph:
    // nothing about a gap says a character is missing, so the sentence just
    // reads broken.
    static const uint32_t kPunct[] = {
        0x2010, 0x2013, 0x2014,          // hyphen, en dash, em dash
        0x2018, 0x2019, 0x201C, 0x201D,  // curly quotes (and the apostrophe
                                         // Unicode-correct metadata uses)
        0x2020, 0x2021, 0x2022, 0x2026,  // daggers, bullet, ellipsis
        0x2032, 0x2033, 0x2039, 0x203A,  // primes, single guillemets
        0x2190, 0x2192,                  // arrows
    };
    cps.insert(cps.end(), std::begin(kPunct), std::end(kPunct));

    // The UI icons, which are ordinary glyphs in an ordinary face.
    cps.insert(cps.end(), std::begin(kIconCodepoints), std::end(kIconCodepoints));

    // Everything else — CJK, Hangul, Kana — comes from the library itself.
    // Baking the Han block eagerly (20,000+ codepoints) is out of proportion
    // to what a music library's metadata ever contains.
    //
    // displayName and Track::album are scanned as well as name: `name` is the
    // raw FOLDER key, which in a downloader-managed library is an opaque hash
    // and carries none of the script the record is actually titled in.
    {
        std::lock_guard<std::mutex> lk(albumsMu_);
        auto scan = [&](const std::string& s) {
            for (size_t i = 0; i < s.size(); ) {
                uint32_t cp = utf8::nextCodepoint(s, i);
                if (cp >= 0x100) cps.push_back(cp);
            }
        };
        for (auto& a : albums_) {
            scan(a.name);
            scan(a.displayName);
            scan(a.artist);
            for (auto& t : a.tracks) {
                scan(t.title);
                scan(t.artist);
                scan(t.albumArtist);
                scan(t.album);
            }
        }
    }

    // ── At what sizes ───────────────────────────────────────────────────────
    //
    // The type roles, and nothing else. caption and secondary share a size by
    // design (they are separated by the colour ladder and by style), so this
    // is four distinct values, not five. Every other size the app happens to
    // ask for — icon boxes derived from layout geometry, the art window's own
    // scale — arrives through the miss path instead of being guessed at here.
    const UiTextSizes& t = metrics_.text;
    const std::vector<int> sizes = {
        (int)(t.caption   + 0.5f), (int)(t.secondary + 0.5f),
        (int)(t.body      + 0.5f), (int)(t.title     + 0.5f),
        (int)(t.header    + 0.5f),
    };

    // A resize changes every role size, and cells are keyed by size — so
    // without this the old set stays baked forever and each resize adds
    // another. Start the sheet over instead; the sizes that are actually being
    // drawn are re-baked immediately below, and anything else comes back
    // through the miss path the first time it is asked for.
    if (sizes != glyphSizes_) {
        if (!glyphSizes_.empty()) msdfFont_.reset();
        glyphSizes_ = sizes;
    }

    // Deduplicate: the library scan above yields the same codepoint once per
    // track that uses it. Every duplicate costs a hash probe per style per
    // size, and a Latin letter appears in essentially every title.
    std::sort(cps.begin(), cps.end());
    cps.erase(std::unique(cps.begin(), cps.end()), cps.end());

    if (msdfFont_.ensureGlyphs(cps, sizes) > 0 || !renderer_->msdfReady())
        renderer_->initMsdf(msdfFont_);

    runGlyphBaker();
}

// Rasterize whatever the cache placed but has not yet drawn, in compute,
// straight into the atlas image.
//
// Ordering is load-bearing: initMsdf() must have created (or recreated) the
// image first, because the baker writes INTO it. And when the atlas grows a
// page the image is a new one and therefore empty — every cell baked into the
// old handle is gone — so a changed handle means bake everything again rather
// than only what is new. That is why RasterFont keeps its outlines.
void PlayerWindow::runGlyphBaker() {
    if (!renderer_ || !glyphBaker_.ready() || !msdfFont_.atlasOnGpu()) return;
    const VkImage atlas = renderer_->msdfAtlasImage();
    if (atlas == VK_NULL_HANDLE) return;

    // A GENERATION, not the handle. Vulkan recycles VkImage handles, so
    // destroying the 2-page atlas and creating the 3-page one gave back the
    // same value — the check said "same image", the re-bake was skipped, and
    // every glyph baked into the old one was gone. That read on screen as the
    // whole UI losing its text while the album art stayed perfect.
    const uint32_t gen = renderer_->msdfAtlasGeneration();
    if (gen != bakedAtlasGen_) {
        msdfFont_.setGpuBakedCount(0);
        bakedAtlasGen_ = gen;
    }
    const uint32_t layers = renderer_->msdfAtlasLayers();

    uint32_t sp = 0, sx = 0, sy = 0, sn = 0;
    if (msdfFont_.takeSolidCell(sp, sx, sy, sn)) {
        std::vector<uint8_t> opaque((size_t)sn * sn, 0xFF);
        glyphBaker_.blit(opaque.data(), sn, sn, sp, sx, sy, atlas, layers);
    }

    const auto& cells = msdfFont_.gpuCells();
    const size_t from = msdfFont_.gpuBakedCount();
    if (from >= cells.size()) return;

    std::vector<GlyphBaker::Job> jobs;
    jobs.reserve(cells.size() - from);
    for (size_t i = from; i < cells.size(); ++i)
        jobs.push_back(GlyphBaker::Job{&cells[i].glyph, cells[i].page,
                                       cells[i].x, cells[i].y});
    glyphBaker_.bake(jobs, atlas, layers);
    msdfFont_.setGpuBakedCount(cells.size());
}

// Drain whatever the last frame asked for and did not have. See
// RasterFont::hasMisses(): a per-size cache cannot know every size in advance,
// so it learns them from what is actually drawn. Costs one frame of a missing
// glyph the first time a size appears, and nothing afterwards.
void PlayerWindow::bakeGlyphMisses() {
    if (!renderer_ || !msdfFont_.hasMisses()) return;
    if (msdfFont_.bakeMisses() > 0) {
        renderer_->initMsdf(msdfFont_);
        runGlyphBaker();
        markDirty();          // redraw now that the glyphs exist
    }
}

void PlayerWindow::markDirty() {
    // Keep rendering for one frame per swapchain image (+1 for the frame
    // currently on screen) so a state change reaches every image in the
    // swapchain's rotation instead of leaving some presenting stale content.
    // Guard against calls that land before renderer_ exists: a resize can
    // fire synchronously during window creation (before create() constructs
    // renderer_ a few lines later), and that resize handler calls invalidate().
    pendingFrames_ = renderer_ ? renderer_->swapchainImageCount() + 1 : 1;
}

void PlayerWindow::invalidate() {
    host_->invalidate();
    markDirty();
}

void PlayerWindow::onHostResized() {
    if (renderer_) renderer_->notifyResized();
    recalcLayout();
    invalidate();
}

void PlayerWindow::onHostLayoutInvalidated() {
    recalcLayout();
    invalidate();
}

void PlayerWindow::onHostExposed() {
    markDirty();
}

void PlayerWindow::run() {
    // Dirty-flag render-on-demand: only draw while pendingFrames_ (armed by
    // markDirty()/invalidate()) is nonzero; otherwise host_->pump() blocks
    // instead of busy-spinning, so the app drops to ~0% CPU whenever nothing
    // on screen actually needs to change. Timers and async completions wake
    // the wait normally and mark dirty from their own handlers.
    while (running_) {
        bool haveWork = pendingFrames_ > 0 || artWin_.hasPendingFrames();
        host_->pump(haveWork);
        if (host_->quitRequested()) { running_ = false; break; }

        if (pendingFrames_ > 0) { drawFrame(); pendingFrames_--; }
        // ArtWindow is a second window on this same thread with its own
        // Renderer — no second message pump, so drive its frame here too.
        if (artWin_.isVisible()) artWin_.renderIfDirty();

        // Hold the display awake exactly while the fullscreen art is up.
        // Synced here rather than at each show()/hide() call site because the
        // window also closes itself (Escape inside ArtWindow) — a release
        // missed that way would pin the screen on for the rest of the session.
        bool wantAwake = artWin_.isVisible();
        if (wantAwake != keepAwake_) {
            keepAwake_ = wantAwake;
            host_->setKeepAwake(wantAwake);
        }
    }
}

// UI icons are glyphs in the shared MTSDF atlas (see ui_icons.hh) — real
// Inkscape-authored curves, tinted at draw time, riding the text pass that
// already runs every frame.
//
// Everything below this line is the FALLBACK for when those glyphs aren't
// available (icon font missing, or an atlas too full to take them): the
// original native vector construction on a 36-unit grid, matching the SVG
// viewBox coordinates the artwork is still authored in. Keeping it means a
// missing font degrades to the old look rather than to blank buttons.
static void drawUiIconPrimitive(Canvas& c, const LayoutRect& rc, UiIcon icon, Color col) {
    Rect r = toRect(rc);
    float s = std::min(r.w, r.h);
    float ox = r.x + (r.w - s) * 0.5f, oy = r.y + (r.h - s) * 0.5f;
    auto X = [&](float u) { return ox + u / 36.0f * s; };
    auto Y = [&](float v) { return oy + v / 36.0f * s; };
    switch (icon) {
    case UiIcon::Play:
        c.triangle(X(13), Y(7), X(13), Y(29), X(28), Y(18), col);
        break;
    case UiIcon::Stop:
        c.rect(X(10), Y(10), s * 16 / 36, s * 16 / 36, col, s * 2 / 36);
        break;
    case UiIcon::Prev:
        c.rect(X(6), Y(10), s * 3 / 36, s * 16 / 36, col, s * 1 / 36);
        c.triangle(X(27), Y(8), X(14), Y(18), X(27), Y(28), col);
        break;
    case UiIcon::Next:
        c.triangle(X(9), Y(8), X(22), Y(18), X(9), Y(28), col);
        c.rect(X(27), Y(10), s * 3 / 36, s * 16 / 36, col, s * 1 / 36);
        break;
    case UiIcon::Settings: {
        // 5-tooth gear: circular hub + 5 teeth at 72° increments, using the
        // same rotation transform vk_canvas already exposes — no new
        // primitive needed.
        float cx = X(18), cy = Y(18);
        float hubR = s * 10.0f / 36.0f;
        c.rect(cx - hubR, cy - hubR, hubR * 2, hubR * 2, col, hubR);
        float toothW = s * 7.0f / 36.0f, toothH = s * 9.0f / 36.0f;
        for (int i = 0; i < 5; i++) {
            float angle = i * (2.0f * 3.14159265f / 5.0f);
            c.setRotation(angle, cx, cy);
            c.rect(cx - toothW * 0.5f, cy - hubR - toothH * 0.55f,
                   toothW, toothH, col, toothW * 0.3f);
            c.clearRotation();
        }
        break;
    }
    case UiIcon::Warning:
        // drawUiIcon() routes this to drawWarningIconPrimitive() before ever
        // reaching here; listed so the switch stays exhaustive (-Wswitch).
        break;
    case UiIcon::Quality:
        // Deliberately draws NOTHING, and is listed rather than omitted so the
        // switch stays exhaustive (this case was simply missing, which is the
        // -Wswitch warning that rode along in every build).
        //
        // The quality mark is a hollow spark whose meaning is its draw-time
        // COLOUR, and this fallback path cannot cut holes — every primitive
        // lands in one pass in one colour, the same limitation
        // drawWarningIconPrimitive() documents. A solid full-chroma star is
        // exactly what quality.svg was hollowed to avoid: against a palette of
        // near-black and greys it shouts, and it would shout on every track of
        // a CD-quality library. Absent metadata degrades better than loud
        // wrong metadata, which is not true of the warning banner — hence the
        // different choice there.
        break;
    }
}

// Same 36-unit-grid fallback construction, for the bitperfect warning
// banner's triangle-with-exclamation-mark.
//
// KNOWN LIMITATION of this fallback: the "!" bar and dot are filled in the
// SAME colour as the triangle and land in the same pass, so they composite
// invisibly — this path can only ever draw a solid triangle. The glyph path
// above cuts the "!" out as real holes, which is what the icon was always
// meant to be. Not worth fixing here: it is the degraded path.
static void drawWarningIconPrimitive(Canvas& c, const LayoutRect& rc, Color col) {
    Rect r = toRect(rc);
    float s = std::min(r.w, r.h);
    float ox = r.x + (r.w - s) * 0.5f, oy = r.y + (r.h - s) * 0.5f;
    auto X = [&](float u) { return ox + u / 36.0f * s; };
    auto Y = [&](float v) { return oy + v / 36.0f * s; };
    c.triangle(X(18), Y(4), X(4), Y(32), X(32), Y(32), col);
    c.rect(X(16), Y(13), s * 4 / 36, s * 12 / 36, col);   // "!" bar
    c.rect(X(16), Y(28), s * 4 / 36, s * 4 / 36, col);    // "!" dot
}

// The entry points the drawing code actually calls: atlas glyph first, the
// primitive construction only if that glyph isn't baked.
static void drawUiIcon(Canvas& c, const LayoutRect& rc, UiIcon icon, Color col) {
    if (drawUiIconGlyph(c, rc, icon, col)) return;
    if (icon == UiIcon::Warning) drawWarningIconPrimitive(c, rc, col);
    else                         drawUiIconPrimitive(c, rc, icon, col);
}

static void drawWarningIcon(Canvas& c, const LayoutRect& rc, Color col) {
    drawUiIcon(c, rc, UiIcon::Warning, col);
}

// Album art or the standard placeholder tile — grid tiles, track panel
// header, and transport thumb all share this instead of open-coding the
// texture-valid check.
static void drawArtOrPlaceholder(Canvas& c, TextureHandle tex,
                                 float x, float y, float w, float h) {
    if (tex != kInvalidTexture) c.imageFg(tex, x, y, w, h);
    else c.rect(x, y, w, h, toColor(CLR_TILE_PLACEHOLDER));
}

// The artist/modifier line's advance derives from the title size —
// hardcoded pixel advances went stale the moment the type scale changed
// (lines overlapped).
static float titleArtistAdvance(float titleSize) { return titleSize * 1.35f; }

// Base-name-priority single-line draw (see splitNameModifier): the base name
// is ellipsized only against the full maxW; the modifier renders dim italic
// after it only when the base fit whole, and it alone absorbs any further
// truncation. Track rows and the transport now-playing title share this.
static void drawNameWithModifier(Canvas& c, const std::string& name,
                                 float x, float y, float maxW, float size,
                                 ColorRef baseColor, FontStyle baseStyle) {
    std::string base, mod;
    splitNameModifier(name, base, mod);
    std::string t = truncateToWidth(c, base, maxW, size, baseStyle);
    c.textStyled(t, x, y, size, toColor(baseColor), baseStyle);
    if (mod.empty() || t != base) return;
    float pad = size * 0.6f;
    float used = c.textWidthStyled(base, size, baseStyle) + pad;
    float remain = maxW - used;
    if (remain < size * 1.5f) return;  // no room for anything legible
    std::string m = truncateToWidth(c, mod, remain, size, FontStyle::Italic);
    c.textStyled(m, x + used, y, size, toColor(CLR_TEXT_DIM), FontStyle::Italic);
}

// ── Album-view sidecar content (description / bio files) ────────────────────

namespace fsys = std::filesystem;

static std::string readWholeFile(const fsys::path& p, size_t maxBytes = 64 * 1024) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (s.size() > maxBytes) s.resize(maxBytes);
    return s;
}

// First text sidecar in `dir` whose (lowercased) name contains one of the
// given needles — e.g. {"bio"} finds "Artist Bio.html". HTML-looking content
// is flattened to plain text.
static std::string loadSidecarText(const fsys::path& dir,
                                   std::initializer_list<const char*> needles) {
    std::error_code ec;
    for (auto& entry : fsys::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().u8string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext != ".txt" && ext != ".html" && ext != ".htm" && ext != ".md") continue;
        auto stem = entry.path().stem().u8string();
        std::transform(stem.begin(), stem.end(), stem.begin(), ::tolower);
        for (const char* n : needles) {
            if (stem.find(n) == std::string::npos) continue;
            std::string raw = readWholeFile(entry.path());
            if (raw.empty()) return {};
            bool looksHtml = (ext == ".html" || ext == ".htm" ||
                              raw.find('<') != std::string::npos);
            return looksHtml ? stripHtmlToPlain(raw) : raw;  // text_util.hh
        }
    }
    return {};
}

// "44.1kHz / 16-bit" — the transport readout of the playing track's format.
static std::string formatQualityText(int sampleRate, int bitDepth) {
    if (sampleRate <= 0 || bitDepth <= 0) return {};
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1fkHz / %d-bit", sampleRate / 1000.0, bitDepth);
    return buf;
}

// "24/96" — compact badge for the track panel header, derived from real
// track metadata (not the folder-name suffix).
static std::string formatQualityBadge(int sampleRate, int bitDepth) {
    if (sampleRate <= 0 || bitDepth <= 0) return {};
    char buf[32];
    double khz = sampleRate / 1000.0;
    if (khz == (int)khz) snprintf(buf, sizeof(buf), "%d/%d", bitDepth, (int)khz);
    else                 snprintf(buf, sizeof(buf), "%d/%.1f", bitDepth, khz);
    return buf;
}

void PlayerWindow::drawFrame() {
    // Bake what the LAST frame asked the glyph cache for and did not have —
    // icon boxes and the art window draw at sizes the type roles do not
    // enumerate. One frame late is the contract; see RasterFont::hasMisses().
    //
    // This runs BEFORE any quad is built, and that ordering is load-bearing.
    // It used to run at the end, immediately before renderer_->draw(): a bake
    // there grows the atlas and re-uploads a taller image, while every quad
    // already in msdfQuads_ had its V normalised against the OLD height — so
    // the entire frame sampled the wrong rows of the sheet. It also put a
    // vkDeviceWaitIdle and a vkQueueWaitIdle between scene-building and
    // submit, which is the worst possible place for them.
    bakeGlyphMisses();

    frameCurves_.clear();
    frameShapes_.clear();
    frameImages_.clear();
    frameImagesFg_.clear();
    msdfQuads_.clear();
    Canvas canvas(frameCurves_, renderer_->width(), renderer_->height(),
                  &uiFont_, 0.0f, 0.0f, 0.0f, 0.0f);
    canvas.useImages(&frameImages_);
    canvas.useImagesFg(&frameImagesFg_);
    // SDF shape fast path: all rect/segment/triangle draws become per-shape
    // quads (Renderer's shape pipeline) — the compute rasterizer never runs
    // and its ~17 MB of screen-size buffers are never even allocated.
    canvas.useShapes(&frameShapes_);
    if (msdfFont_.valid())
        canvas.useMsdf(&msdfFont_, &msdfQuads_);
    // No canvas.clear(): it draws an opaque full-screen vector rect rather
    // than a true framebuffer clear — the render pass already clears to
    // black, and a screen-wide clear here would hide album art drawn as a
    // background image layer. Each panel below fills only its own rect.

    if (uiMode_ == UiMode::Essential) {
        canvas.rect(0, 0, canvas.w(), canvas.h(), toColor(CLR_BG_MAIN));

        Rect artR = toRect(rcEssentialArt_);
        if (transportArtTex_ != kInvalidTexture)
            canvas.imageFg(transportArtTex_, artR.x, artR.y, artR.w, artR.h);
        else
            canvas.rect(artR.x, artR.y, artR.w, artR.h, toColor(CLR_TILE_PLACEHOLDER));

        Rect titleR = toRect(rcEssentialTitle_);
        std::string titleStr = currentTitle_.empty() ? "No track" : currentTitle_;
        float titleW = canvas.textWidthStyled(titleStr, metrics_.text.title, FontStyle::Bold);
        canvas.textStyled(titleStr, titleR.x + std::max(0.0f, (titleR.w - titleW) * 0.5f), titleR.y,
                          metrics_.text.title, toColor(CLR_TEXT_PRIMARY), FontStyle::Bold);

        // Single combined Play/Stop button (per design: not a separate
        // resume-vs-restart-from-zero distinction in Essential mode).
        struct EBtn { LayoutRect rc; int idx; UiIcon icon; ColorRef clr; };
        EBtn ebuttons[] = {
            { rcEssentialPrev_,     0, UiIcon::Prev, CLR_TEXT_PRIMARY },
            { rcEssentialPlayStop_, 1, isPlaying_ ? UiIcon::Stop : UiIcon::Play,
                                       isPlaying_ ? CLR_TEXT_PRIMARY : CLR_ACCENT },
            { rcEssentialNext_,     2, UiIcon::Next, CLR_TEXT_PRIMARY },
        };
        for (auto& b : ebuttons) {
            if (hoverEssentialBtn_ == b.idx) {
                Rect r = toRect(b.rc);
                canvas.rect(r.x, r.y, r.w, r.h, toColor(CLR_HOVER), UI_CORNER_RADIUS);
            }
            drawUiIcon(canvas, b.rc, b.icon, toColor(b.clr));
        }

        renderer_->draw(frameCurves_, /*overlay_rotation_deg=*/0, frameImages_, frameImagesFg_, msdfQuads_, frameShapes_);
        return;
    }

    // ── Sidebar ──────────────────────────────────────────────────────────
    {
        Rect sb = toRect(rcSidebar_);
        canvas.rect(sb.x, sb.y, sb.w, sb.h, toColor(CLR_BG_SIDEBAR));
        canvas.rect(sb.x + sb.w - metrics_.stroke(1.0f), sb.y, metrics_.stroke(1.0f), sb.h,
                    toColor(CLR_SEPARATOR));

        // Brand must stay inside the sidebar — it once painted over the
        // first grid column's art because the sidebar width didn't scale
        // with the text (see recalcLayout()). The truncate is a backstop.
        canvas.textStyled(truncateToWidth(canvas, "MATRIX PLAYER",
                                          sb.w - metrics_.space(SP_LG),
                                          metrics_.text.body, FontStyle::Bold),
                          metrics_.space(16.0f),
                          rcBrand_.bottom * 0.5f - metrics_.text.body * 0.5f,
                          metrics_.text.body, toColor(CLR_ACCENT), FontStyle::Bold);

        // Search box — filters the album grid live as the user types.
        drawSearchField(canvas, rcSearch_, searchQuery_, searchFocused_, "Search",
                        metrics_.text.secondary);

        struct NavItem { const char* label; LayoutRect rc; AlbumTypeFilter filter; };
        NavItem items[] = {
            // Listed in the order they are DRAWN — each carries its own rect,
            // so this array is documentation of the reading order, not what
            // creates it. recalcLayout() is where the rows are placed.
            { "Albums",  rcNavAlbum_,  AlbumTypeFilter::Album  },
            { "EPs",     rcNavEp_,     AlbumTypeFilter::Ep     },
            { "Singles", rcNavSingle_, AlbumTypeFilter::Single },
            { "Compilations", rcNavCompilation_, AlbumTypeFilter::Compilation },
            { "Live",    rcNavLive_,   AlbumTypeFilter::Live   },
            { "Remixes", rcNavRemix_,  AlbumTypeFilter::Remix  },
        };
        // Seven content rows, one rule: a row is active when its section is the
        // one showing and Settings is not covering it.
        const bool playlistsOpen = (!settingsOpen_ && navSection_ == NavSection::Playlists);
        for (auto& item : items) {
            bool active = (!settingsOpen_ && navSection_ == NavSection::Albums &&
                           albumTypeFilter_ == item.filter);
            bool hovered = (hoverSidebarItem_ == (int)item.filter && !active);
            Rect r = toRect(item.rc);
            const float pillX = r.x + metrics_.space(4.0f);
            const float pillW = r.w - metrics_.space(8.0f);
            if (active) {
                // Selected: accent-tint fill + left bar, full height + square —
                // matches the hover highlight exactly (one selection family).
                canvas.rect(pillX, r.y, pillW, r.h,
                            toColor(CLR_ACCENT, UI_SELECT_TINT_ALPHA), UI_CORNER_RADIUS);
                canvas.rect(pillX, r.y, metrics_.stroke(3.0f), r.h,
                            toColor(CLR_ACCENT), UI_CORNER_RADIUS);
            } else if (hovered) {
                canvas.rect(pillX, r.y, pillW, r.h, toColor(CLR_HOVER), UI_CORNER_RADIUS);
            }
            canvas.text(item.label, r.x + metrics_.space(20.0f),
                       r.y + r.h * 0.5f - metrics_.text.body * 0.5f,
                       metrics_.text.body, toColor(active ? CLR_ACCENT : CLR_TEXT_SECONDARY));
        }

        // Playlists — the fifth content row, still above the hairline, and by
        // now the same KIND of thing as the four above it: a section whose
        // tiles fill the content area (see drawPlaylistSection). It is one row
        // rather than three because two of the three say nothing on a fresh
        // install, and a grid of three tiles absorbs that where dead sidebar
        // rows would not.
        {
            bool active  = playlistsOpen;
            bool hovered = (hoverSidebarItem_ == kSidebarPlaylistsHit && !active);
            Rect r = toRect(rcNavPlaylists_);
            const float pillX = r.x + metrics_.space(4.0f);
            const float pillW = r.w - metrics_.space(8.0f);
            if (active) {
                canvas.rect(pillX, r.y, pillW, r.h,
                            toColor(CLR_ACCENT, UI_SELECT_TINT_ALPHA), UI_CORNER_RADIUS);
                canvas.rect(pillX, r.y, metrics_.stroke(3.0f), r.h,
                            toColor(CLR_ACCENT), UI_CORNER_RADIUS);
            } else if (hovered) {
                canvas.rect(pillX, r.y, pillW, r.h, toColor(CLR_HOVER), UI_CORNER_RADIUS);
            }
            canvas.text("Playlists", r.x + metrics_.space(20.0f),
                        r.y + r.h * 0.5f - metrics_.text.body * 0.5f,
                        metrics_.text.body,
                        toColor(active ? CLR_ACCENT : CLR_TEXT_SECONDARY));
        }

        // Settings — spatially separated below a hairline, never mixed into
        // the content-type list above (the user's explicit ask: a music
        // player should read as albums-and-music first, configuration
        // second). A WORD, not the gear glyph: the four rows above it are
        // words, and a lone icon among them read as a different kind of
        // control than it is. The gear survives in the icon set (it is still
        // baked and still exercised by icon_preview/ui_icons_test) — it just
        // has no draw site in the app.
        canvas.rect((float)rcNavSettings_.left, rcNavSettings_.top - metrics_.space(7.0f),
                    sb.w, metrics_.stroke(1.0f), toColor(CLR_SEPARATOR));
        {
            const bool settingsActive = settingsOpen_;
            bool hovered = (hoverSidebarItem_ == kSidebarSettingsHit && !settingsActive);
            Rect r = toRect(rcNavSettings_);
            const float settPillX = r.x + metrics_.space(4.0f);
            const float settPillW = r.w - metrics_.space(8.0f);
            if (settingsActive) {
                canvas.rect(settPillX, r.y, settPillW, r.h,
                            toColor(CLR_ACCENT, UI_SELECT_TINT_ALPHA), UI_CORNER_RADIUS);
                canvas.rect(settPillX, r.y, metrics_.stroke(3.0f), r.h,
                            toColor(CLR_ACCENT), UI_CORNER_RADIUS);
            } else if (hovered) {
                canvas.rect(settPillX, r.y, settPillW, r.h, toColor(CLR_HOVER), UI_CORNER_RADIUS);
            }
            // Same inset, size and active-color rule as the nav rows above —
            // five rows, one family.
            canvas.text("Settings", r.x + metrics_.space(20.0f),
                        r.y + r.h * 0.5f - metrics_.text.body * 0.5f,
                        metrics_.text.body,
                        toColor(settingsActive ? CLR_ACCENT : CLR_TEXT_SECONDARY));
        }

        // (The now-playing mini card that used to fill the space below the
        // nav items was removed: it duplicated the transport bar's art,
        // title, and artist — the transport bar is the single now-playing
        // readout now, including the format line next to the BITPERFECT badge.)

        // The space it left is where the headphone switcher lives, anchored to
        // the BOTTOM of the sidebar so it never pushes the nav around.
        drawHeadphoneBlock(canvas, rcSidebar_);
    }

    // ── Main content: album grid, settings page, or (below) the full-page
    // album view that replaces the grid while an album is focused ─────────
    // ── The chip strip: what the query currently says ──────────────────────
    // Drawn above the grid because that is where the width is, and because a
    // filter belongs beside the results it produced. Clicking a chip removes
    // it — the whole query is one row of things you can take back.
    chipRects_.clear();
    suggestRects_.clear();
    if (rcChips_.right > rcChips_.left) {
        Rect cs = toRect(rcChips_);
        canvas.rect(cs.x, cs.y, cs.w, cs.h, toColor(CLR_BG_MAIN));
        canvas.rect(cs.x, cs.y + cs.h - metrics_.stroke(1.0f), cs.w,
                    metrics_.stroke(1.0f), toColor(CLR_SEPARATOR));

        const float sz    = metrics_.text.secondary;
        const float padX  = metrics_.space(10.0f);
        const float gap   = metrics_.space(8.0f);
        const float chipH = metrics_.space(34.0f);
        const float rowH  = metrics_.space(46.0f);
        float x = cs.x + metrics_.space(20.0f);
        float y = cs.y + (rowH - chipH) * 0.5f;

        for (size_t i = 0; i < searchChips_.size(); i++) {
            // The connective is DERIVED (facets::sameGroup) and merely shown:
            // OR between alternatives, AND between different questions. It is
            // drawn in CLR_TEXT_DIM and upper case — never CLR_ACCENT, which
            // §4 reserves for state — and that contrast is also what tells it
            // apart from a record actually CALLED "AND": a title lives inside
            // a chip, in ordinary text, while the connective sits outside and
            // dimmed.
            if (i > 0) {
                const char* conn = facets::sameGroup(searchChips_[i - 1], searchChips_[i])
                                       ? "OR" : "AND";
                float cw = canvas.textWidthStyled(conn, sz, FontStyle::Roman);
                canvas.textStyled(conn, x, y + (chipH - sz) * 0.5f, sz,
                                  toColor(CLR_TEXT_DIM), FontStyle::Roman);
                x += cw + gap;
            }
            const std::string& label = searchChips_[i].value;
            float tw = canvas.textWidthStyled(label, sz, FontStyle::Roman);
            float xw = canvas.textWidthStyled("\xC3\x97", sz, FontStyle::Roman);  // ×
            float w  = padX + tw + gap + xw + padX;

            bool hot = hoverChipIdx_ == (int)i;
            canvas.rect(x, y, w, chipH, toColor(hot ? CLR_HOVER : CLR_INPUT_BG),
                        UI_CORNER_RADIUS);
            canvas.textStyled(label, x + padX, y + (chipH - sz) * 0.5f, sz,
                              toColor(CLR_TEXT_PRIMARY), FontStyle::Roman);
            canvas.textStyled("\xC3\x97", x + padX + tw + gap, y + (chipH - sz) * 0.5f, sz,
                              toColor(hot ? CLR_TEXT_PRIMARY : CLR_TEXT_DIM), FontStyle::Roman);

            chipRects_.push_back({ (int)x, (int)y, (int)(x + w), (int)(y + chipH) });
            x += w + gap;
        }

        // ── Suggestions, on their own row, laid out across the strip ────────
        // A row of choices rather than a vertical dropdown, because this is
        // the space that actually exists: it is wide, it is reserved, and
        // nothing is drawn underneath it to bleed through.
        if (searchFocused_ && !searchSuggest_.empty()) {
            float sx = cs.x + metrics_.space(20.0f);
            float sy = cs.y + (searchChips_.empty() ? 0.0f : rowH)
                     + (rowH - chipH) * 0.5f;

            for (size_t i = 0; i < searchSuggest_.size(); i++) {
                const facets::Suggestion& s = searchSuggest_[i];
                std::string count = std::to_string(s.count);
                float lw = canvas.textWidthStyled(s.label, sz, FontStyle::Roman);
                float cw = canvas.textWidthStyled(count, sz, FontStyle::Math);
                float w  = padX + lw + gap + cw + padX;
                if (sx + w > cs.x + cs.w - metrics_.space(20.0f)) break;  // no wrap: one row

                bool hot = (int)i == hoverSuggestIdx_ || (int)i == searchSuggestSel_;
                // A DISABLED row means "this exists, but not with what you
                // already picked" — the count reads 0 and the row stays put so
                // the listener can see it and learn what is blocking it. A
                // value the library does not hold at all never gets here:
                // facets::suggest() builds its candidates from the albums.
                canvas.rect(sx, sy, w, chipH,
                            toColor(hot && s.enabled ? CLR_HOVER : CLR_INPUT_BG),
                            UI_CORNER_RADIUS);
                canvas.textStyled(s.label, sx + padX, sy + (chipH - sz) * 0.5f, sz,
                                  toColor(s.enabled ? CLR_TEXT_PRIMARY : CLR_TEXT_DIM),
                                  FontStyle::Roman);
                canvas.textStyled(count, sx + padX + lw + gap, sy + (chipH - sz) * 0.5f, sz,
                                  toColor(CLR_TEXT_DIM), FontStyle::Math);

                suggestRects_.push_back({ (int)sx, (int)sy, (int)(sx + w), (int)(sy + chipH) });
                sx += w + gap;
            }
        }
    }

    if (!settingsOpen_ && !trackPanelOpen_) {
        Rect g = toRect(rcGrid_);
        canvas.rect(g.x, g.y, g.w, g.h, toColor(CLR_BG_MAIN));

        // Empty states are centred by MEASURING the string, not by subtracting a
        // guessed half-width (the old `- 160` / `- 120`, each hardcoded for one
        // string at one text size and already visibly off before the type scale
        // changed underneath them).
        auto emptyState = [&](const std::string& msg) {
            float w = canvas.textWidthStyled(msg, metrics_.text.body, FontStyle::Italic);
            canvas.textStyled(msg, g.x + (g.w - w) * 0.5f, g.y + metrics_.space(100.0f),
                              metrics_.text.body, toColor(CLR_TEXT_DIM), FontStyle::Italic);
        };

        // The fifth section draws into exactly this area, at exactly this
        // point — it is a sibling of the album grid, not something laid over
        // it. Everything already drawn (the sidebar) and everything drawn
        // below (the transport bar) is untouched and stays live.
        if (navSection_ == NavSection::Playlists) {
            drawPlaylistSection(canvas, rcGrid_);
        } else if (albums_.empty()) {
            emptyState("No albums yet. Use the gear icon below to add a music folder.");
        } else if (gridIndices_.empty()) {
            std::string msg;
            // A chip set that matched nothing gets the REASON, not a shrug.
            // "No 24-bit in 1990s — your 24-bit releases are from 2002 onward"
            // is the whole point of the guided search: the listener learns
            // where the thing they asked for actually lives instead of facing
            // an empty page and guessing which of their choices was wrong.
            facets::EmptyReason why = searchEmptyReason();
            if (why.empty) {
                msg = why.message;
            } else if (!searchQuery_.empty()) {
                msg = "No matches for \"" + searchQuery_ + "\"";
            } else {
                const char* filterLabel =
                    albumTypeFilter_ == AlbumTypeFilter::Ep     ? "EPs" :
                    albumTypeFilter_ == AlbumTypeFilter::Single ? "Singles" :
                    albumTypeFilter_ == AlbumTypeFilter::Remix  ? "Remixes" :
                    albumTypeFilter_ == AlbumTypeFilter::Compilation ? "Compilations" :
                    albumTypeFilter_ == AlbumTypeFilter::Live   ? "Live records"
                                                                : "Albums";
                msg = std::string("No ") + filterLabel + " yet";
            }
            emptyState(msg);
        } else {
            canvas.setClip(g.x, g.y, g.w, g.h);
            int tileStepX = gridStepX_;   // resolved in recalcLayout()
            int tileStepY = gridTileSize_ + gridRowGap_;
            int firstRow = std::max(0, gridScrollY_ / tileStepY);
            int gridH = rcGrid_.bottom - rcGrid_.top;
            int lastRow = (gridScrollY_ + gridH) / tileStepY + 1;

            for (int row = firstRow; row <= lastRow; row++) {
                for (int col = 0; col < gridCols_; col++) {
                    int tile = row * gridCols_ + col;
                    if (tile >= (int)gridIndices_.size()) break;
                    int idx = gridIndices_[tile];   // real albums_ index
                    const Album& alb = albums_[idx];

                    float x = (float)(rcGrid_.left + gridPadXpx_ + col * tileStepX + (tileStepX - gridArtSize_) / 2);
                    float y = (float)(rcGrid_.top + gridPadYpx_ + row * tileStepY - gridScrollY_);
                    float a = (float)gridArtSize_;

                    bool nowPlaying = isPlaying_ && idx == displayAlbum_;

                    // Hover: neutral grey focus frame (hover is never accent —
                    // accent signals state only). A grey rounded rect slightly
                    // larger than the art reads as a border halo once the art
                    // (drawn above the vector layer) covers its center.
                    if (hoverAlbumIdx_ == idx && !nowPlaying && selectedAlbumIdx_ != idx) {
                        canvas.rect(x - metrics_.space(SP_XS), y - metrics_.space(SP_XS),
                                    a + metrics_.space(12.0f), a + metrics_.space(12.0f),
                                    toColor(CLR_HOVER), UI_CORNER_RADIUS);
                    }
                    // No quality-tier frame here, deliberately. A colour per
                    // tile described nothing worth the noise on a grid that is
                    // already busy with the artwork itself — and it will make
                    // even less sense once one tile stands for a whole set of
                    // quality/edition variants (see TODO.md). Quality is read
                    // in one place now: the per-track mark in the album view.

                    // Now-playing ring: one solid square band hugging the art.
                    // This used to be a three-layer rounded glow (0.20/0.45/1.0
                    // at radii 12/10/8) — the last real exception to "square
                    // throughout" (§1.3). Curves and a soft falloff read as a
                    // different UI's vocabulary next to this one's hard edges,
                    // and the fade bought nothing the solid band doesn't say
                    // outright.
                    //
                    // NOW-PLAYING ONLY — not selection. Selection is the user
                    // saying "show me this"; this band is the app saying "this
                    // is what you are hearing". They were briefly the same
                    // ring at two alphas, and it lied: open any album, press
                    // Escape, and that album wore the playing colour. A state
                    // the user can point at is not a state.
                    //
                    // The four bands are laid out NOT to overlap — horizontals
                    // full width, verticals only the art's own height. Full
                    // height on both would paint each corner twice, and at any
                    // alpha under 1 the second pass composites over the first,
                    // so the corners came out brighter than the edges.
                    if (nowPlaying) {
                        const float d = metrics_.space(3.0f);
                        const Color c = toColor(CLR_ACCENT);
                        canvas.rect(x - d, y - d, a + d * 2, d, c);   // top
                        canvas.rect(x - d, y + a, a + d * 2, d, c);   // bottom
                        canvas.rect(x - d, y,     d,         a, c);   // left
                        canvas.rect(x + a, y,     d,         a, c);   // right
                    }

                    // A REMIX GROUP gets a 2x2 mosaic of its members' covers
                    // instead of the best member's alone. An edition group can
                    // wear one cover honestly — a deluxe is the same record
                    // with more on it — but a remix set is different music by
                    // different hands, so showing only one would misrepresent
                    // what is behind the tile. Albums/EPs/singles keep the
                    // single cover (see drawVariantMosaic).
                    if (!drawVariantMosaic(canvas, idx, x, y, a))
                        drawArtOrPlaceholder(canvas, getGridArtTexture(idx), x, y, a, a);

                    // Last-played / now-playing marker: a thin accent bar
                    // hugging the art's bottom edge, exactly the art's width
                    // (the art itself can't carry a badge: imageFg composites
                    // above the vector layer). Replaces the old offset dot,
                    // which broke the grid's column alignment.
                    // Only last-played needs this bar; now-playing is already
                    // unmistakable from its glow (no double-marking).
                    bool lastPlayed = !nowPlaying &&
                        alb.name == lastPlayedAlbumName_ &&
                        alb.artist == lastPlayedArtistName_;
                    if (lastPlayed)
                        // The +2 is the GAP below the art; the trailing 2 is the
                        // bar's THICKNESS — different classes, same call.
                        canvas.rect(x, y + a + metrics_.space(2.0f), a,
                                    metrics_.stroke(2.0f), toColor(CLR_ACCENT, 0.4f));

                    // Tile text is centered under the art and confined to
                    // exactly the art's width — the grid's vertical edges are
                    // hard walls; nothing (including a truncation ellipsis)
                    // may cross them. A trailing "(Deluxe)"-class modifier
                    // (see splitNameModifier) gets its own dim second line —
                    // "Rush" over "Are You Coming" — so the base name never
                    // loses space to it; without one, the title wraps to two
                    // lines before ellipsis.
                    float textMaxW = a;
                    auto centered = [&](const std::string& s, float yy, float sz,
                                        ColorRef clr, FontStyle st) {
                        float w = canvas.textWidthStyled(s, sz, st);
                        canvas.textStyled(s, x + std::max(0.0f, (a - w) * 0.5f), yy,
                                          sz, toColor(clr), st);
                    };
                    float adv = titleArtistAdvance(metrics_.text.body);
                    float ty = y + a + metrics_.space(16.0f);
                    std::string base, mod;
                    if (splitNameModifier(alb.displayName, base, mod)) {
                        centered(truncateToWidth(canvas, base, textMaxW, metrics_.text.body, FontStyle::Bold),
                                 ty, metrics_.text.body, CLR_TEXT_ALBUM_TITLE, FontStyle::Bold);
                        centered(truncateToWidth(canvas, mod, textMaxW, metrics_.text.secondary, FontStyle::Italic),
                                 ty + adv, metrics_.text.secondary, CLR_TEXT_DIM, FontStyle::Italic);
                    } else {
                        std::string l1, l2;
                        splitTwoLines(canvas, alb.displayName, textMaxW, metrics_.text.body, FontStyle::Bold, l1, l2);
                        centered(l1, ty, metrics_.text.body, CLR_TEXT_ALBUM_TITLE, FontStyle::Bold);
                        if (!l2.empty())
                            centered(l2, ty + adv, metrics_.text.body, CLR_TEXT_ALBUM_TITLE, FontStyle::Bold);
                    }
                    // Artist sits in a fixed slot (below 2 title lines) so it
                    // aligns across tiles whether titles wrapped or not. The
                    // release year rides on that same line rather than taking
                    // a fourth: the tile's height is fixed by the layout, and
                    // a year is short enough that truncation still lands in
                    // the artist name, where an ellipsis is legible.
                    // Untagged (year 0) simply omits it — no "Unknown", which
                    // would be noise on every MP3 in a library nothing reads
                    // ID3 for.
                    std::string byline = alb.artist;
                    if (int yr = facets::albumYear(alb)) {
                        if (!byline.empty()) byline += " · ";
                        byline += std::to_string(yr);
                    }
                    centered(truncateToWidth(canvas, byline, textMaxW, metrics_.text.secondary, FontStyle::Italic),
                             ty + adv * 2, metrics_.text.secondary, CLR_TEXT_SECONDARY, FontStyle::Italic);
                }
            }
            canvas.clearClip();
        }
    } else if (settingsOpen_ && activePanel_ != SettingsPanel::None) {
        // A settings panel (Phase 7) takes over the whole content area,
        // replacing the settings-page row list below until closed.
        drawActivePanel(canvas, rcGrid_);
    } else if (settingsOpen_) {
        Rect g = toRect(rcGrid_);
        canvas.rect(g.x, g.y, g.w, g.h, toColor(CLR_BG_MAIN));

        // Centering is done by measuring the styled text ourselves —
        // Canvas::textCentered() measures with the curve font while the UI
        // renders from the MTSDF atlas, and its baseline convention differs,
        // so labels came out visibly off-center both ways.
        auto centeredIn = [&](const std::string& s, const Rect& r, float sz,
                              ColorRef clr, FontStyle st) {
            float w = canvas.textWidthStyled(s, sz, st);
            canvas.textStyled(s, r.x + std::max(0.0f, (r.w - w) * 0.5f),
                              r.y + r.h * 0.5f - sz * 0.5f, sz, toColor(clr), st);
        };
        {
            Rect hdr = { g.x, g.y + 24, g.w, metrics_.text.header };
            centeredIn("Settings", hdr, metrics_.text.header, CLR_TEXT_PRIMARY, FontStyle::Bold);
        }

        bool bp = bitperfectMode_.load();
        std::string modeLabel = bp
            ? "Mode: Bitperfect - click to switch to Reference EQ"
            : "Mode: Reference EQ - click to switch to Bitperfect";

        struct SettItem { LayoutRect rc; std::string label; int idx; };
        SettItem items[] = {
            { rcSettingsAddFolder_, "Add Music Folder",      0 },
            { rcSettingsManage_,    "Manage Music Folders",  1 },
            { rcSettingsAudio_,     "Audio Output Settings", 2 },
            { rcSettingsEq_,        "EQ / AutoEQ Profiles",  3 },
            { rcSettingsBitperfect_, modeLabel,              4 },
        };
        for (auto& item : items) {
            Rect r = toRect(item.rc);
            bool isActiveModeRow = (item.idx == 4 && bp);
            // Hover fills the box (below the border so the outline stays crisp).
            if (hoverSettingsItem_ == item.idx && !isActiveModeRow)
                canvas.rect(r.x, r.y, r.w, r.h, toColor(CLR_HOVER), UI_CORNER_RADIUS);
            // Outlined box: a full 4-side rectangle border per row. The active
            // bitperfect toggle gets a 2px accent border; the rest a 1px hairline.
            ColorRef border = isActiveModeRow ? CLR_ACCENT : CLR_SEPARATOR;
            float bt = isActiveModeRow ? metrics_.stroke(2.0f) : metrics_.stroke(1.0f);
            canvas.rect(r.x, r.y, r.w, bt, toColor(border));
            canvas.rect(r.x, r.y + r.h - bt, r.w, bt, toColor(border));
            canvas.rect(r.x, r.y, bt, r.h, toColor(border));
            canvas.rect(r.x + r.w - bt, r.y, bt, r.h, toColor(border));
            ColorRef textClr = (item.idx == 3 && bp) ? CLR_TEXT_DIM
                             : isActiveModeRow ? CLR_ACCENT : CLR_TEXT_PRIMARY;
            centeredIn(item.label, r, metrics_.text.body, textClr, FontStyle::Roman);
        }
    }

    // ── Album view (full page — replaces the grid while open) ───────────
    // Clicking an album focuses it: the rest of the library disappears and
    // the whole content area belongs to this one album — big art on the
    // left, track list on the right, album description and artist bio (from
    // the sidecar files next to the music) below. The page scrolls as one.
    if (!settingsOpen_ && trackPanelOpen_) {
        Rect tp = toRect(rcTrackPanel_);
        canvas.rect(tp.x, tp.y, tp.w, tp.h, toColor(CLR_BG_TRACKPANEL));

        if (selectedAlbumIdx_ >= 0 && selectedAlbumIdx_ < (int)albums_.size()) {
            const Album& album = albums_[selectedAlbumIdx_];
            canvas.setClip(tp.x, tp.y, tp.w, tp.h);

            float pad = metrics_.space(SP_XL);
            float scroll = (float)trackScrollY_;
            float artSize = std::min(tp.w * 0.32f, tp.h * 0.55f);
            float artX = tp.x + pad;
            float artY = tp.y + pad + metrics_.space(16.0f) - scroll;
            // The art scrolls with the page. imageFg isn't clipped by
            // setClip, but the art sits at the top of the content, so
            // scrolling only ever moves it up off the window — never down
            // over the transport bar.
            drawArtOrPlaceholder(canvas, trackPanelArtTex_, artX, artY, artSize, artSize);

            // Right column: title block + track list.
            float colX = artX + artSize + metrics_.space(SP_XL);
            // Capped, not "whatever is left". A reading measure must scale with
            // type (through space()), but a cap that scales at the same rate
            // as the content binds nothing — the old 1180 always resolved above
            // the uncapped width. 820 sits under the ~925 the layout produces
            // unbounded, so it actually constrains the pairing distance.
            float colW = std::min(tp.x + tp.w - pad - colX, metrics_.space(820.0f));
            float y = artY + metrics_.space(4.0f);

            std::string base, mod;
            splitNameModifier(album.displayName, base, mod);
            // The album name wraps over as many lines as it needs — this
            // page is the one place the full name must always be readable,
            // never ellipsized ("F_CK U SKRILLEX you think ur andy warhol
            // but ur not!!" class titles).
            {
                std::vector<std::string> titleLines;
                wrapText(canvas, base, colW, metrics_.text.title, FontStyle::Bold, titleLines);
                for (auto& ln : titleLines) {
                    canvas.textStyled(ln, colX, y, metrics_.text.title,
                                      toColor(CLR_TEXT_PRIMARY), FontStyle::Bold);
                    y += titleArtistAdvance(metrics_.text.title);
                }
            }
            if (!mod.empty()) {
                std::vector<std::string> modLines;
                wrapText(canvas, mod, colW, metrics_.text.secondary, FontStyle::Italic, modLines);
                for (auto& ln : modLines) {
                    canvas.textStyled(ln, colX, y, metrics_.text.secondary,
                                      toColor(CLR_TEXT_DIM), FontStyle::Italic);
                    y += metrics_.text.secondary * 1.35f;
                }
            }
            if (!album.artist.empty()) {
                canvas.textStyled(truncateToWidth(canvas, album.artist, colW, metrics_.text.secondary, FontStyle::Italic),
                                  colX, y, metrics_.text.secondary, toColor(CLR_TEXT_SECONDARY), FontStyle::Italic);
                y += metrics_.text.secondary * 1.35f;
            }
            // Quality badge from the album's actual track metadata (max
            // rate/depth across tracks), not the folder-name suffix.
            int maxRate = 0, maxBit = 0;
            for (auto& t : album.tracks) {
                maxRate = std::max(maxRate, t.sampleRate);
                maxBit  = std::max(maxBit,  t.bitDepth);
            }
            // The release year leads that same line. This page is where a
            // record is actually read, and until now the year was stored and
            // never shown anywhere in the app — which made "what era is this"
            // unanswerable at a glance and searching by decade meaningless.
            std::string badge = formatQualityBadge(maxRate, maxBit);
            if (int yr = facets::albumYear(album)) {
                std::string y0 = std::to_string(yr);
                badge = badge.empty() ? y0 : y0 + " · " + badge;
            }
            if (!badge.empty()) {
                canvas.textStyled(badge, colX, y, metrics_.text.caption,
                                  toColor(CLR_TEXT_DIM), FontStyle::Math);
                y += metrics_.text.caption * 1.8f;
            }
            y += metrics_.space(SP_XS);
            canvas.rect(colX, y, colW, metrics_.stroke(1.0f), toColor(CLR_SEPARATOR));
            y += metrics_.space(12.0f);

            // ── One rectangle governs the row ────────────────────────────
            // Every layer of a row — the hover/playing fill, the now-playing
            // bar, the columns — derives from rowX/rowW, so nothing can drift
            // out of alignment with anything else.
            const float rowOverhang = metrics_.space(7.0f);
            const float rowX = colX - rowOverhang;
            const float rowW = colW + rowOverhang * 2.0f;
            const float durRight = rowX + rowW - metrics_.space(10.0f);
            // Number column (right-aligned) and title column, hoisted out of
            // the row loop because the disc separators are measured against
            // the same box.
            const float numColW = metrics_.space(49.0f), titleX = metrics_.space(75.0f);
            // The quality mark sits in a column of its own, immediately left
            // of the duration. It is anchored to the duration's RESERVED width
            // (durColW below, measured once on "88:88") rather than to each
            // stamp's actual width, or it would shuffle between "3:52" and
            // "10:05" instead of forming a column.
            const float markSize = metrics_.text.caption;

            // Hit-test anchors for trackPanelHitTest(). Left/right match the
            // row box that's actually drawn, not the narrower text column —
            // clicking the overhang used to miss. (Row tops go into
            // trackRowTop_ below, once disc separators are known.)
            trackListLeft_  = (int)rowX;
            trackListRight_ = (int)(rowX + rowW);

            // Duration column width measured once (widest realistic stamp),
            // so titles reserve real space instead of a guessed constant.
            float durColW = canvas.textWidthStyled("88:88", metrics_.text.secondary, FontStyle::Math);
            const float markX = durRight - durColW - metrics_.space(SP_SM) - markSize;

            // Disc grouping: a "DISC n" separator only appears when the album
            // actually spans more than one tagged disc. Files with no
            // DISCNUMBER carry 0 (see core/library.h) — that's every file of a
            // single-disc release, so those lists lay out exactly as before.
            bool multiDisc = false;
            {
                int firstDisc = 0;
                for (auto& t : album.tracks) {
                    if (t.discNumber <= 0) continue;
                    if (firstDisc == 0) firstDisc = t.discNumber;
                    else if (t.discNumber != firstDisc) { multiDisc = true; break; }
                }
            }
            // Taller than the label needs, so the separator carries its own
            // breathing room rather than crowding the rows around it. Its
            // rules stop short of the row box on both sides.
            const float discHeaderH = multiDisc ? metrics_.space(SP_LG + SP_SM) : 0.0f;
            const float discRuleL   = rowX + metrics_.space(SP_MD);
            const float discRuleR   = rowX + rowW - metrics_.space(SP_MD);

            // Rows no longer sit on a fixed i*rowHeight grid — a disc header
            // pushes everything below it down — so their scroll-0 tops are
            // recorded here for trackPanelHitTest() to read back. Indices stay
            // indices into album.tracks; headers never consume one.
            trackRowTop_.assign(album.tracks.size(), 0);

            float rowY = y;
            int   headedDisc = -1;   // disc whose separator has already been emitted
            for (int i = 0; i < (int)album.tracks.size(); i++) {
                const Track& tr = album.tracks[i];

                if (multiDisc && tr.discNumber != headedDisc) {
                    headedDisc = tr.discNumber;
                    // Culling is draw-only: rowY must keep accumulating even
                    // off-screen or every row below would be misplaced.
                    if (rowY + discHeaderH >= tp.y && rowY <= tp.y + tp.h) {
                        char lbl[24];
                        snprintf(lbl, sizeof(lbl), "DISC %d", headedDisc);
                        float lblW  = canvas.textWidthStyled(lbl, metrics_.text.caption, FontStyle::Bold);
                        float midY  = rowY + discHeaderH * 0.5f;
                        // Centered label with a rule reaching out to either
                        // side: the separator reads as one symmetric object
                        // rather than as a left-anchored heading.
                        float labelX = (discRuleL + discRuleR - lblW) * 0.5f;
                        canvas.textStyled(lbl, labelX, midY - metrics_.text.caption * 0.5f,
                                          metrics_.text.caption, toColor(CLR_TEXT_DIM), FontStyle::Bold);
                        float gap   = metrics_.space(SP_MD);
                        float rule  = metrics_.stroke(1.0f);
                        float ruleY = midY - rule * 0.5f;
                        // Each side is drawn only if it still has width — a
                        // long label in a narrow window must not produce a
                        // negative-width rect.
                        if (labelX - gap > discRuleL)
                            canvas.rect(discRuleL, ruleY, (labelX - gap) - discRuleL, rule,
                                        toColor(CLR_SEPARATOR));
                        if (discRuleR > labelX + lblW + gap)
                            canvas.rect(labelX + lblW + gap, ruleY,
                                        discRuleR - (labelX + lblW + gap), rule,
                                        toColor(CLR_SEPARATOR));
                    }
                    rowY += discHeaderH;
                }

                trackRowTop_[i] = (int)(rowY + scroll);
                bool visible = (rowY + trackRowHeight_ >= tp.y) && (rowY <= tp.y + tp.h);
                if (!visible) { rowY += trackRowHeight_; continue; }

                bool isPlayingRow = (displayAlbum_ == selectedAlbumIdx_ && displayTrack_ == i && isPlaying_);
                if (isPlayingRow) {
                    // Playing row: accent-tint pill + left bar (one selection family) —
                    // full height + square, matching the hover highlight exactly.
                    canvas.rect(rowX, rowY, rowW, (float)trackRowHeight_,
                                toColor(CLR_ACCENT, UI_SELECT_TINT_ALPHA), UI_CORNER_RADIUS);
                    canvas.rect(rowX, rowY, metrics_.stroke(3.0f), (float)trackRowHeight_,
                                toColor(CLR_ACCENT), UI_CORNER_RADIUS);
                } else if (hoverTrackIdx_ == i) {
                    canvas.rect(rowX, rowY, rowW, (float)trackRowHeight_, toColor(CLR_HOVER), UI_CORNER_RADIUS);
                }

                // Quality tier, as one small mark in its own column. It keeps
                // its tier colour on the playing row too: this is the only
                // reading of quality left in the list, and the duration beside
                // it already stays neutral there.
                QualityColor tc = qualityColorFor(tr.sampleRate, false);
                if (tc.hasColor) {
                    LayoutRect markRc{ (int)markX,
                                       (int)(rowY + (trackRowHeight_ - markSize) * 0.5f),
                                       (int)(markX + markSize),
                                       (int)(rowY + (trackRowHeight_ + markSize) * 0.5f) };
                    drawUiIconGlyph(canvas, markRc, UiIcon::Quality, toColor(tc.color));
                }

                // Track number / duration are numeric readouts: Mono (repurposed
                // Math style slot) keeps digits from jittering column-to-column.
                int trackNum = tr.trackNumber > 0 ? tr.trackNumber : i + 1;
                std::string trackNumStr = std::to_string(trackNum);
                // Baselines centered by the actual text size (the old "-6"
                // magic offset drifted across resolutions), columns scaled.
                float trackNumW = canvas.textWidthStyled(trackNumStr, metrics_.text.body, FontStyle::Math);
                canvas.textStyled(trackNumStr, colX + numColW - trackNumW,
                                rowY + trackRowHeight_ * 0.5f - metrics_.text.body * 0.5f,
                                metrics_.text.body, toColor(isPlayingRow ? CLR_ACCENT : CLR_TEXT_SECONDARY), FontStyle::Math);
                // Base-name priority: only the trailing "(from the Netflix
                // Series...)" modifier ever gets truncated, never the name.
                // Measured back from the mark column, which is the leftmost
                // thing on the right-hand side of the row.
                float titleMaxW = markX - metrics_.space(SP_MD) - (colX + titleX);
                FontStyle rowStyle = isPlayingRow ? FontStyle::Bold : FontStyle::Roman;
                drawNameWithModifier(canvas, tr.title,
                                     colX + titleX,
                                     rowY + trackRowHeight_ * 0.5f - metrics_.text.body * 0.5f,
                                     titleMaxW, metrics_.text.body,
                                     isPlayingRow ? CLR_ACCENT : CLR_TEXT_PRIMARY, rowStyle);

                int durMs = tr.durationMs;
                if (durMs > 0) {
                    char durBuf[16];
                    snprintf(durBuf, sizeof(durBuf), "%d:%02d", durMs / 60000, (durMs % 60000) / 1000);
                    float durW = canvas.textWidthStyled(durBuf, metrics_.text.secondary, FontStyle::Math);
                    canvas.textStyled(durBuf, durRight - durW,
                                    rowY + trackRowHeight_ * 0.5f - metrics_.text.secondary * 0.5f,
                                    metrics_.text.secondary, toColor(CLR_TEXT_SECONDARY), FontStyle::Math);
                }

                rowY += trackRowHeight_;
            }
            float tracksBottom = rowY;

            // ── Sidecar text sections (album description, artist bio) ──
            float sectY = std::max(tracksBottom, artY + artSize) + metrics_.space(36.0f);
            float textW = tp.w - pad * 2.0f;
            if (albumTextWrapW_ != textW) {
                albumDescLines_.clear();
                artistBioLines_.clear();
                if (!albumDescText_.empty())
                    wrapText(canvas, albumDescText_, textW, metrics_.text.secondary, FontStyle::Roman, albumDescLines_);
                if (!artistBioText_.empty())
                    wrapText(canvas, artistBioText_, textW, metrics_.text.secondary, FontStyle::Roman, artistBioLines_);
                albumTextWrapW_ = textW;
            }
            float lineAdv = metrics_.text.secondary * 1.5f;
            auto drawSection = [&](const std::string& caption,
                                   const std::vector<std::string>& lines, float& yy) {
                if (lines.empty()) return;
                canvas.textStyled(caption, tp.x + pad, yy, metrics_.text.caption,
                                  toColor(CLR_TEXT_DIM), FontStyle::Bold);
                yy += metrics_.text.caption * 2.2f;
                for (auto& ln : lines) {
                    if (ln.empty()) { yy += lineAdv * 0.6f; continue; }
                    // Height accounting always runs; drawing is culled to
                    // the visible band.
                    if (yy + lineAdv >= tp.y && yy <= tp.y + tp.h)
                        canvas.textStyled(ln, tp.x + pad, yy, metrics_.text.secondary,
                                          toColor(CLR_TEXT_SECONDARY), FontStyle::Roman);
                    yy += lineAdv;
                }
                yy += metrics_.space(28.0f);
            };
            const float textTop = sectY;
            drawSection("ABOUT THIS ALBUM", albumDescLines_, sectY);
            if (!artistBioLines_.empty()) {
                // Artist image above the bio. It is CROPPED to the panel by
                // hand, via imageFg's UV sub-rect, rather than gated on fitting
                // whole: imageFg composites above the vector layer, so an
                // overflowing photo would paint over the transport bar, and
                // setClip is explicitly a tile-granular (~16px) safety net, not
                // an exact mask. Requiring it to fit entirely was the cheap
                // way out and it cost a bug — the photo's height still counted
                // toward the layout while nothing was drawn, so scrolling down
                // hit a long dead gap and then the photo snapped into view.
                // Cropping draws exactly the visible slice, to the pixel.
                if (artistImgTex_ != kInvalidTexture) {
                    float imgSize = metrics_.space(196.0f);
                    float top     = std::max(sectY, tp.y);
                    float bottom  = std::min(sectY + imgSize, tp.y + tp.h);
                    if (bottom > top) {
                        float v0 = (top - sectY) / imgSize;
                        float v1 = (bottom - sectY) / imgSize;
                        rcArtistImg_ = LayoutRect{ (int)(tp.x + pad), (int)top,
                                                   (int)(tp.x + pad + imgSize), (int)bottom };
                        canvas.imageFg(artistImgTex_, tp.x + pad, top, imgSize, bottom - top,
                                       0.0f, v0, 1.0f, v1);
                    } else {
                        rcArtistImg_ = LayoutRect{ 0, 0, 0, 0 };
                    }
                    sectY += imgSize + metrics_.space(16.0f);
                }
                drawSection(album.artist.empty() ? std::string("ABOUT THE ARTIST")
                                                 : album.artist, artistBioLines_, sectY);
            }
            // The prose column, clipped to what's on screen — read only by the
            // cursor logic (see cursorForPoint). This block deliberately has
            // no hover background: it reads as a printed page, so the cursor
            // is its only affordance.
            {
                float top = std::max(textTop, tp.y);
                float bot = std::min(sectY, tp.y + tp.h);
                rcAlbumText_ = (bot > top)
                    ? LayoutRect{ (int)(tp.x + pad), (int)top,
                                  (int)(tp.x + pad + textW), (int)bot }
                    : LayoutRect{ 0, 0, 0, 0 };
            }

            // ── OTHER VERSIONS — the rest of this album's group ──────────
            // The same release held more than once: another edition (Deluxe,
            // Edición Especial) or the same edition at another quality. The
            // grid shows only the group's best member, so this strip is the
            // ONLY way to reach the others — see core/variants.h.
            //
            // These are FULL-SIZE grid tiles — the same gridArtSize_ art over
            // the same centered title / modifier / artist stack the main grid
            // draws. A variant is an album, so it is shown as one; shrinking it
            // would say it were a lesser thing. It also means the art comes out
            // of getGridArtTexture() at 1:1, the density it was decoded for.
            //
            // NO quality figure and no tier mark. Sample rate and bit depth do
            // not tell you which version you want — every FLAC in this library
            // reads much the same, and the numbers were just noise repeated
            // under every tile. Only a format that changes what those numbers
            // MEAN is called out: MP3 (lossy — its 16/44.1 is reconstructed)
            // and DSD. FLAC is the baseline and stays unlabelled.
            rcVariantTiles_.clear();
            {
                std::vector<int> others = otherVariantsOf(selectedAlbumIdx_);
                if (!others.empty()) {
                    // "OTHER VERSIONS" is wrong on a remix page: a remix
                    // already IS a version, so the caption says nothing. What
                    // is actually below is simply more of them.
                    const char* stripCaption =
                        album.releaseType == Album::ReleaseType::Remix
                            ? "MORE REMIXES" : "OTHER VERSIONS";
                    canvas.textStyled(stripCaption, tp.x + pad, sectY,
                                      metrics_.text.caption, toColor(CLR_TEXT_DIM),
                                      FontStyle::Bold);
                    sectY += metrics_.text.caption * 2.2f;

                    const float artW  = (float)gridArtSize_;
                    const float gapX  = metrics_.space(SP_LG);
                    const float stepX = artW + gapX;
                    const float adv   = titleArtistAdvance(metrics_.text.body);
                    // art + gap + title + modifier + artist + the format line.
                    // The format line's slot is reserved whether or not it is
                    // used, so tiles in a row stay the same height.
                    const float tileH = artW + metrics_.space(16.0f) + adv * 3.0f +
                                        metrics_.text.caption * 1.6f;
                    const float stepY = tileH + metrics_.space(SP_LG);
                    // Wrap to a second row rather than shrink or truncate: the
                    // page already scrolls as one, so extra rows cost nothing
                    // but height, and hiding a version defeats the strip.
                    int perRow = std::max(1, (int)((textW + gapX) / stepX));
                    int rows   = ((int)others.size() + perRow - 1) / perRow;

                    for (size_t i = 0; i < others.size(); i++) {
                        int vIdx = others[i];
                        const Album& v = albums_[vIdx];
                        float tx = tp.x + pad + (float)((int)i % perRow) * stepX;
                        float ty = sectY + (float)((int)i / perRow) * stepY;

                        // Hover frame FIRST, so the art covers its middle and
                        // it reads as a halo — the order the grid tiles use.
                        // Grey, never accent: accent means state.
                        if (hoverVariantIdx_ == vIdx)
                            canvas.rect(tx - metrics_.space(SP_XS), ty - metrics_.space(SP_XS),
                                        artW + metrics_.space(12.0f), artW + metrics_.space(12.0f),
                                        toColor(CLR_HOVER), UI_CORNER_RADIUS);

                        // The art is imageFg — composited ABOVE the vector
                        // layer, so setClip does not contain it and a
                        // scrolled-past tile would paint over the transport
                        // bar. Crop it by hand to the visible band, exactly as
                        // the artist photo above does. The placeholder is a
                        // plain rect, which setClip DOES clip, so it needs
                        // none of this.
                        TextureHandle tex = getGridArtTexture(vIdx);
                        if (tex != kInvalidTexture) {
                            float top = std::max(ty, tp.y);
                            float bot = std::min(ty + artW, tp.y + tp.h);
                            if (bot > top)
                                canvas.imageFg(tex, tx, top, artW, bot - top,
                                               0.0f, (top - ty) / artW,
                                               1.0f, (bot - ty) / artW);
                        } else {
                            canvas.rect(tx, ty, artW, artW, toColor(CLR_TILE_PLACEHOLDER));
                        }

                        // Text centered under the art and confined to exactly
                        // the art's width — the grid's rule, and these are
                        // grid tiles.
                        auto vCentered = [&](const std::string& s, float yy, float sz,
                                             ColorRef clr, FontStyle st) {
                            if (s.empty()) return;
                            float w = canvas.textWidthStyled(s, sz, st);
                            canvas.textStyled(s, tx + std::max(0.0f, (artW - w) * 0.5f),
                                              yy, sz, toColor(clr), st);
                        };

                        float ly = ty + artW + metrics_.space(16.0f);
                        // Title, then its edition on its own dim italic line —
                        // "(Deluxe)", "- Edición Especial", a remix tag. That
                        // second line is the whole point of the strip: it is
                        // what tells one version from another at a glance.
                        std::string vBase, vMod;
                        splitNameModifier(v.displayName, vBase, vMod);
                        vCentered(truncateToWidth(canvas, vBase, artW, metrics_.text.body, FontStyle::Bold),
                                  ly, metrics_.text.body, CLR_TEXT_ALBUM_TITLE, FontStyle::Bold);
                        vCentered(truncateToWidth(canvas, vMod, artW, metrics_.text.secondary, FontStyle::Italic),
                                  ly + adv, metrics_.text.secondary, CLR_TEXT_DIM, FontStyle::Italic);
                        // Artist in a fixed slot, so it aligns across tiles
                        // whether or not a version carried an edition line. A
                        // group shares a base name but NOT necessarily an
                        // artist credit — a collaboration is its own version —
                        // which is why this is printed rather than assumed.
                        vCentered(truncateToWidth(canvas, v.artist, artW, metrics_.text.secondary, FontStyle::Italic),
                                  ly + adv * 2.0f, metrics_.text.secondary,
                                  CLR_TEXT_SECONDARY, FontStyle::Italic);
                        // MP3 / DSD only — see variantFormatLabel().
                        vCentered(variantFormatLabel(v), ly + adv * 3.0f,
                                  metrics_.text.caption, CLR_TEXT_DIM, FontStyle::Math);

                        // Hit rect: the whole tile, clipped to what is on
                        // screen — you can only click what you can see.
                        float hTop = std::max(ty, tp.y);
                        float hBot = std::min(ty + tileH, tp.y + tp.h);
                        if (hBot > hTop)
                            rcVariantTiles_.emplace_back(
                                LayoutRect{ (int)tx, (int)hTop,
                                            (int)(tx + artW), (int)hBot }, vIdx);
                    }
                    sectY += (float)rows * stepY;
                }
            }
            albumViewContentH_ = (int)(sectY + scroll - tp.y + pad);

            // The third instance of the scroll invariant (see recalcLayout()
            // for the grid). It has to live HERE, at the tail of the draw,
            // because albumViewContentH_ is measured by the draw itself —
            // recalcLayout() only ever sees the previous frame's value, and
            // this page's content height genuinely changes with the panel
            // width (the title block wraps, variant tiles reflow).
            //
            // So this one self-heals in a single frame instead of zero:
            // resizing taller can leave trackScrollY_ past the end for the
            // frame that discovers it, and markDirty() schedules the repaint
            // that puts it right. Without it the tracklist stays scrolled off
            // the top until the listener touches the wheel.
            const int panelH = rcTrackPanel_.bottom - rcTrackPanel_.top;
            const int clamped = (int)clampScroll((float)trackScrollY_,
                                                 (float)albumViewContentH_,
                                                 (float)panelH);
            if (clamped != trackScrollY_) { trackScrollY_ = clamped; markDirty(); }

            canvas.clearClip();
        }

        // No on-screen close button — Escape closes the album view.
    }

    // ── Transport bar ────────────────────────────────────────────────────
    {
        Rect t = toRect(rcTransport_);
        canvas.rect(t.x, t.y, t.w, t.h, toColor(CLR_BG_TRANSPORT));
        canvas.rect(t.x, t.y, t.w, metrics_.stroke(1.0f), toColor(CLR_SEPARATOR));

        Rect artR = toRect(rcTransportArt_);
        drawArtOrPlaceholder(canvas, transportArtTex_, artR.x, artR.y, artR.w, artR.h);

        // Now-playing title/artist. Title gets base-name priority: the
        // trailing "(Deluxe)"-class modifier is what truncates first, never
        // the name itself (see splitNameModifier).
        Rect infoR = toRect(rcTransportInfo_);
        drawNameWithModifier(canvas,
                             currentTitle_.empty() ? "No track" : currentTitle_,
                             infoR.x, infoR.y, infoR.w,
                             metrics_.text.title, CLR_TEXT_PRIMARY, FontStyle::Bold);
        std::string artist = currentArtist_;
        if (!artist.empty()) {
            std::string a = truncateToWidth(canvas, artist, infoR.w,
                                            metrics_.text.secondary, FontStyle::Italic);
            canvas.textStyled(a, infoR.x, infoR.y + titleArtistAdvance(metrics_.text.title),
                              metrics_.text.secondary, toColor(CLR_TEXT_SECONDARY), FontStyle::Italic);
        }

        // Three buttons only — prev / play-stop / next, same combined
        // play-stop toggle as Essential mode. No pause: this user only ever
        // stops or starts from zero.
        struct BtnDef { LayoutRect rc; int idx; UiIcon icon; ColorRef clr; };
        BtnDef buttons[] = {
            { rcBtnPrev_, 0, UiIcon::Prev, CLR_TEXT_PRIMARY },
            { rcBtnPlay_, 1, isPlaying_ ? UiIcon::Stop : UiIcon::Play,
                             isPlaying_ ? CLR_TEXT_PRIMARY : CLR_ACCENT },
            { rcBtnNext_, 2, UiIcon::Next, CLR_TEXT_PRIMARY },
        };
        for (auto& b : buttons) {
            if (hoverTransportBtn_ == b.idx) {
                Rect r = toRect(b.rc);
                canvas.rect(r.x, r.y, r.w, r.h, toColor(CLR_HOVER), UI_CORNER_RADIUS);
            }
            drawUiIcon(canvas, b.rc, b.icon, toColor(b.clr));
        }

        // Right side, minimal: elapsed/total time, then the DSP state tag,
        // baseline-aligned and vertically centered in the bar. Hovering the
        // tag swaps the whole cluster for the full signal-path readout
        // (source format » DSP stage » output backend). '→' (U+2192) IS baked
        // now (see refreshGlyphs), so swapping it in is a free choice
        // rather than a missing glyph — '»' stays because a chevron reads as a
        // separator at this size where an arrow reads as a claim of direction.
        {
            // Reflects what the chain ACHIEVED (bpState_, set in onPlay), not
            // what the toggle requested — claiming BITPERFECT while silently
            // truncating would be exactly the dishonesty this badge exists to
            // prevent. Before playback starts bpState_ is Off, so fall back to
            // the toggle for the idle label.
            const char* dsp;
            ColorRef    dspClr;
            switch (bpState_) {
            case BpState::Exact:     dsp = "BITPERFECT";     dspClr = CLR_ACCENT;   break;
            // Asterisk, not a lie in either direction: exact up to the server.
            case BpState::ViaServer: dsp = "BITPERFECT*";    dspClr = CLR_ACCENT;   break;
            case BpState::Degraded:  dsp = "NOT BITPERFECT"; dspClr = CLR_WARNING;  break;
            case BpState::Off:
            default:
                dsp    = bitperfectMode_.load() && !isPlaying_ ? "BITPERFECT" : "REF EQ";
                dspClr = bitperfectMode_.load() && !isPlaying_ ? CLR_ACCENT : CLR_TEXT_DIM;
                break;
            }
            // The outer margin must EXCEED the gap inside the cluster (the
            // space(24) between clock and badge, below). At 16 it did not, so
            // the reading and the state sat closer to the window edge than to
            // each other and read as one run of text.
            float rightEdge = t.x + t.w - metrics_.space(SP_LG);
            float cy = t.y + t.h * 0.5f;
            float tagW = canvas.textWidthStyled(dsp, metrics_.text.caption, FontStyle::Math);

            // Hover hit rect always tracks the compact tag's home (with a
            // little slop), so the hover state stays stable while the
            // expanded readout is showing.
            const float badgeSlop = metrics_.space(8.0f);
            rcDspBadge_ = { (int)(rightEdge - tagW - badgeSlop), (int)(cy - metrics_.text.caption),
                            (int)(rightEdge + badgeSlop),        (int)(cy + metrics_.text.caption) };

            if (hoverDspBadge_) {
                std::string src;
                if (displayAlbum_ >= 0 && displayAlbum_ < (int)albums_.size() &&
                    displayTrack_ >= 0 && displayTrack_ < (int)albums_[displayAlbum_].tracks.size()) {
                    const Track& dt = albums_[displayAlbum_].tracks[displayTrack_];
                    src = formatQualityText(dt.sampleRate, dt.bitDepth);
                }
                struct Seg { std::string text; ColorRef clr; };
                std::vector<Seg> segs;
                if (!src.empty()) {
                    segs.push_back({src, CLR_TEXT_DIM});
                    segs.push_back({" \xC2\xBB ", CLR_TEXT_DIM});
                }
                segs.push_back({dsp, dspClr});
                segs.push_back({" \xC2\xBB ", CLR_TEXT_DIM});
                segs.push_back({audioBackendLabel(), CLR_TEXT_DIM});
                // The reason, in words. "Full transparency" means the user
                // should never have to infer WHY the badge says what it says.
                if (!bpDetail_.empty()) {
                    segs.push_back({"  \xE2\x80\x94  ", CLR_TEXT_DIM});
                    segs.push_back({bpDetail_, bpState_ == BpState::Degraded
                                                   ? CLR_WARNING : CLR_TEXT_DIM});
                }

                float total = 0;
                for (auto& s : segs) total += canvas.textWidthStyled(s.text, metrics_.text.caption, FontStyle::Math);
                float sx = rightEdge - total;
                float sy = cy - metrics_.text.caption * 0.5f;
                for (auto& s : segs) {
                    canvas.textStyled(s.text, sx, sy, metrics_.text.caption, toColor(s.clr), FontStyle::Math);
                    sx += canvas.textWidthStyled(s.text, metrics_.text.caption, FontStyle::Math);
                }
            } else {
                canvas.textStyled(dsp, rightEdge - tagW, cy - metrics_.text.caption * 0.5f,
                                  metrics_.text.caption, toColor(dspClr), FontStyle::Math);

                char timeBuf[64];
                snprintf(timeBuf, sizeof(timeBuf), "%d:%02d / %d:%02d",
                        seekPosMs_ / 60000, (seekPosMs_ % 60000) / 1000,
                        seekTotalMs_ / 60000, (seekTotalMs_ % 60000) / 1000);
                float timeW = canvas.textWidthStyled(timeBuf, metrics_.text.secondary, FontStyle::Math);
                canvas.textStyled(timeBuf, rightEdge - tagW - metrics_.space(24.0f) - timeW,
                                  cy - metrics_.text.secondary * 0.5f,
                                  metrics_.text.secondary, toColor(CLR_TEXT_SECONDARY), FontStyle::Math);
            }
        }
    }

    // (No on-screen mode toggle — Alt+L switches Essential/Complete.)

    // ── Audio notice strip (non-modal, both platforms) — see audioNotice_ ──
    if (!audioNotice_.empty()) {
        Rect w = toRect(rcAudioNotice_);
        canvas.rect(w.x, w.y, w.w, w.h, toColor(CLR_WARNING, UI_SELECT_TINT_ALPHA));
        const float hair = metrics_.stroke(1.0f);
        canvas.rect(w.x, w.y, w.w, hair, toColor(CLR_WARNING));                // top hairline
        canvas.rect(w.x, w.y + w.h - hair, w.w, hair, toColor(CLR_WARNING));   // bottom hairline

        float iconSize = w.h - metrics_.space(8.0f);
        float iconX = w.x + metrics_.space(8.0f), iconY = w.y + metrics_.space(4.0f);
        LayoutRect iconRc = { (int)iconX,             (int)iconY,
                              (int)(iconX + iconSize), (int)(iconY + iconSize) };
        drawWarningIcon(canvas, iconRc, toColor(CLR_WARNING));

        float textX = iconRc.right + metrics_.space(8.0f);
        float textY = w.y + w.h * 0.5f - metrics_.text.secondary * 0.5f;
        canvas.text(audioNotice_, textX, textY, metrics_.text.secondary, toColor(CLR_WARNING));
    }

    renderer_->draw(frameCurves_, /*overlay_rotation_deg=*/0, frameImages_, frameImagesFg_, msdfQuads_, frameShapes_);
}

// ── Layout ───────────────────────────────────────────────────────────────────

void PlayerWindow::recalcLayout() {
    // host_->init() (CreateWindowExW on Windows) can synchronously deliver a
    // WM_SIZE before it returns, reaching here via onHostLayoutInvalidated()
    // while renderer_ is still null (it isn't constructed until create()
    // resumes after host_->init() — see player_view.cc's create()). The real
    // layout pass runs once construction actually reaches that point; this
    // one is a spurious pre-construction echo, not a resize to honor.
    if (!renderer_) return;
    int W = (int)renderer_->width(), H = (int)renderer_->height();

    // WHICH TILE IS AT THE TOP-LEFT, read while the members still describe the
    // OLD layout. Restored at the bottom of this function, once the new column
    // count is known.
    //
    // Without it a resize is measured in pixels, and a pixel offset means a
    // different row the moment gridCols_ changes — so widening the window
    // teleported the listener somewhere else in the library. Worse, nothing
    // re-clamped the offset at all: packing the same albums into more columns
    // shrinks gridTotalHeight_, leaving gridScrollY_ past the end, and
    // drawFrame()'s firstRow then started beyond the last tile and emitted
    // NOTHING — a blank content area with no empty-state message either
    // (gridIndices_ is not empty, so that branch is not taken). It looked like
    // the library had vanished until the next scroll re-clamped it.
    //
    // Only the tile-COUNT changing was ever guarded, by resetting to 0 after
    // rebuildGridIndices() (search, section switch, filter, rescan).
    const int anchorTile = gridAnchorTile(gridScrollY_,
                                          gridTileSize_ + gridRowGap_,
                                          gridCols_);

    // Type roles + geometry factor, both from the window's content height.
    metrics_ = computeUiMetrics((float)H);


    // ── Essential-mode geometry — computed in BOTH modes, deliberately ──────
    //
    // Not just when Essential is active: loadTransportArtTexture() sizes the
    // now-playing texture for max(transportArt, essentialArt) so that toggling
    // modes never stretches it. With rcEssentialArt_ left at {} in Complete
    // mode, that max() picked the ~92px transport thumb, and Alt+L then scaled
    // it ~5.4x — the blurry-art bug. Its tell was that the art snapped sharp on
    // the next track change, because that reload finally saw a populated rect.
    //
    // This block is pure arithmetic on W/H (no cache side effects), so running
    // it in Complete mode costs nothing.
    {
        // Phone-shaped panel: art fills most of it, title band below,
        // prev/play-stop/next centered near the bottom. No seek bar, no artist
        // text, per design. Geometry is deliberately proportional to the window
        // (W/20, H/5, ...) rather than going through metrics_.space() — a
        // compact mode sized off window fractions is a valid choice, not drift,
        // and it is what lets this mode work at sizes Complete mode refuses.
        int margin = std::max(12, W / 20);
        int btnSize = std::max(40, W / 8);
        int btnGap  = std::max(16, W / 12);
        int titleH  = (int)(metrics_.text.title * 1.6f);
        int titleGap = 12;

        // The bottom stack (title + buttons) is laid out FIRST and the art takes
        // whatever is left. The old code did the reverse — it sized the art
        // against a `bottomReserve = max(120, H/5)` that had no relationship to
        // what actually goes in it, then placed W/8-sized buttons inside. At
        // this mode's own default 1200x700 that reserve was 140px against a
        // 12 + 40 + 150 + 60 = 262px stack, so the buttons overlapped the bottom
        // 70px of the album art AND sat on top of the title band. Deriving the
        // art from the stack cannot overlap by construction, and it keeps the
        // controls at full size — this is a now-playing widget, so the buttons
        // are the last thing that should shrink.
        int bottomStack = titleGap * 2 + titleH + btnSize;
        int artSize = std::min(W - margin * 2, H - margin * 2 - bottomStack);
        artSize = std::max(artSize, 1);   // degenerate windows: never negative
        int artX = (W - artSize) / 2;
        int artY = margin;  // (no corner toggle button to clear anymore)
        rcEssentialArt_ = { artX, artY, artX + artSize, artY + artSize };

        int titleY = rcEssentialArt_.bottom + titleGap;
        rcEssentialTitle_ = { margin, titleY, W - margin, titleY + titleH };

        int totalBtnW = btnSize * 3 + btnGap * 2;
        int btnX = (W - totalBtnW) / 2;
        int btnY = H - margin - btnSize;
        rcEssentialPrev_     = { btnX, btnY, btnX + btnSize, btnY + btnSize };
        btnX += btnSize + btnGap;
        rcEssentialPlayStop_ = { btnX, btnY, btnX + btnSize, btnY + btnSize };
        btnX += btnSize + btnGap;
        rcEssentialNext_     = { btnX, btnY, btnX + btnSize, btnY + btnSize };
    }

    if (uiMode_ == UiMode::Essential) return;

    // Every fixed-pixel value below is authored at the 1080 reference height
    // and passed through metrics_.space() — see ui_metrics.hh. Values that used
    // to read `X * us` were re-authored as trunc(X * 1.63389), the old factor at
    // 1080: TRUNCATED, not rounded, because the original code cast with (int).
    // Rounding instead shifts these by a pixel each and compounds to ~8px on the
    // settings rows. Values consumed as floats and accumulated (navRowH, navTop,
    // settOffset) keep their fraction. Values that were bare kept their number.
    int transportH = (int)metrics_.space(130.0f);  // scales with the text it contains

    // Sidebar width scales the same way — fixed pixel widths with
    // height-scaled text is how "MATRIX PLAYER" ended up painted over the
    // first grid column's art at 1080p: the sidebar stayed 170px while its
    // text grew ~1.6x.
    int sidebarW = (int)metrics_.space(277.0f);

    rcTransport_ = { 0, H - transportH, W, H };
    rcSidebar_   = { 0, 0, sidebarW, H - transportH };

    // The album view is a full page, not a side panel: opening an album
    // replaces the grid entirely (the grid draw is skipped while it's open).
    // rcGrid_ stays the full content area in both states — the settings
    // page and the grid share it.
    rcGrid_ = { sidebarW, 0, W, H - transportH };
    rcTrackPanel_ = trackPanelOpen_ ? rcGrid_ : LayoutRect{ 0, 0, 0, 0 };

    // The search strip eats into the GRID only, and only when it has something
    // to say: the chips currently applied, and — while the box has focus — the
    // suggestions on offer. It is deliberately not part of rcTrackPanel_/the
    // settings page: those are full pages the listener opened on purpose.
    // Taking it off rcGrid_ here — after rcTrackPanel_ is assigned — means the
    // draw loop, gridHitTest() and the scroll extent all pick the new top edge
    // up on their own.
    //
    // It RESERVES space rather than floating over the grid, and that is not a
    // stylistic choice: the renderer draws every rect (frameCurves_) before
    // every glyph (msdfQuads_), so a panel painted last still comes out UNDER
    // any text painted earlier. An overlay dropdown here renders as the
    // sidebar's nav labels showing straight through it — seen, not guessed.
    rcChips_ = { 0, 0, 0, 0 };
    if (!settingsOpen_ && !trackPanelOpen_) {
        int rowH = (int)metrics_.space(46.0f);
        int rows = (searchChips_.empty() ? 0 : 1)
                 + ((searchFocused_ && !searchSuggest_.empty()) ? 1 : 0);
        if (rows > 0) {
            int chipsH = rowH * rows + (int)metrics_.space(12.0f);
            rcChips_ = { sidebarW, 0, W, chipsH };
            rcGrid_.top += chipsH;
        }
    }

    // Grid columns — derived from a target tile pitch (~220px art + margins)
    // rather than a fixed column count, so density stays consistent across
    // window widths and panel open/close: more of the library visible at a
    // glance, tiles still large enough to enjoy the art.
    static constexpr int kTargetTilePitch = 250;  // desired cell stride incl. margins
    static constexpr int kMinGridArtSize  = 80;   // legible floor; drop a column instead of going below it
    static constexpr int kGridArtMargin   = 30;   // gap reserved around the art within its cell

    // These three were bare literals (never scaled), so they keep their numbers
    // and merely gain space() — a no-op at the reference height, and it finally
    // makes tile density track the window on a taller display.
    const int tilePitch  = (int)metrics_.space((float)kTargetTilePitch);
    const int minArtSize = (int)metrics_.space((float)kMinGridArtSize);
    const int artMargin  = (int)metrics_.space((float)kGridArtMargin);

    gridPadXpx_ = (int)metrics_.space((float)gridPadX_);
    int gridW = rcGrid_.right - rcGrid_.left - gridPadXpx_ * 2;
    int desiredCols = std::clamp(gridW / tilePitch, 2, 8);
    while (desiredCols > 1 && (gridW / desiredCols) - artMargin < minArtSize) desiredCols--;
    gridCols_ = std::max(1, desiredCols);

    int newGridArtSize = std::max(minArtSize, gridW / gridCols_ - artMargin);
    if (newGridArtSize != gridArtSize_) {
        // Tile size changed (resize, monitor change, panel open/close) — cached
        // art was decoded for the old size, so it must reload at the new one.
        gridArtSize_ = newGridArtSize;
        clearGridArtTexCache();
    }
    gridTileSize_ = gridArtSize_ + artMargin;

    // Cell stride, resolved once. The draw block and gridHitTest() both used
    // to recompute this from raw pads and disagree with the line above.
    gridStepX_ = gridCols_ > 1 ? gridW / gridCols_ : gridTileSize_;

    // Derived, not authored — see gridTopPad()'s comment in ui_metrics.hh.
    gridPadYpx_ = gridTopPad(gridPadXpx_, gridStepX_, gridArtSize_);

    // Tile text block height from the ACTUAL text sizes (two title lines +
    // artist + breathing room) — see gridRowGap_'s comment in the header.
    gridRowGap_ = (int)(titleArtistAdvance(metrics_.text.body) * 2.0f
                        + metrics_.text.secondary * 1.35f + metrics_.space(29.41f));

    // Track rows likewise scale with their text.
    trackRowHeight_ = (int)metrics_.space(SP_XL);

    // Tile count is whichever section is showing — the Playlists section draws
    // its three tiles on this same geometry, so the scroll extent has to come
    // from the same place or the wheel would clamp against the album grid's.
    int tileCount = (navSection_ == NavSection::Playlists)
                        ? 3 : (int)gridIndices_.size();
    int albumRows = (tileCount + gridCols_ - 1) / gridCols_;
    gridTotalHeight_ = albumRows * (gridTileSize_ + gridRowGap_) + gridPadYpx_;

    // Sidebar items — search box sits between the brand and the nav. All
    // Y positions scale with the text (fixed values put "Albums" visibly
    // adrift of the search box across resolutions).
    rcBrand_       = { 0, 0, sidebarW, (int)metrics_.space(81.0f) };
    const int searchInset = (int)metrics_.space(12.0f);
    rcSearch_      = { searchInset, (int)metrics_.space(94.0f),
                       sidebarW - searchInset, (int)metrics_.space(147.0f) };
    float navRowH = metrics_.space(65.3556f), navTop = metrics_.space(166.6568f);
    // ROW ORDER IS NOT ENUM ORDER, and the two must not be conflated. The enum
    // values are frozen — they are stored as integers in Db's albums table — so
    // the reading order of the sidebar lives here and only here. It runs:
    // original material by descending size (Albums, EPs, Singles), then the
    // artist's own material re-presented (Compilations, Live), then other
    // people's reworkings of it (Remixes).
    rcNavAlbum_  = { 0, (int)(navTop),               sidebarW, (int)(navTop + navRowH) };
    rcNavEp_     = { 0, (int)(navTop + navRowH),     sidebarW, (int)(navTop + navRowH * 2) };
    rcNavSingle_ = { 0, (int)(navTop + navRowH * 2), sidebarW, (int)(navTop + navRowH * 3) };
    rcNavCompilation_ = { 0, (int)(navTop + navRowH * 3), sidebarW, (int)(navTop + navRowH * 4) };
    rcNavLive_   = { 0, (int)(navTop + navRowH * 4), sidebarW, (int)(navTop + navRowH * 5) };
    rcNavRemix_  = { 0, (int)(navTop + navRowH * 5), sidebarW, (int)(navTop + navRowH * 6) };
    // Playlists sits ABOVE the hairline, with the content filters: it is a way
    // of browsing music, not a setting. Settings therefore moves down a slot.
    rcNavPlaylists_ = { 0, (int)(navTop + navRowH * 6), sidebarW, (int)(navTop + navRowH * 7) };
    const float settOffset = metrics_.space(13.0711f);
    rcNavSettings_ = { 0, (int)(navTop + navRowH * 7 + settOffset),
                          sidebarW, (int)(navTop + navRowH * 8 + settOffset) };

    // Transport sub-regions — proportional to the (scaled) bar height.
    int tTop = rcTransport_.top;
    int tPad = (int)metrics_.space(SP_MD);
    int artSide = transportH - 2 * tPad;
    rcTransportArt_  = { tPad, tTop + tPad, tPad + artSide, tTop + tPad + artSide };

    // Bitperfect-mismatch warning strip: a full-width overlay directly above
    // the transport bar. Doesn't reserve/shrink grid space — this is a rare,
    // transient event, not worth a permanent layout dependency.
    int warnH = (int)metrics_.space(45.0f);
    rcAudioNotice_ = { 0, tTop - warnH, W, tTop };

    // Center buttons: the app's primary interactive elements (44px at the
    // reference window; scaled like everything else). Three of them:
    // prev / play-stop / next (no pause, no separate stop).
    int btnSize = (int)metrics_.space(71.0f);
    int btnGap = (int)metrics_.space(SP_MD);
    int totalBtnW = btnSize * 3 + btnGap * 2;
    int btnX = W / 2 - totalBtnW / 2;
    int btnY = tTop + (transportH - btnSize) / 2;
    rcBtnPrev_ = { btnX, btnY, btnX + btnSize, btnY + btnSize };
    btnX += btnSize + btnGap;
    rcBtnPlay_ = { btnX, btnY, btnX + btnSize, btnY + btnSize };
    btnX += btnSize + btnGap;
    rcBtnNext_ = { btnX, btnY, btnX + btnSize, btnY + btnSize };

    // Now-playing text runs from the art thumb to ~2cm (76px @96dpi) short
    // of the Prev button — was a fixed 360px that wasted the whole gap and
    // needlessly ellipsized titles.
    rcTransportInfo_ = { rcTransportArt_.right + tPad, tTop + (int)metrics_.space(26.0f),
                         rcBtnPrev_.left - (int)metrics_.space(76.0f),
                         tTop + transportH - tPad };

    // (The album view has no on-screen close button — Escape closes it.)

    // Settings page items — geometry scales with the same text factor as
    // the panels (fixed 400x50 rows under height-scaled text looked cramped
    // and off-center at 1080p), centered on the content area with a uniform
    // vertical rhythm.
    int settCx   = (rcGrid_.left + rcGrid_.right) / 2;
    int settTop  = (int)metrics_.space(147.0f);
    int rowHalfW = (int)metrics_.space(359.0f);
    int rowH     = (int)metrics_.space(84.0f);
    int rowStep  = rowH + (int)metrics_.space(22.0f);
    auto settRow = [&](int i) -> LayoutRect {
        return { settCx - rowHalfW, settTop + i * rowStep,
                 settCx + rowHalfW, settTop + i * rowStep + rowH };
    };
    rcSettingsAddFolder_  = settRow(0);
    rcSettingsManage_     = settRow(1);
    rcSettingsAudio_      = settRow(2);
    rcSettingsEq_         = settRow(3);
    rcSettingsBitperfect_ = settRow(4);

    // Put the anchor tile back at the top-left, now that gridCols_ and
    // gridTotalHeight_ are final, and clamp — because the new grid may be
    // shorter than the old one, and an offset past the end draws nothing at
    // all (see the anchor capture at the top of this function).
    //
    // Both numbers are current here: rcGrid_ and gridTotalHeight_ are both
    // assigned above. clampScroll() is vk_canvas's (core/layout.hh), the same
    // one core/tests/layout_test.cc already pins for the past-the-bottom case
    // — this bug was a missing call, not missing arithmetic.
    gridScrollY_ = gridScrollForAnchor(anchorTile,
                                       gridTileSize_ + gridRowGap_, gridCols_);
    gridScrollY_ = (int)clampScroll((float)gridScrollY_,
                                    (float)gridTotalHeight_,
                                    (float)(rcGrid_.bottom - rcGrid_.top));
}

// ── UI mode (Essential/Complete) ─────────────────────────────────────────────
// The actual monitor-fitting/positioning math (computeCompleteWindowRect,
// computeEssentialWindowRect equivalents) now lives in os/windows_host.cc /
// os/linux_host.cc — see host.hh's class comment for why Linux's versions of
// adaptToCurrentMonitor/snapToEdge are real no-ops (Wayland clients cannot
// query monitor work areas or set their own position) while applyUiMode's
// Complete-mode fullscreen has a real Wayland equivalent.

void PlayerWindow::toggleUiMode() {
    uiMode_ = (uiMode_ == UiMode::Complete) ? UiMode::Essential : UiMode::Complete;
    host_->applyUiMode(uiMode_);
}

void PlayerWindow::adaptToCurrentMonitor() {
    host_->adaptToCurrentMonitor(uiMode_);
}

int PlayerWindow::essentialHitTest(int x, int y) const {
    auto in = [&](const LayoutRect& r) {
        return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
    };
    if (in(rcEssentialPrev_))     return 0;
    if (in(rcEssentialPlayStop_)) return 1;
    if (in(rcEssentialNext_))     return 2;
    return -1;
}

// Alt+F/J/C/U/G/H — the window-move mechanism in place of title-bar dragging
// on Windows (there is no title bar). No-op on Linux; see the comment above.
void PlayerWindow::snapToEdge(int hotkeyId) {
    host_->snapToEdge(hotkeyId);
}

// ── Art cache (Vulkan textures) ──────────────────────────────────────────────

TextureHandle PlayerWindow::getGridArtTexture(int albumIdx, int sizeClass) {
    const int key = artKey(albumIdx, sizeClass);
    auto it = gridArtTexCache_.find(key);
    if (it != gridArtTexCache_.end()) {
        gridArtLastUse_[key] = ++artUseTick_;
        return it->second;
    }
    if (albumIdx < 0 || albumIdx >= (int)albums_.size()) return kInvalidTexture;

    // Decode at the size it will be DRAWN at. A mosaic quadrant is half the
    // tile's edge, and there is no mip chain to fall back on (see
    // onArtDecoded), so handing back the full-size texture would alias.
    const int target = (sizeClass == ArtHalf) ? std::max(1, gridArtSize_ / 2)
                                              : gridArtSize_;

    // Not cached: queue an async decode (once) and show the placeholder this
    // frame — onArtDecoded() invalidates when the texture is ready.
    if (artDecodePending_.emplace(key, 1).second) {
        if (!artDecodeThread_.joinable())
            artDecodeThread_ = std::thread([this]{ artDecodeWorker(); });
        {
            std::lock_guard<std::mutex> lk(artDecodeMu_);
            artDecodeQueue_.push_back({key, albums_[albumIdx].artPath,
                                       target, artCacheGen_.load()});
        }
        artDecodeCv_.notify_one();
    }
    return kInvalidTexture;
}

void PlayerWindow::artDecodeWorker() {
    FileByteReader reader;
    for (;;) {
        ArtDecodeJob job;
        {
            std::unique_lock<std::mutex> lk(artDecodeMu_);
            artDecodeCv_.wait(lk, [&]{ return artDecodeQuit_ || !artDecodeQueue_.empty(); });
            if (artDecodeQuit_) return;
            job = std::move(artDecodeQueue_.front());
            artDecodeQueue_.pop_front();
        }
        // Stale job from before a rescan: skip the decode entirely, but still
        // report back so the main thread clears its pending mark.
        ArtDecodeResult res;
        res.key = job.key;
        res.gen = job.gen;
        if (job.gen == artCacheGen_.load() && !job.path.empty()) {
            std::vector<uint8_t> bytes;
            if (reader.read(job.path.c_str(), bytes) && !bytes.empty()) {
                DecodedImage img = decodeImageScaled(bytes.data(), bytes.size(),
                                                     job.targetSize, job.targetSize);
                res.rgba = std::move(img.rgba);
                res.w = img.w; res.h = img.h;
            }
        }
        {
            std::lock_guard<std::mutex> lk(artDecodeMu_);
            artDecodeDone_.push_back(std::move(res));
        }
        host_->postAppEvent(AppEvent::ArtDecoded);
    }
}

void PlayerWindow::onArtDecoded() {
    std::vector<ArtDecodeResult> done;
    {
        std::lock_guard<std::mutex> lk(artDecodeMu_);
        done.swap(artDecodeDone_);
    }
    bool anyNew = false;
    for (auto& r : done) {
        artDecodePending_.erase(r.key);
        if (r.gen != artCacheGen_.load()) continue;  // rescan invalidated it
        if (gridArtTexCache_.count(r.key)) continue;  // duplicate job — keep the existing texture
        // Failed decodes cache kInvalidTexture so the tile keeps its
        // placeholder without re-queuing a doomed decode every frame —
        // same behavior the old synchronous path had.
        TextureHandle tex = kInvalidTexture;
        if (!r.rgba.empty())
            // Grid art is decoded to the tile size it's drawn at (≤2x) —
            // skip the mip chain: −33% VRAM per tile, no blit pass.
            tex = renderer_->create_texture(r.rgba.data(), (uint32_t)r.w, (uint32_t)r.h,
                                            /*mips=*/false);
        gridArtTexCache_[r.key] = tex;
        gridArtLastUse_[r.key] = ++artUseTick_;
        anyNew = true;
    }

    // LRU eviction: keep VRAM bounded on libraries with more albums than the
    // cap. Only entries not drawn recently get dropped, so everything visible
    // (stamped this frame via getGridArtTexture) always survives.
    while (gridArtTexCache_.size() > kMaxGridArtTextures) {
        int oldestKey = INT_MIN;
        uint64_t oldestTick = UINT64_MAX;
        for (auto& [key, tex] : gridArtTexCache_) {
            auto u = gridArtLastUse_.find(key);
            uint64_t tick = (u != gridArtLastUse_.end()) ? u->second : 0;
            if (tick < oldestTick) { oldestTick = tick; oldestKey = key; }
        }
        if (oldestKey == INT_MIN) break;
        auto e = gridArtTexCache_.find(oldestKey);
        if (e->second != kInvalidTexture) renderer_->destroy_texture(e->second);
        gridArtTexCache_.erase(e);
        gridArtLastUse_.erase(oldestKey);
    }

    if (anyNew) invalidate();
}

void PlayerWindow::stopArtDecodeThread() {
    {
        std::lock_guard<std::mutex> lk(artDecodeMu_);
        artDecodeQuit_ = true;
    }
    artDecodeCv_.notify_one();
    if (artDecodeThread_.joinable()) artDecodeThread_.join();
}

void PlayerWindow::clearGridArtTexCache() {
    artCacheGen_.fetch_add(1);   // invalidates queued/in-flight decodes
    artDecodePending_.clear();
    for (auto& [idx, tex] : gridArtTexCache_)
        if (tex != kInvalidTexture) renderer_->destroy_texture(tex);
    gridArtTexCache_.clear();
    gridArtLastUse_.clear();
}

void PlayerWindow::loadTrackPanelArtTexture(int albumIdx) {
    if (trackPanelArtTexAlbum_ == albumIdx && trackPanelArtTex_ != kInvalidTexture) return;
    if (trackPanelArtTex_ != kInvalidTexture) { renderer_->destroy_texture(trackPanelArtTex_); trackPanelArtTex_ = kInvalidTexture; }
    trackPanelArtTexAlbum_ = albumIdx;
    if (albumIdx >= 0 && albumIdx < (int)albums_.size()) {
        // Same target-size formula the album view's draw block uses, so the
        // texture always covers what's actually displayed.
        int panelW = rcTrackPanel_.right - rcTrackPanel_.left;
        int panelH = rcTrackPanel_.bottom - rcTrackPanel_.top;
        int targetSize = (int)std::min(panelW * 0.32f, panelH * 0.55f);
        if (targetSize <= 0) targetSize = 320;
        FileByteReader reader;
        trackPanelArtTex_ = createTextureFromImageFile(*renderer_, reader,
                                                        albums_[albumIdx].artPath.c_str(),
                                                        targetSize, targetSize,
                                                        nullptr, nullptr, /*mips=*/false);
    }
}

void PlayerWindow::openAlbumView(int albumIdx) {
    selectedAlbumIdx_ = albumIdx;
    trackPanelOpen_ = true;
    trackScrollY_ = 0;
    hoverTrackIdx_ = -1;

    // The album view belongs to the section that CONTAINS this release, and it
    // is reached from places that know nothing about the sidebar: the "other
    // versions" strip, and onPlay() retargeting the page at whatever started.
    // Without this, leaving the view dropped the listener into whichever
    // section happened to be selected before — a single playing under Remixes
    // put them back in Remixes, a section its own album is not in. Realigning
    // here means Escape always lands where the album actually lives.
    if (albumIdx >= 0 && albumIdx < (int)albums_.size()) {
        auto want = (AlbumTypeFilter)(int)albums_[albumIdx].releaseType;
        if (navSection_ != NavSection::Albums || albumTypeFilter_ != want) {
            navSection_      = NavSection::Albums;
            albumTypeFilter_ = want;
            rebuildGridIndices();
            gridScrollY_ = 0;
        }
    }
    // Layout first: loadTrackPanelArtTexture() sizes its texture from
    // rcTrackPanel_, which recalcLayout() just grew to the full page.
    recalcLayout();
    loadTrackPanelArtTexture(albumIdx);
    loadAlbumViewContent(albumIdx);
    invalidate();
}

std::string PlayerWindow::artistImagePathFor(int albumIdx) const {
    if (albumIdx < 0 || albumIdx >= (int)albums_.size()) return {};
    const Album& a = albums_[albumIdx];
    fsys::path albumDir;
    if (!a.tracks.empty())       albumDir = fsys::u8path(a.tracks[0].filePath).parent_path();
    else if (!a.artPath.empty()) albumDir = fsys::u8path(a.artPath).parent_path();
    if (albumDir.empty()) return {};

    // Same two sources, in the same order, as loadAlbumViewContent().
    std::string root = rootForPath(albumDir.u8string());
    if (!root.empty()) {
        auto it = streamerDbs_.find(root);
        if (it != streamerDbs_.end() && it->second.isOpen())
            if (auto info = it->second.artistInfoForAlbum(a.name))
                if (!info->imagePath.empty()) return info->imagePath;
    }
    fsys::path artistDir = albumDir.parent_path();
    if (artistDir.filename().u8string() == "Singles")
        artistDir = artistDir.parent_path();
    if (artistDir.empty()) return {};
    return resolveArtPath(artistDir.u8string());
}

TextureHandle PlayerWindow::loadArtistImageTexture(const std::string& path) {
    artistImgPath_ = path;   // remembered so ArtWindow can show the original
    // Decode at the size it is actually drawn, and WITHOUT mips.
    //
    // Both matter, and the second one is what made the photo look washed out
    // even at 1080. art_texture.hh spells out the trap: the sampler picks its
    // LOD from the on-screen ratio, so a mip chain "softens anything drawn
    // even slightly below 1:1". This was decoded at a fixed 256 and drawn at
    // space(196) — a ratio of 0.77, just under 1:1, which is enough to pull in
    // a blurrier level. The fixed 256 was the other half: space() scales with
    // resolution, so on a 4x display the photo was drawn at 784 from a 256px
    // source. loadTrackPanelArtTexture() already does it this way.
    const int target = std::max(64, (int)metrics_.space(196.0f));
    FileByteReader reader;
    return createTextureFromImageFile(*renderer_, reader, path.c_str(),
                                      target, target,
                                      nullptr, nullptr, /*mips=*/false);
}

void PlayerWindow::loadAlbumViewContent(int albumIdx) {
    albumDescText_.clear();
    artistBioText_.clear();
    albumDescLines_.clear();
    artistBioLines_.clear();
    albumTextWrapW_ = -1.0f;
    if (artistImgTex_ != kInvalidTexture) {
        renderer_->destroy_texture(artistImgTex_);
        artistImgTex_ = kInvalidTexture;
    }
    artistImgPath_.clear();
    rcArtistImg_ = {};
    // The strip belongs to the album being left; clearing the hover too keeps
    // a stale highlight from landing on whichever tile inherits that slot.
    rcVariantTiles_.clear();
    hoverVariantIdx_ = -1;
    if (albumIdx < 0 || albumIdx >= (int)albums_.size()) return;
    const Album& a = albums_[albumIdx];

    // Album folder from the tracks (fallback: the cover file's folder).
    fsys::path albumDir;
    if (!a.tracks.empty())      albumDir = fsys::u8path(a.tracks[0].filePath).parent_path();
    else if (!a.artPath.empty()) albumDir = fsys::u8path(a.artPath).parent_path();
    if (albumDir.empty()) return;

    albumDescText_ = loadSidecarText(albumDir, { "desc", "info", "about" });

    // Prefer a sibling .streamer/library.db (Album::name is that DB's
    // albums.id primary key) — this is the current downloader layout, where
    // the album folder's parent is just "FR", not a per-artist folder.
    bool gotFromStreamer = false;
    std::string root = rootForPath(albumDir.u8string());
    if (!root.empty()) {
        auto it = streamerDbs_.find(root);
        if (it != streamerDbs_.end() && it->second.isOpen()) {
            if (auto info = it->second.artistInfoForAlbum(a.name)) {
                if (!info->bioText.empty()) {
                    // Bio text sidecars from this downloader are HTML fragments
                    // (e.g. "<p>...</p>"), same as the legacy bio.html convention.
                    bool looksHtml = info->bioText.find('<') != std::string::npos;
                    artistBioText_ = looksHtml ? stripHtmlToPlain(info->bioText) : info->bioText;
                }
                if (!info->imagePath.empty())
                    artistImgTex_ = loadArtistImageTexture(info->imagePath);
                gotFromStreamer = !info->bioText.empty() || artistImgTex_ != kInvalidTexture;
            }
        }
    }

    // Fallback: legacy sidecar convention — artist folder is one level up
    // (two for Singles/<Title> layouts) — for libraries with no .streamer db.
    if (!gotFromStreamer) {
        fsys::path artistDir = albumDir.parent_path();
        if (artistDir.filename().u8string() == "Singles")
            artistDir = artistDir.parent_path();
        if (!artistDir.empty()) {
            artistBioText_ = loadSidecarText(artistDir, { "bio" });
            std::string artistImg = resolveArtPath(artistDir.u8string());
            if (!artistImg.empty())
                artistImgTex_ = loadArtistImageTexture(artistImg);
        }
    }
}

std::string PlayerWindow::rootForPath(const std::string& path) const {
    std::string best;
    for (auto& [root, _] : streamerDbs_)
        if (path.rfind(root, 0) == 0 && root.size() > best.size()) best = root;
    return best;
}

void PlayerWindow::loadTransportArtTexture(const std::string& artPath) {
    if (transportArtTexPath_ == artPath && transportArtTex_ != kInvalidTexture) return;
    if (transportArtTex_ != kInvalidTexture) { renderer_->destroy_texture(transportArtTex_); transportArtTex_ = kInvalidTexture; }
    transportArtTexPath_ = artPath;
    // Single choke point for "the now-playing art changed" (onPlay, gapless
    // boundary flips via applyTrackMetadata) — keep the fullscreen art
    // window in sync while it's open. Which picture it wants depends on what
    // it was opened to show: the album cover, or the ARTIST behind the track
    // now playing (resolved live, since the playing album may not be the one
    // whose page is open).
    if (artWin_.isVisible()) {
        std::string img = artWinShowsArtist_ ? artistImagePathFor(displayAlbum_)
                                             : artPath;
        if (!img.empty()) artWin_.updateImage(img);
    }
    if (!artPath.empty()) {
        // This one texture is shown in two different-sized contexts without a
        // reload between them (Complete mode's small rcTransportArt_ and
        // Essential mode's much larger rcEssentialArt_, toggled via
        // toggleUiMode() which doesn't touch this texture) — size for
        // whichever context is larger so switching modes never shows a
        // stretched-blurry version.
        int transportW = rcTransportArt_.right - rcTransportArt_.left;
        int transportH = rcTransportArt_.bottom - rcTransportArt_.top;
        int essentialW = rcEssentialArt_.right - rcEssentialArt_.left;
        int essentialH = rcEssentialArt_.bottom - rcEssentialArt_.top;
        int targetW = std::max(transportW, essentialW);
        int targetH = std::max(transportH, essentialH);
        FileByteReader reader;
        transportArtTex_ = createTextureFromImageFile(*renderer_, reader, artPath.c_str(),
                                                       targetW, targetH);
    }
}

// ── Hit testing ──────────────────────────────────────────────────────────────

// ASCII-case-insensitive substring match; non-ASCII bytes compare exact.
// Good enough for live search — matching "Björk" needs the same bytes typed,
// which the user typing from the same tags will produce.
static bool containsNoCase(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return true;
    auto lower = [](char ch) -> char {
        return (ch >= 'A' && ch <= 'Z') ? (char)(ch + 32) : ch;
    };
    if (needle.size() > hay.size()) return false;
    for (size_t i = 0; i + needle.size() <= hay.size(); i++) {
        size_t j = 0;
        while (j < needle.size() && lower(hay[i + j]) == lower(needle[j])) j++;
        if (j == needle.size()) return true;
    }
    return false;
}

void PlayerWindow::rebuildAlbumGroups() {
    albumGroups_ = groupAlbumVariants(albums_);
    albumGroupOf_.assign(albums_.size(), -1);
    for (int g = 0; g < (int)albumGroups_.size(); g++)
        for (int m : albumGroups_[g].members)
            albumGroupOf_[m] = g;

    // Rebuild the key → playable-copy index in the same pass, so it cannot be
    // stale while albums_ is fresh. A playlist is a list of trackKey()s and
    // several copies of the same music legitimately share one; the copy that
    // plays is the one on the album variantOutranks() prefers, which is the
    // very ranking the grid already uses to pick a group's tile. One rule, one
    // implementation.
    trackKeyIndex_.clear();
    trackKeyIndex_.reserve(albums_.size() * 12);
    for (int ai = 0; ai < (int)albums_.size(); ai++) {
        const Album& a = albums_[ai];
        for (int ti = 0; ti < (int)a.tracks.size(); ti++) {
            const std::string& key = a.tracks[ti].trackKey;
            if (key.empty()) continue;
            auto it = trackKeyIndex_.find(key);
            if (it == trackKeyIndex_.end()) {
                trackKeyIndex_.emplace(key, std::make_pair(ai, ti));
            } else if (it->second.first != ai &&
                       variantOutranks(a, albums_[it->second.first])) {
                // Better copy of the same music. The album-level comparison is
                // what decides: quality is a property of the release, not of
                // the one file, and comparing per track would let a single
                // odd-rate file drag a whole album's worth of history onto a
                // copy nobody wants to hear.
                it->second = std::make_pair(ai, ti);
            }
        }
    }
}

// The 2x2 mosaic a REMIX group's tile wears instead of one cover. Returns
// false when this album is not the primary of a multi-member remix group, so
// the caller falls back to the normal single-cover draw.
//
// Quadrant order is the group's own ranking (best first), reading left-to-
// right, top-to-bottom. Fewer than four members leave the remaining quadrants
// flat CLR_TILE_PLACEHOLDER — the tile's own background, so an empty quadrant
// reads as absence rather than as a broken image. MORE than four replaces the
// LAST quadrant with a fade into CLR_TILE_MORE_GREEN: three covers plus "and
// there is more", rather than an arbitrary fourth cover pretending to be the
// whole set.
bool PlayerWindow::drawVariantMosaic(Canvas& canvas, int albumIdx,
                                     float x, float y, float a) {
    if (albumIdx < 0 || albumIdx >= (int)albums_.size()) return false;
    if (albums_[albumIdx].releaseType != Album::ReleaseType::Remix) return false;
    if (albumIdx >= (int)albumGroupOf_.size()) return false;
    int g = albumGroupOf_[albumIdx];
    if (g < 0 || g >= (int)albumGroups_.size()) return false;
    const AlbumGroup& grp = albumGroups_[g];
    if (grp.members.size() < 2) return false;      // nothing to mosaic
    if (grp.primary != albumIdx) return false;     // only the tile draws it

    const int   n    = (int)grp.members.size();
    const float half = a * 0.5f;
    // The last quadrant becomes the "and more" fade once the group outgrows
    // the four slots. At exactly four, all four are covers.
    const bool  overflow = n > 4;
    const int   covers   = overflow ? 3 : std::min(n, 4);

    for (int q = 0; q < 4; q++) {
        float qx = x + (q % 2) * half;
        float qy = y + (q / 2) * half;
        if (q < covers) {
            // Half-size decode, not the full tile texture scaled down: there
            // is no mip chain here (see onArtDecoded), so a 2:1 minification
            // would alias. See getGridArtTexture's sizeClass.
            TextureHandle tex = getGridArtTexture(grp.members[q], ArtHalf);
            if (tex != kInvalidTexture) {
                canvas.imageFg(tex, qx, qy, half, half);
                continue;
            }
            // Not decoded yet (or no cover at all): the placeholder below.
        }
        if (q == 3 && overflow) {
            canvas.rectGradient(qx, qy, half, half,
                                toColor(CLR_TILE_PLACEHOLDER),
                                toColor(CLR_TILE_MORE_GREEN),
                                Canvas::GradientDir::Vertical);
        } else {
            canvas.rect(qx, qy, half, half, toColor(CLR_TILE_PLACEHOLDER));
        }
    }
    return true;
}

std::vector<int> PlayerWindow::otherVariantsOf(int albumIdx) const {
    std::vector<int> out;
    if (albumIdx < 0 || albumIdx >= (int)albumGroupOf_.size()) return out;
    int g = albumGroupOf_[albumIdx];
    if (g < 0 || g >= (int)albumGroups_.size()) return out;
    for (int m : albumGroups_[g].members)
        if (m != albumIdx) out.push_back(m);
    return out;
}

void PlayerWindow::rebuildGridIndices() {
    gridIndices_.clear();
    gridIndices_.reserve(albumGroups_.size());
    // One tile per GROUP, not per album — the group's best member represents
    // it (see rebuildAlbumGroups). The filter reads that member, since it is
    // what gets drawn.
    for (const AlbumGroup& grp : albumGroups_) {
        int i = grp.primary;
        if (i < 0 || i >= (int)albums_.size()) continue;
        const Album& a = albums_[i];
        if ((int)a.releaseType != (int)albumTypeFilter_) continue;
        // Both filters match on ANY member of the group. Without this,
        // grouping would HIDE results: a track title that only exists on the
        // variant sitting behind the tile would stop finding its own album.
        if (!searchChips_.empty()) {
            bool hit = false;
            for (size_t mi = 0; !hit && mi < grp.members.size(); mi++)
                hit = facets::matches(albums_[grp.members[mi]], searchChips_);
            if (!hit) continue;
        }
        if (!searchQuery_.empty()) {
            bool hit = false;
            for (size_t mi = 0; !hit && mi < grp.members.size(); mi++) {
                const Album& m = albums_[grp.members[mi]];
                hit = containsNoCase(m.displayName, searchQuery_) ||
                      containsNoCase(m.artist, searchQuery_);
                for (size_t t = 0; !hit && t < m.tracks.size(); t++)
                    hit = containsNoCase(m.tracks[t].title, searchQuery_);
            }
            if (!hit) continue;
        }
        gridIndices_.push_back(i);
    }
}

// ── Guided search ───────────────────────────────────────────────────────────

// The dropdown is a shortlist, not a catalogue. facets::suggest() already
// sorts reachable-first then by result size, so the rows that survive the cut
// are the ones worth offering; a longer list would cover the sidebar's nav.
static constexpr size_t kMaxSuggestRows = 8;

void PlayerWindow::refreshSuggestions() {
    searchSuggest_.clear();
    searchSuggestSel_ = -1;
    if (!searchFocused_) return;
    // Suggestions come from the WHOLE library, not from the section currently
    // showing: asking for "1990s" while Singles is open is a question about
    // the library, and offering only what this tab holds would hide most of
    // the answer and report misleading counts.
    searchSuggest_ = facets::suggest(albums_, searchQuery_, searchChips_);
    if (searchSuggest_.size() > kMaxSuggestRows)
        searchSuggest_.resize(kMaxSuggestRows);
}

void PlayerWindow::acceptSuggestion(int i) {
    if (i < 0 || i >= (int)searchSuggest_.size()) return;
    // A disabled row is offered so the listener can SEE that the value exists
    // and learn what blocks it — accepting it would just empty the screen.
    if (!searchSuggest_[i].enabled) return;
    searchChips_.push_back(searchSuggest_[i].chip);
    searchQuery_.clear();          // the text became a chip; the box starts over
    refreshSuggestions();
    rebuildGridIndices();
    gridScrollY_ = 0;
    recalcLayout();                // the chip strip changes the grid's top edge
    invalidate();
}

void PlayerWindow::removeChip(int i) {
    if (i < 0 || i >= (int)searchChips_.size()) return;
    searchChips_.erase(searchChips_.begin() + i);
    refreshSuggestions();
    rebuildGridIndices();
    gridScrollY_ = 0;
    recalcLayout();
    invalidate();
}

facets::EmptyReason PlayerWindow::searchEmptyReason() const {
    facets::EmptyReason r = facets::explainEmpty(albums_, searchChips_);
    if (r.empty) return r;

    // The chips match something, yet the screen is empty — which normally
    // means the free text is aiming at an ATTRIBUTE rather than a name.
    // Typing "24" while 1990s is applied is a question about bit depth, and
    // answering it with «No matches for "24"» is technically true and useless:
    // the listener is left guessing whether they own no 24-bit at all.
    //
    // So if the suggestion row is already offering that very value and has it
    // blocked, explain THAT instead — the same sentence the listener would
    // have got by accepting it.
    for (const facets::Suggestion& s : searchSuggest_) {
        if (s.enabled) continue;
        std::vector<facets::Chip> probe = searchChips_;
        probe.push_back(s.chip);
        facets::EmptyReason why = facets::explainEmpty(albums_, probe);
        if (why.empty) return why;
    }
    return {};
}

int PlayerWindow::gridHitTest(int x, int y) const {
    if (trackPanelOpen_) return -1;  // grid is hidden behind the album view
    // The Playlists section stands on the SAME geometry (see
    // playlistTileHitTest), so without this a click meant for a playlist tile
    // would resolve to whichever album occupies that cell in the section that
    // is not showing — including from onLButtonDblClk, which hit-tests the
    // grid directly rather than going through the section dispatch.
    if (navSection_ == NavSection::Playlists) return -1;
    if (x < rcGrid_.left || x >= rcGrid_.right || y < rcGrid_.top || y >= rcGrid_.bottom)
        return -1;
    int tileStepX = gridStepX_;
    int tileStepY = gridTileSize_ + gridRowGap_;

    int col = (x - rcGrid_.left - gridPadXpx_) / tileStepX;
    int row = (y - rcGrid_.top - gridPadYpx_ + gridScrollY_) / tileStepY;
    if (col < 0 || col >= gridCols_ || row < 0) return -1;
    int idx = row * gridCols_ + col;
    if (idx >= (int)gridIndices_.size()) return -1;

    // Only the artwork itself is a target — the gaps, the text block below,
    // and the cell margins are dead space. Same art-position math as the
    // grid draw block in drawFrame().
    int artX = rcGrid_.left + gridPadXpx_ + col * tileStepX + (tileStepX - gridArtSize_) / 2;
    int artY = rcGrid_.top + gridPadYpx_ + row * tileStepY - gridScrollY_;
    if (x < artX || x >= artX + gridArtSize_ ||
        y < artY || y >= artY + gridArtSize_) return -1;

    return gridIndices_[idx];  // real albums_ index (search-filtered mapping)
}

// Portable PtInRect replacement — left/top inclusive, right/bottom exclusive
// (same semantics PtInRect always had).
static bool ptInRect(const LayoutRect& r, int x, int y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

// Row index at (x,y) among the rows widgets::drawScrollList cached last frame,
// or -1 if none. Replaces panels::hitTestRows: the visible-row rects the widget
// returns already encode scroll offset and off-screen clipping.
static int hitTestListRows(const std::vector<widgets::ListRow>& rows, int x, int y) {
    for (auto& r : rows)
        if (r.rect.contains((float)x, (float)y)) return r.index;
    return -1;
}

int PlayerWindow::trackPanelHitTest(int x, int y) const {
    if (!trackPanelOpen_ || settingsOpen_) return -1;
    if (x < trackListLeft_ || x >= trackListRight_) return -1;
    if (y < rcTrackPanel_.top || y >= rcTrackPanel_.bottom) return -1;
    if (selectedAlbumIdx_ < 0 || selectedAlbumIdx_ >= (int)albums_.size()) return -1;
    if (trackRowTop_.size() != albums_[selectedAlbumIdx_].tracks.size()) return -1;
    // trackRowTop_ holds each row's scroll-0 window Y (written by the album
    // view draw block); rows scroll with the page. A linear scan — album track
    // counts are in the dozens, and this runs once per mouse event.
    for (int i = 0; i < (int)trackRowTop_.size(); i++) {
        int top = trackRowTop_[i] - trackScrollY_;
        if (y >= top && y < top + trackRowHeight_) return i;
    }
    return -1;
}

// Album index of the "OTHER VERSIONS" thumbnail at (x,y), or -1. The rects
// were recorded by the album view's draw block, already clipped to the
// visible band and already scroll-adjusted — unlike trackRowTop_, which
// stores scroll-0 positions — so this is a plain containment test.
int PlayerWindow::variantTileHitTest(int x, int y) const {
    if (!trackPanelOpen_ || settingsOpen_) return -1;
    for (const auto& [rc, albumIdx] : rcVariantTiles_)
        if (ptInRect(rc, x, y)) return albumIdx;
    return -1;
}

int PlayerWindow::sidebarHitTest(int x, int y) const {
    if (ptInRect(rcNavAlbum_, x, y))  return (int)AlbumTypeFilter::Album;
    if (ptInRect(rcNavEp_, x, y))     return (int)AlbumTypeFilter::Ep;
    if (ptInRect(rcNavSingle_, x, y)) return (int)AlbumTypeFilter::Single;
    if (ptInRect(rcNavCompilation_, x, y)) return (int)AlbumTypeFilter::Compilation;
    if (ptInRect(rcNavLive_, x, y))   return (int)AlbumTypeFilter::Live;
    if (ptInRect(rcNavRemix_, x, y))  return (int)AlbumTypeFilter::Remix;
    if (ptInRect(rcNavPlaylists_, x, y)) return kSidebarPlaylistsHit;
    if (ptInRect(rcNavSettings_, x, y)) return kSidebarSettingsHit;
    // AutoEQ block. hpRows_/hpNoneRc_/hpMoreRc_ are empty in bitperfect mode
    // (and whenever the block didn't fit), so this costs nothing when hidden.
    for (int i = 0; i < (int)hpRows_.size(); i++)
        if (ptInRect(hpRows_[i].rc, x, y)) return kSidebarHpRowBase + i;
    if (ptInRect(hpNoneRc_, x, y)) return kSidebarHpNoneHit;
    if (ptInRect(hpMoreRc_, x, y)) return kSidebarHpMoreHit;
    return -1;
}

int PlayerWindow::transportBtnHitTest(int x, int y) const {
    if (ptInRect(rcBtnPrev_, x, y)) return 0;
    if (ptInRect(rcBtnPlay_, x, y)) return 1;
    if (ptInRect(rcBtnNext_, x, y)) return 2;
    return -1;
}

int PlayerWindow::settingsHitTest(int x, int y) const {
    if (ptInRect(rcSettingsAddFolder_, x, y)) return 0;
    if (ptInRect(rcSettingsManage_, x, y)) return 1;
    if (ptInRect(rcSettingsAudio_, x, y)) return 2;
    if (ptInRect(rcSettingsEq_, x, y)) return 3;
    if (ptInRect(rcSettingsBitperfect_, x, y)) return 4;
    return -1;
}

// ── Mouse handling ───────────────────────────────────────────────────────────

void PlayerWindow::onMouseMove(int x, int y) {
    if (activePanel_ != SettingsPanel::None) { onPanelMouseMove(x, y); return; }
#ifdef _WIN32
    // Win32 doesn't generate a "mouse left the window" event unless you ask
    // for it per-move; Wayland's wl_pointer.leave is unconditional (LinuxHost
    // calls onMouseLeave() straight from that), so there's nothing to arm there.
    if (!mouseTracking_) {
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, host_->nativeHandle(), 0 };
        TrackMouseEvent(&tme);
        mouseTracking_ = true;
    }
#endif

    // Chip / suggestion hover. Both lists are small and only exist while the
    // search is in use, so this runs before the rest rather than being folded
    // into a section-specific branch.
    {
        int oldChip = hoverChipIdx_, oldSugg = hoverSuggestIdx_;
        hoverChipIdx_ = hoverSuggestIdx_ = -1;
        for (size_t i = 0; i < suggestRects_.size(); i++)
            if (ptInRect(suggestRects_[i], x, y)) { hoverSuggestIdx_ = (int)i; break; }
        if (hoverSuggestIdx_ < 0)
            for (size_t i = 0; i < chipRects_.size(); i++)
                if (ptInRect(chipRects_[i], x, y)) { hoverChipIdx_ = (int)i; break; }
        if (oldChip != hoverChipIdx_ || oldSugg != hoverSuggestIdx_) invalidate();
    }

    if (uiMode_ == UiMode::Essential) {
        int oldHoverEssential = hoverEssentialBtn_;
        hoverEssentialBtn_ = essentialHitTest(x, y);
        if (hoverEssentialBtn_ != oldHoverEssential)
            invalidate();
        return;
    }

    int oldHoverAlbum = hoverAlbumIdx_;
    int oldHoverTrack = hoverTrackIdx_;
    int oldHoverVariant = hoverVariantIdx_;
    int oldHoverSidebar = hoverSidebarItem_;
    int oldHoverTransBtn = hoverTransportBtn_;
    int oldHoverSettings = hoverSettingsItem_;
    bool oldHoverDsp = hoverDspBadge_;
    int oldPlRow = plHoverRow_, oldPlTile = plHoverTile_, oldPlTab = plHoverRangeTab_;

    hoverAlbumIdx_ = -1;
    hoverTrackIdx_ = -1;
    hoverVariantIdx_ = -1;
    hoverSidebarItem_ = -1;
    hoverTransportBtn_ = -1;
    hoverSettingsItem_ = -1;
    hoverDspBadge_ = false;
    // Cleared here with the rest, not inside the section's own hit-test, so a
    // pointer that leaves the content area for the sidebar drops this hover
    // too — the same reason every line above it is written this way.
    plHoverRow_ = plHoverTile_ = plHoverRangeTab_ = -1;

    if (ptInRect(rcSidebar_, x, y)) {
        hoverSidebarItem_ = sidebarHitTest(x, y);
    } else if (ptInRect(rcTransport_, x, y)) {
        hoverTransportBtn_ = transportBtnHitTest(x, y);
        hoverDspBadge_ = ptInRect(rcDspBadge_, x, y) != 0;
    } else if (trackPanelOpen_ && !settingsOpen_ && ptInRect(rcTrackPanel_, x, y)) {
        hoverVariantIdx_ = variantTileHitTest(x, y);
        // The strip lives below the track list, but a wide panel puts later
        // columns inside the list's x span — so the more specific rect wins,
        // the same way rcSearch_ beats the sidebar rows in cursorForPoint().
        hoverTrackIdx_   = hoverVariantIdx_ >= 0 ? -1 : trackPanelHitTest(x, y);
    } else if (ptInRect(rcGrid_, x, y)) {
        if (settingsOpen_)
            hoverSettingsItem_ = settingsHitTest(x, y);
        else if (navSection_ == NavSection::Playlists)
            onPlaylistsMouseMove(x, y);
        else
            hoverAlbumIdx_ = gridHitTest(x, y);
    }

    bool changed = (plHoverRow_ != oldPlRow ||
                    plHoverTile_ != oldPlTile ||
                    plHoverRangeTab_ != oldPlTab ||
                    hoverAlbumIdx_ != oldHoverAlbum ||
                    hoverTrackIdx_ != oldHoverTrack ||
                    hoverVariantIdx_ != oldHoverVariant ||
                    hoverSidebarItem_ != oldHoverSidebar ||
                    hoverTransportBtn_ != oldHoverTransBtn ||
                    hoverSettingsItem_ != oldHoverSettings ||
                    hoverDspBadge_ != oldHoverDsp);
    if (changed)
        invalidate();

    applyCursorFor(x, y);
}

// Runs off the hover state onMouseMove just computed plus the few rects that
// are clickable without one (search box, artwork, prose column).
CursorShape PlayerWindow::cursorForPoint(int x, int y) const {
    if (uiMode_ == UiMode::Essential)
        return essentialHitTest(x, y) >= 0 ? CursorShape::Hand : CursorShape::Arrow;

    // Typing surface first: it sits inside the sidebar, whose own rows are
    // hands, so the more specific rect has to win.
    if (ptInRect(rcSearch_, x, y)) return CursorShape::Text;

    if (hoverSidebarItem_   >= 0) return CursorShape::Hand;
    if (hoverTransportBtn_  >= 0) return CursorShape::Hand;
    if (hoverDspBadge_)           return CursorShape::Hand;
    if (hoverTrackIdx_      >= 0) return CursorShape::Hand;
    if (hoverVariantIdx_    >= 0) return CursorShape::Hand;
    if (hoverAlbumIdx_      >= 0) return CursorShape::Hand;
    if (hoverSettingsItem_  >= 0) return CursorShape::Hand;
    if (navSection_ == NavSection::Playlists && !settingsOpen_ &&
        (plHoverRow_ >= 0 || plHoverTile_ >= 0 || plHoverRangeTab_ >= 0))
        return CursorShape::Hand;

    // Artwork opens the fullscreen view; the artist photo does too.
    if (transportArtTex_ != kInvalidTexture && ptInRect(rcTransportArt_, x, y))
        return CursorShape::Hand;
    if (rcArtistImg_.right > rcArtistImg_.left && ptInRect(rcArtistImg_, x, y))
        return CursorShape::Hand;

    // The prose column: not clickable, so NOT a hand — the text cursor says
    // "this is a page" without promising anything happens on click.
    if (trackPanelOpen_ && !settingsOpen_ &&
        rcAlbumText_.right > rcAlbumText_.left && ptInRect(rcAlbumText_, x, y))
        return CursorShape::Text;

    return CursorShape::Arrow;
}

void PlayerWindow::applyCursorFor(int x, int y) {
    CursorShape want = cursorForPoint(x, y);
    if (want == lastCursor_) return;    // both hosts collapse repeats too
    lastCursor_ = want;
    host_->setCursor(want);
}

void PlayerWindow::onMouseLeave() {
    mouseTracking_ = false;
    hoverAlbumIdx_ = -1;
    hoverTrackIdx_ = -1;
    hoverVariantIdx_ = -1;
    hoverSidebarItem_ = -1;
    hoverTransportBtn_ = -1;
    hoverSettingsItem_ = -1;
    hoverDspBadge_ = false;
    hoverEssentialBtn_ = -1;
    plHoverRow_ = plHoverTile_ = plHoverRangeTab_ = -1;
    invalidate();
}

void PlayerWindow::onLButtonDown(int x, int y) {
    if (activePanel_ != SettingsPanel::None) { onPanelClick(x, y); return; }

    if (uiMode_ == UiMode::Essential) {
        int btn = essentialHitTest(x, y);
        if (btn == 0) { onPrev(); return; }
        if (btn == 1) { if (isPlaying_) onStop(); else onPlay(); return; }
        if (btn == 2) { onNext(); return; }
        return;
    }

    // The suggestion dropdown is tested FIRST: it floats over the nav rows,
    // so without this a click meant for "1990s" would land on whichever
    // section happens to sit underneath and change the view instead.
    for (size_t i = 0; i < suggestRects_.size(); i++) {
        if (ptInRect(suggestRects_[i], x, y)) { acceptSuggestion((int)i); return; }
    }
    // Clicking a chip takes it back — the query is a row of undoable choices.
    for (size_t i = 0; i < chipRects_.size(); i++) {
        if (ptInRect(chipRects_[i], x, y)) { removeChip((int)i); return; }
    }

    // Search box focus: clicking it starts typing; clicking anywhere else
    // releases focus (the query itself stays, still filtering).
    {
        bool wasFocused = searchFocused_;
        searchFocused_ = ptInRect(rcSearch_, x, y) != 0;
        if (searchFocused_ != wasFocused) {
            // Focus decides whether the suggestion row exists at all, so the
            // list is rebuilt on both edges: filled on focus (offering the
            // whole menu before a single letter is typed), dropped on blur.
            // recalcLayout() follows because that row occupies real space —
            // the grid's top edge moves with it.
            refreshSuggestions();
            recalcLayout();
            invalidate();
        }
        if (searchFocused_) return;
    }

    // Bitperfect warning banner: click anywhere on it dismisses.
    if (!audioNotice_.empty() && ptInRect(rcAudioNotice_, x, y)) {
        audioNotice_.clear();
        invalidate();
        return;
    }

    // Transport buttons — middle is the combined play-stop toggle, same
    // rule as the VK_SPACE handler.
    int btn = transportBtnHitTest(x, y);
    if (btn == 0) { onPrev(); return; }
    if (btn == 1) { if (isPlaying_) onStop(); else onPlay(); return; }
    if (btn == 2) { onNext(); return; }

    // Transport art -> fullscreen
    if (ptInRect(rcTransportArt_, x, y) && transportArtTex_ != kInvalidTexture) {
        onArtClick();
        return;
    }

    // Artist photo -> fullscreen. Tested before the track list because the
    // photo sits inside the album view's own scroll area. The photo is the
    // ONLY clickable thing in the sidecar block, and it carries no hover
    // treatment: that block reads as a printed page, and a background that
    // lights up under the cursor is the wrong vocabulary for it.
    if (trackPanelOpen_ && !artistImgPath_.empty() &&
        rcArtistImg_.right > rcArtistImg_.left && ptInRect(rcArtistImg_, x, y)) {
        // Straight into the real fullscreen window — the same one the
        // transport thumbnail opens. The first attempt drew the photo as a
        // page overlay inside this window, and it could not work: MSDF text
        // composites in one pass AFTER all geometry (see canvas.hh), so the
        // track list and the bio printed straight through the photo, and they
        // scrolled while it sat still. ArtWindow is a real second surface with
        // its own renderer, so nothing from this window can reach it.
        artWinShowsArtist_ = true;
        ensureArtWindow();
        artWin_.show(artistImgPath_);
        return;
    }

    // "OTHER VERSIONS" thumbnail -> open that variant in this same panel.
    // Tested before the track list for the reason onMouseMove states: on a
    // wide panel the strip's later columns fall inside the list's x span.
    // openAlbumView() already reloads art, sidecar content and scroll, so
    // navigating between variants needs nothing of its own.
    if (trackPanelOpen_ && !settingsOpen_) {
        int variant = variantTileHitTest(x, y);
        if (variant >= 0) {
            openAlbumView(variant);
            return;
        }
    }

    // Sidebar
    if (ptInRect(rcSidebar_, x, y)) {
        int nav = sidebarHitTest(x, y);
        // The headphone sentinels MUST be tested before the `nav >= 0` branch
        // below, which casts whatever it gets into an AlbumTypeFilter.
        if (nav >= kSidebarHpRowBase) {
            int i = nav - kSidebarHpRowBase;
            if (i < (int)hpRows_.size()) {
                int hp = hpRows_[i].headphoneIdx;
                // -1 is the on-trial row: already applied, so clicking it is a
                // no-op rather than a pointless re-assign that would restart
                // its minute and make the row impossible to ever promote.
                if (hp >= 0 && hp < (int)eqHeadphones_.size()) {
                    const auto& h = eqHeadphones_[hp];
                    selectEqProfile({ h.name, h.source, h.form });
                }
            }
        } else if (nav == kSidebarHpNoneHit) {
            clearEqProfile();
        } else if (nav == kSidebarHpMoreHit) {
            onEqSettings();            // clears panelFromSidebar_, as openers do
            // The panel only draws under settingsOpen_, so borrow it — but the
            // listener never asked to BE in Settings, and closeActivePanel()
            // has to hand the view back rather than strand them there.
            // trackPanelOpen_ is left alone on purpose: if they were reading an
            // album, that is where Escape should return them.
            settingsOpen_     = true;
            panelFromSidebar_ = true;
        } else if (nav == kSidebarPlaylistsHit) {
            // The seven content rows all obey one rule: a click takes you to
            // that section's ROOT — its grid of tiles — from wherever you are,
            // unless you are already standing there, in which case it does
            // nothing rather than throw away your scroll position. That
            // "unless" used to read "unless the filter already matches", which
            // meant clicking Singles from inside a single's album view did
            // nothing at all.
            const bool atRoot = !settingsOpen_ &&
                                navSection_ == NavSection::Playlists &&
                                plKind_ == PlaylistKind::None;
            if (!atRoot) {
                openPlaylistSection();
                navForwardValid_ = false;
            }
        } else if (nav == kSidebarSettingsHit) {
            if (!settingsOpen_) { settingsOpen_ = true; navForwardValid_ = false; invalidate(); }
        } else if (nav >= 0) {
            const bool atRoot = !settingsOpen_ && !trackPanelOpen_ &&
                                navSection_ == NavSection::Albums &&
                                albumTypeFilter_ == (AlbumTypeFilter)nav;
            if (!atRoot) {
                settingsOpen_ = false;
                trackPanelOpen_ = false;
                navSection_ = NavSection::Albums;
                albumTypeFilter_ = (AlbumTypeFilter)nav;
                rebuildGridIndices();
                gridScrollY_ = 0;
                navForwardValid_ = false;
                recalcLayout();
                invalidate();
            }
        }
        return;
    }

    // (No album view back button — Escape closes it.)

    // Album view track click
    if (trackPanelOpen_ && !settingsOpen_ && ptInRect(rcTrackPanel_, x, y)) {
        int track = trackPanelHitTest(x, y);
        printf("[Click] Album view track click (%d,%d): hit=%d, tracks=%d\n",
               x, y, track,
               (selectedAlbumIdx_ >= 0 && selectedAlbumIdx_ < (int)albums_.size())
                   ? (int)albums_[selectedAlbumIdx_].tracks.size() : -1);
        fflush(stdout);
        if (track >= 0) {
            // Leaving a playlist: picking a track by hand is exactly what
            // clearQueue()'s contract means by "only from click handlers".
            // Without this, queueActive() stays true for the rest of the
            // session and every later manual pick is logged as
            // StartCause::Playlist — which IS_AFFINITY excludes, so the
            // generated lists would slowly stop learning from real choices.
            // Nothing crashes and nothing looks wrong; only the history does.
            clearQueue();
            currentAlbum_ = selectedAlbumIdx_;
            currentTrack_ = track;
            onPlay();
        }
        return;
    }

    // Playlists section — the same content area, the same two levels (tile
    // grid, then one list opened) as the album grid immediately below.
    if (!settingsOpen_ && navSection_ == NavSection::Playlists && ptInRect(rcGrid_, x, y)) {
        onPlaylistsClick(x, y);
        return;
    }

    // Grid (albums view)
    if (!settingsOpen_ && !trackPanelOpen_ && ptInRect(rcGrid_, x, y)) {
        int idx = gridHitTest(x, y);
        if (idx >= 0) openAlbumView(idx);
        return;
    }

    // Settings page
    if (settingsOpen_ && ptInRect(rcGrid_, x, y)) {
        int sett = settingsHitTest(x, y);
        if (sett == 0) onAddFolder();
        if (sett == 1) onManageFolders();
        if (sett == 2) onAudioSettings();
        if (sett == 3) onEqSettings();
        if (sett == 4) toggleBitperfectMode();
        return;
    }
}

void PlayerWindow::onLButtonDblClk(int x, int y) {
    if (activePanel_ != SettingsPanel::None) return;  // no double-click behavior inside panels

    // Double-click the transport thumbnail closes the fullscreen art view.
    // Single-click already opens it (onLButtonDown -> onArtClick), so a fast
    // double-click nets an open-then-close flash — harmless, same layering
    // the grid-tile dblclk below already has (click opens album view, dblclk
    // also plays the first track).
    if (ptInRect(rcTransportArt_, x, y) && artWin_.isVisible()) {
        artWin_.hide();
        return;
    }

    // Double-click on grid tile: play first track
    if (!settingsOpen_ && !trackPanelOpen_ && ptInRect(rcGrid_, x, y)) {
        int idx = gridHitTest(x, y);
        if (idx >= 0) {
            // Leaving a playlist: picking a track by hand is exactly what
            // clearQueue()'s contract means by "only from click handlers".
            // Without this, queueActive() stays true for the rest of the
            // session and every later manual pick is logged as
            // StartCause::Playlist — which IS_AFFINITY excludes, so the
            // generated lists would slowly stop learning from real choices.
            // Nothing crashes and nothing looks wrong; only the history does.
            clearQueue();
            openAlbumView(idx);
            currentAlbum_ = idx;
            currentTrack_ = 0;
            onPlay();
        }
        return;
    }

    // Double-click on track panel: play that track
    if (trackPanelOpen_ && ptInRect(rcTrackPanel_, x, y)) {
        int track = trackPanelHitTest(x, y);
        if (track >= 0) {
            // Leaving a playlist: picking a track by hand is exactly what
            // clearQueue()'s contract means by "only from click handlers".
            // Without this, queueActive() stays true for the rest of the
            // session and every later manual pick is logged as
            // StartCause::Playlist — which IS_AFFINITY excludes, so the
            // generated lists would slowly stop learning from real choices.
            // Nothing crashes and nothing looks wrong; only the history does.
            clearQueue();
            currentAlbum_ = selectedAlbumIdx_;
            currentTrack_ = track;
            onPlay();
        }
        return;
    }
}

// x,y are client-relative (the host converts from whatever coordinate space
// its own wheel event delivers — Windows' WM_MOUSEWHEEL is screen-relative
// and gets ScreenToClient()'d in windows_host.cc before calling this;
// Wayland's pointer coords are already surface-relative).
void PlayerWindow::onMouseWheel(int x, int y, int delta) {
    if (activePanel_ != SettingsPanel::None) { onPanelWheel(x, y, delta); return; }
    if (trackPanelOpen_ && !settingsOpen_ && ptInRect(rcTrackPanel_, x, y)) {
        // The album view scrolls as one page; its content height is
        // measured by the draw block (albumViewContentH_).
        trackScrollY_ -= delta;
        int panelH = rcTrackPanel_.bottom - rcTrackPanel_.top;
        trackScrollY_ = std::clamp(trackScrollY_, 0,
                                   std::max(0, albumViewContentH_ - panelH));
        invalidate();
        return;
    }

    // An opened playlist scrolls its own list; the tile grid falls through to
    // the grid scroll below, which is sized for it (see recalcLayout).
    if (!settingsOpen_ && navSection_ == NavSection::Playlists &&
        plKind_ != PlaylistKind::None && ptInRect(rcGrid_, x, y)) {
        int listH = plListArea_.bottom - plListArea_.top;
        int contentH = (int)plEntries_.size() * plRowH_;
        plScrollY_ = std::clamp(plScrollY_ - delta, 0, std::max(0, contentH - listH));
        invalidate();
        return;
    }

    if (!trackPanelOpen_ && ptInRect(rcGrid_, x, y)) {
        gridScrollY_ -= delta;
        int gridH = rcGrid_.bottom - rcGrid_.top;
        gridScrollY_ = std::clamp(gridScrollY_, 0, std::max(0, gridTotalHeight_ - gridH));
        invalidate();
    }
}

// ── Prev / Next ──────────────────────────────────────────────────────────────

void PlayerWindow::onNext() {
    if (currentAlbum_ < 0) return;
    // Where "next" lands, computed the same way auto-advance computes it: past
    // the last track we continue into the next album OF THIS SECTION. Skipping
    // that step used to dead-end the button at every album's end — and in
    // Singles, where every album holds one track, that meant Next did nothing
    // at all. Resolved under the lock, then released: onPlay() takes it too.
    int wantedAlbum, wantedTrack;
    int wantedQueuePos = -1;       // >= 0 only on the playlist path
    {
        std::lock_guard<std::mutex> lk(albumsMu_);
        if (queueActiveLocked()) {
            // A playlist crosses albums, so "next" is the next ENTRY, not the
            // next track of this album. At the end it stops, like the section
            // walk below does.
            const int next = queuePos_ + 1;
            if (next >= (int)queue_.size()) return;
            wantedQueuePos = next;
            wantedAlbum    = queue_[next].album;
            wantedTrack    = queue_[next].track;
        } else {
            if (currentAlbum_ >= (int)albums_.size()) return;
            wantedAlbum = currentAlbum_;
            wantedTrack = currentTrack_ + 1;
            if (wantedTrack >= (int)albums_[wantedAlbum].tracks.size()) {
                wantedAlbum = nextAlbumInSection(wantedAlbum);
                wantedTrack = 0;
            }
        }
    }
    if (wantedAlbum < 0) return;   // end of the section

    // Seamless path: if we're playing, the prepared nextDecoder_ matches the
    // requested track, and its (sampleRate, channels) match the running USB
    // output, hand off via the gapless coordinator. The output stream stays
    // alive — no stop()/configure()/start() cycle, no working-set re-lock
    // dance, no cold-start transient. prepareNextTrack() precomputes exactly
    // this pair, so the handoff now also covers a jump across an album
    // boundary — single to single included.
    //
    // Decoder::stop() does not fire the done callback (only natural EOF
    // does), so we signal gaplessSignal_ ourselves after stopping the
    // current decoder. flush() drops the ~3 s of stale tail still queued in
    // the ring so the user actually hears the next track promptly.
    if (isPlaying_ && active_ && output_ &&
        nextAlbum_ == wantedAlbum && nextTrack_ == wantedTrack) {
        Decoder* incoming = (active_ == &decoder_) ? &nextDecoder_ : &decoder_;
        if (incoming->sampleRate() == output_->getConfiguredRate() &&
            incoming->channels()   == output_->getConfiguredChannels()) {
            // Banked HERE, before the handoff, because this path never
            // returns to the code below: the coordinator picks it up and
            // applyTrackMetadata() does the rest. Without this the listener's
            // skip would be logged as an ordinary gapless advance — and
            // "tracks you bail out of" is exactly what that would hide.
            flushTrackStats(EndCause::Next);
            // Manual normally — the listener pressed Next. But inside a
            // playlist the play still CAME OUT of the playlist, and letting it
            // log as Manual would feed the very ranking the list was built
            // from. Choosing a row is not the same as earning one.
            gaplessStartCause_ = queueActive() ? StartCause::Playlist
                                               : StartCause::Manual;
            active_->stop();
            output_->flush();
            {
                std::lock_guard<std::mutex> lk(gaplessMu_);
                gaplessSignal_ = true;
            }
            gaplessCv_.notify_one();
            return;
        }
    }

    flushTrackStats(EndCause::Next);
    if (wantedQueuePos >= 0) {
        std::lock_guard<std::mutex> lk(albumsMu_);
        queuePos_ = wantedQueuePos;
    }
    currentAlbum_ = wantedAlbum;
    currentTrack_ = wantedTrack;
    onPlay();
}

void PlayerWindow::onPrev() {
    if (currentAlbum_ < 0 || currentTrack_ < 0) return;
    if (seekPosMs_ > 3000) {
        onSeek(0);   // rebases playedFrames_ to the current track's start
        return;      // same track restarted — not a track change, nothing to bank
    }
    // Past this point every path changes track, so the outgoing one is banked
    // once here rather than at each of the two exits below.
    flushTrackStats(EndCause::Prev);

    // A playlist walks its own entries backwards, ignoring album boundaries
    // entirely — the previous entry may live in another album, or in the same
    // one two tracks up. At the head it stops, mirroring onNext at the tail.
    bool onQueue = false;
    {
        std::lock_guard<std::mutex> lk(albumsMu_);
        if (queueActiveLocked()) {
            onQueue = true;
            if (queuePos_ <= 0) return;
            const int prev = queuePos_ - 1;
            const int a = queue_[prev].album, t = queue_[prev].track;
            if (a < 0 || a >= (int)albums_.size() ||
                t < 0 || t >= (int)albums_[a].tracks.size()) return;
            queuePos_     = prev;
            currentAlbum_ = a;
            currentTrack_ = t;
        }
    }
    if (onQueue) { onPlay(); return; }

    if (currentTrack_ > 0) {
        currentTrack_--;
        onPlay();
        return;
    }
    // At an album's first track, step back into the previous album of this
    // section, landing on its LAST track — the mirror of onNext(). Same
    // lock-then-release shape, for the same reason.
    int prevAlbum, prevTrack;
    {
        std::lock_guard<std::mutex> lk(albumsMu_);
        prevAlbum = prevAlbumInSection(currentAlbum_);
        if (prevAlbum < 0) return;   // start of the section
        prevTrack = (int)albums_[prevAlbum].tracks.size() - 1;
    }
    currentAlbum_ = prevAlbum;
    currentTrack_ = prevTrack;
    onPlay();
}

// ── Bitperfect / DSP mode toggle ─────────────────────────────────────────────

void PlayerWindow::toggleBitperfectMode() {
    bool newMode = !bitperfectMode_.load();
    bitperfectMode_.store(newMode);
    db_.saveSetting("audio_mode", newMode ? "bitperfect" : "reference_eq");

    // Both directions take effect on the next track started, not mid-play.
    // (Don't clear the EQ engine here: the running Reference EQ callback checks
    // isActive() every chunk, so clearing would kill EQ live and make the switch
    // asymmetric. The bit-perfect branch of onPlay clears EQ itself next track.)
    printf("[Audio] Switched to %s mode (applies on next track)\n",
           newMode ? "BITPERFECT" : "REFERENCE EQ");
    invalidate();
}

// ── Settings panels (Phase 7) ────────────────────────────────────────────────
// vk_canvas-native replacements for the four native Win32 dialogs (Manage
// Folders / Audio Settings / EQ Settings / SHBrowseForFolderW), identical on
// both platforms — see panels/settings_panels.hh's header comment for why
// (full-page overlay, not a modal popup: Wayland has no owned-window
// primitive to build a real modal on).

void PlayerWindow::closeActivePanel() {
    activePanel_ = SettingsPanel::None;
    // Swapping headphones is a hot action, not a trip into configuration: the
    // quick-switcher borrows the settings overlay to show the profile list, so
    // closing has to give the previous view back. Without this, Escape leaves
    // the listener parked on the Settings page they never asked for.
    if (panelFromSidebar_) {
        settingsOpen_     = false;
        panelFromSidebar_ = false;
    }
    invalidate();
}

void PlayerWindow::drawActivePanel(Canvas& canvas, const LayoutRect& area) {
    LayoutRect* closeRc = nullptr;
    bool hoverClose = false;
    switch (activePanel_) {
    case SettingsPanel::ManageFolders:
        drawManageFolders(canvas, area);
        closeRc = &mfCloseRc_; hoverClose = mfHoverClose_;
        break;
    case SettingsPanel::AudioSettings:
        drawAudioSettings(canvas, area);
        closeRc = &asCloseRc_; hoverClose = asHoverClose_;
        break;
    case SettingsPanel::EqSettings:
        drawEqSettings(canvas, area);
        closeRc = &eqCloseRc_; hoverClose = eqHoverClose_;
        break;
    case SettingsPanel::FolderPicker:
        drawFolderPicker(canvas, area);
        closeRc = &fpCloseRc_; hoverClose = fpHoverClose_;
        break;
    case SettingsPanel::None:
        break;
    }
    if (closeRc)
        panels::drawButton(canvas, *closeRc, "Close", hoverClose, metrics_.text.body);
}

void PlayerWindow::onPanelMouseMove(int x, int y) {
    bool changed = false;
    switch (activePanel_) {
    case SettingsPanel::ManageFolders: {
        bool hc = ptInRect(mfCloseRc_, x, y);  if (hc != mfHoverClose_)  { mfHoverClose_  = hc; changed = true; }
        bool hr = ptInRect(mfBtnRemove_, x, y); if (hr != mfHoverRemove_) { mfHoverRemove_ = hr; changed = true; }
        bool hd = ptInRect(mfBtnDone_, x, y);   if (hd != mfHoverDone_)   { mfHoverDone_   = hd; changed = true; }
        int row = hitTestListRows(mfListRows_, x, y);
        if (row != mfHoverRow_) { mfHoverRow_ = row; changed = true; }
        break;
    }
    case SettingsPanel::AudioSettings: {
        bool hc = ptInRect(asCloseRc_, x, y); if (hc != asHoverClose_) { asHoverClose_ = hc; changed = true; }
        bool ha = ptInRect(asBtnApply_, x, y); if (ha != asHoverApply_) { asHoverApply_ = ha; changed = true; }
        int hb = -1;
        for (int i = 0; i < (int)asBackendRowRects_.size(); i++)
            if (ptInRect(asBackendRowRects_[i], x, y)) { hb = i; break; }
        if (hb != asHoverBackendRow_) { asHoverBackendRow_ = hb; changed = true; }

        int hdv = hitTestListRows(asDeviceListRows_, x, y);
        if (hdv != asHoverDeviceRow_) { asHoverDeviceRow_ = hdv; changed = true; }
#ifdef _WIN32
        AudioBackend sel = asBackendOptions_.empty() ? AudioBackend::Usb : asBackendOptions_[asBackendSelIdx_];
        int hm = -1;
        if (sel == AudioBackend::Wasapi)
            for (int i = 0; i < 2; i++) if (ptInRect(asModeRows_[i], x, y)) { hm = i; break; }
        if (hm != asHoverModeRow_) { asHoverModeRow_ = hm; changed = true; }
#endif
        break;
    }
    case SettingsPanel::EqSettings: {
        bool hc = ptInRect(eqCloseRc_, x, y); if (hc != eqHoverClose_) { eqHoverClose_ = hc; changed = true; }
        bool ha = ptInRect(eqBtnAssign_, x, y); if (ha != eqHoverAssign_) { eqHoverAssign_ = ha; changed = true; }
        bool hcl = ptInRect(eqBtnClear_, x, y); if (hcl != eqHoverClear_) { eqHoverClear_ = hcl; changed = true; }
        bool hp = ptInRect(eqBtnPin_, x, y); if (hp != eqHoverPin_) { eqHoverPin_ = hp; changed = true; }
        bool hr = ptInRect(eqBtnRemove_, x, y); if (hr != eqHoverRemove_) { eqHoverRemove_ = hr; changed = true; }
        bool htm = ptInRect(eqTabMine_, x, y); if (htm != eqHoverTabMine_) { eqHoverTabMine_ = htm; changed = true; }
        bool hta = ptInRect(eqTabAll_, x, y); if (hta != eqHoverTabAll_) { eqHoverTabAll_ = hta; changed = true; }
        int row = hitTestListRows(eqListRows_, x, y);
        if (row != eqHoverRow_) { eqHoverRow_ = row; changed = true; }
        break;
    }
    case SettingsPanel::FolderPicker: {
        bool hc = ptInRect(fpCloseRc_, x, y);   if (hc != fpHoverClose_)  { fpHoverClose_  = hc; changed = true; }
        bool hs = ptInRect(fpBtnSelect_, x, y); if (hs != fpHoverSelect_) { fpHoverSelect_ = hs; changed = true; }
        bool ha = ptInRect(fpBtnCancel_, x, y); if (ha != fpHoverCancel_) { fpHoverCancel_ = ha; changed = true; }
        int row = hitTestListRows(fpListRows_, x, y);
        if (row != fpHoverRow_) { fpHoverRow_ = row; changed = true; }
        break;
    }
    case SettingsPanel::None:
        break;
    }
    if (changed) invalidate();
}

void PlayerWindow::onPanelClick(int x, int y) {
    switch (activePanel_) {
    case SettingsPanel::ManageFolders: {
        if (ptInRect(mfCloseRc_, x, y) || ptInRect(mfBtnDone_, x, y)) {
            if (mfChanged_) { watcher_.unwatchAll(); setupWatchers(); startBackgroundScan(); }
            closeActivePanel();
            return;
        }
        if (ptInRect(mfBtnRemove_, x, y)) {
            if (mfSelectedRow_ >= 0 && mfSelectedRow_ < (int)mfRoots_.size()) {
                db_.removeMusicRoot(mfRoots_[mfSelectedRow_]);
                mfRoots_.erase(mfRoots_.begin() + mfSelectedRow_);
                mfSelectedRow_ = -1;
                mfChanged_ = true;
                invalidate();
            }
            return;
        }
        int row = hitTestListRows(mfListRows_, x, y);
        if (row >= 0) { mfSelectedRow_ = row; invalidate(); }
        return;
    }
    case SettingsPanel::AudioSettings: {
        if (ptInRect(asCloseRc_, x, y)) { closeActivePanel(); return; }  // cancel, no apply
        for (int i = 0; i < (int)asBackendRowRects_.size(); i++)
            if (ptInRect(asBackendRowRects_[i], x, y)) {
                asBackendSelIdx_ = i;
                asDeviceScrollY_ = 0;
                invalidate();
                return;
            }

        AudioBackend sel = asBackendOptions_.empty() ? AudioBackend::Usb : asBackendOptions_[asBackendSelIdx_];
        int devRow = hitTestListRows(asDeviceListRows_, x, y);
        if (sel == AudioBackend::Usb) {
            if (devRow >= 0) { asUsbSel_ = devRow; invalidate(); return; }
        }
#ifdef _WIN32
        else if (sel == AudioBackend::Wasapi) {
            if (devRow >= 0) { asWasapiSel_ = devRow; invalidate(); return; }
            for (int i = 0; i < 2; i++)
                if (ptInRect(asModeRows_[i], x, y)) { asExclusive_ = (i == 1); invalidate(); return; }
        }
#else
#ifdef MATRIX_HAVE_ALSA
        else if (sel == AudioBackend::Alsa) {
            if (devRow >= 0) { asAlsaSel_ = devRow; invalidate(); return; }
        }
#endif
#ifdef MATRIX_HAVE_JACK
        else if (sel == AudioBackend::Jack) {
            if (devRow >= 0) { asJackSel_ = devRow; invalidate(); return; }
        }
#endif
#endif
        if (ptInRect(asBtnApply_, x, y)) { applyAudioSettingsPanel(); return; }
        return;
    }
    case SettingsPanel::EqSettings: {
        if (ptInRect(eqCloseRc_, x, y)) { closeActivePanel(); return; }
        bool wasFocused = eqSearchFocused_;
        eqSearchFocused_ = ptInRect(eqSearchRc_, x, y);
        if (eqSearchFocused_ != wasFocused) invalidate();
        if (eqSearchFocused_) return;

        if (ptInRect(eqTabMine_, x, y) || ptInRect(eqTabAll_, x, y)) {
            bool mine = ptInRect(eqTabMine_, x, y);
            if (mine != eqShowMine_) {
                eqShowMine_ = mine;
                eqSelectedRow_ = -1;   // the row indices mean something else now
                eqScrollY_ = 0;
                eqRefilter();
                invalidate();
            }
            return;
        }
        // Selecting a profile goes through the one shared path — see
        // selectEqProfile(). It writes the assignment, applies the
        // coefficients live, and restarts the trial clock together.
        if (ptInRect(eqBtnAssign_, x, y)) {
            if (eqSelectedRow_ >= 0 && eqSelectedRow_ < (int)eqFilteredIndices_.size()) {
                ensureEqProfiles();   // indexes getAll() directly, below
                auto& p = eqProfiles_.getAll()[eqFilteredIndices_[eqSelectedRow_]];
                selectEqProfile({ p.name, p.source, p.form });
            }
            return;
        }
        if (ptInRect(eqBtnPin_, x, y)) {
            if (const EqHeadphone* sel = eqSelectedHeadphone()) {
                db_.setEqHeadphonePinned(sel->name, sel->source, sel->form, !sel->pinned);
                reloadEqHeadphones();   // invalidates sel
                invalidate();
            }
            return;
        }
        if (ptInRect(eqBtnRemove_, x, y)) {
            if (const EqHeadphone* sel = eqSelectedHeadphone()) {
                db_.removeEqHeadphone(sel->name, sel->source, sel->form);
                reloadEqHeadphones();
                // Removing the pair that is currently playing doesn't stop the
                // EQ — the assignment is untouched, so it drops back to being
                // on trial and can earn its row again.
                eqRefilter();
                eqSelectedRow_ = -1;
                invalidate();
            }
            return;
        }
        if (ptInRect(eqBtnClear_, x, y)) {
            clearEqProfile();
            return;
        }
        int row = hitTestListRows(eqListRows_, x, y);
        if (row >= 0) { eqSelectedRow_ = row; invalidate(); }
        return;
    }
    case SettingsPanel::FolderPicker: {
        if (ptInRect(fpCloseRc_, x, y) || ptInRect(fpBtnCancel_, x, y)) { closeActivePanel(); return; }
        if (ptInRect(fpBtnSelect_, x, y)) {
            commitAddFolder(fpCurrentDir_);
            closeActivePanel();
            return;
        }
        int row = hitTestListRows(fpListRows_, x, y);
        if (row < 0) return;
        if (fpHasParent_ && row == 0) {
            fpLoadDir(std::filesystem::path(fpCurrentDir_).parent_path().string());
        } else {
            int idx = fpHasParent_ ? row - 1 : row;
            fpLoadDir((std::filesystem::path(fpCurrentDir_) / fpEntries_[idx]).string());
        }
        invalidate();
        return;
    }
    case SettingsPanel::None:
        break;
    }
}

void PlayerWindow::onPanelWheel(int x, int y, int delta) {
    switch (activePanel_) {
    case SettingsPanel::ManageFolders: {
        int listH = mfListArea_.bottom - mfListArea_.top;
        int contentH = (int)((float)mfRoots_.size() * panelRowH());
        mfScrollY_ = std::clamp(mfScrollY_ - delta, 0, std::max(0, contentH - listH));
        invalidate();
        return;
    }
    case SettingsPanel::EqSettings: {
        int listH = eqListArea_.bottom - eqListArea_.top;
        int contentH = (int)((float)eqFilteredIndices_.size() * panelRowH());
        eqScrollY_ = std::clamp(eqScrollY_ - delta, 0, std::max(0, contentH - listH));
        invalidate();
        return;
    }
    case SettingsPanel::FolderPicker: {
        int rowCount = (int)fpEntries_.size() + (fpHasParent_ ? 1 : 0);
        int listH = fpListArea_.bottom - fpListArea_.top;
        int contentH = (int)((float)rowCount * panelRowH());
        fpScrollY_ = std::clamp(fpScrollY_ - delta, 0, std::max(0, contentH - listH));
        invalidate();
        return;
    }
    case SettingsPanel::AudioSettings: {
        (void)x; (void)y;
        AudioBackend sel = asBackendOptions_.empty() ? AudioBackend::Usb : asBackendOptions_[asBackendSelIdx_];
        int rowCount = 0;
        switch (sel) {
        case AudioBackend::Usb: rowCount = (int)asUsbDevices_.size(); break;
#ifdef _WIN32
        case AudioBackend::Wasapi: rowCount = (int)asWasapiDevices_.size() + 1; break;
#else
#ifdef MATRIX_HAVE_ALSA
        case AudioBackend::Alsa: rowCount = (int)asAlsaDevices_.size() + 1; break;
#endif
#ifdef MATRIX_HAVE_JACK
        case AudioBackend::Jack: rowCount = (int)asJackPorts_.size() + 1; break;
#endif
#endif
        default: break;
        }
        int listH = asDeviceListArea_.bottom - asDeviceListArea_.top;
        int contentH = (int)((float)rowCount * panelRowH());
        asDeviceScrollY_ = std::clamp(asDeviceScrollY_ - delta, 0, std::max(0, contentH - listH));
        invalidate();
        return;
    }
    case SettingsPanel::None:
        (void)x; (void)y;
        return;
    }
}

// ── Manage Folders panel ─────────────────────────────────────────────────────

void PlayerWindow::onManageFolders() {
    mfRoots_ = db_.loadMusicRoots();
    mfSelectedRow_ = -1;
    mfHoverRow_ = -1;
    mfScrollY_ = 0;
    mfChanged_ = false;
    mfHoverClose_ = mfHoverRemove_ = mfHoverDone_ = false;
    activePanel_ = SettingsPanel::ManageFolders;
    invalidate();
}

void PlayerWindow::drawManageFolders(Canvas& canvas, const LayoutRect& area) {
    LayoutRect content = panels::drawHeader(canvas, area, "Music Folders", metrics_.scale, metrics_.text.header, mfCloseRc_);
    float pad = metrics_.space(SP_LG);
    float btnH = metrics_.space(58.0f);

    LayoutRect listArea = { content.left, (int)(content.top + pad),
                            content.right, (int)(content.bottom - (btnH + pad * 2)) };
    mfListArea_ = listArea;
    float mfRowH = panelRowH();
    mfListRows_ = widgets::drawScrollList(canvas, toRect(listArea), mfRoots_,
                                          mfSelectedRow_, (float)mfScrollY_, mfRowH,
                                          mfHoverRow_, widgets::kTextFree, matrixListStyle());
    panels::drawScrollbar(canvas, listArea, (int)((float)mfRoots_.size() * mfRowH), mfScrollY_, metrics_.scale);
    if (mfRoots_.empty()) {
        Rect a = toRect(listArea);
        canvas.textStyled("No music folders added yet.", a.x + metrics_.space(22.0f), a.y + metrics_.space(22.0f),
                          metrics_.text.body, toColor(CLR_TEXT_DIM), FontStyle::Italic);
    }

    float btnW = metrics_.space(277.0f);
    int by = (int)(content.bottom - (btnH + pad));
    auto mfRects = panels::layoutEdgePair(
        content, pad, btnW, btnW,
        metrics_.space(panels::kMinActionBtnW), metrics_.space(SP_MD), by, (int)btnH);
    mfBtnRemove_ = mfRects.first;
    mfBtnDone_   = mfRects.second;
    panels::drawButton(canvas, mfBtnRemove_, "Remove Selected", mfHoverRemove_, metrics_.text.body);
    panels::drawButton(canvas, mfBtnDone_, "Done", mfHoverDone_, metrics_.text.body, true);
}

// ── Audio Settings panel ─────────────────────────────────────────────────────

void PlayerWindow::onAudioSettings() {
    asUsbDevices_ = UsbAudioDriver::enumerateUsbAudioDevices();
    asUsbSel_ = 0;
    {
        auto savedVid = db_.loadSetting("usb_vid");
        auto savedPid = db_.loadSetting("usb_pid");
        if (!savedVid.empty() && !savedPid.empty()) {
            uint16_t sv = (uint16_t)strtoul(savedVid.c_str(), nullptr, 16);
            uint16_t sp = (uint16_t)strtoul(savedPid.c_str(), nullptr, 16);
            for (int i = 0; i < (int)asUsbDevices_.size(); i++)
                if (asUsbDevices_[i].vid == sv && asUsbDevices_[i].pid == sp) { asUsbSel_ = i; break; }
        }
    }

    asBackendOptions_.clear();
    asBackendOptions_.push_back(AudioBackend::Usb);
#ifdef _WIN32
    asBackendOptions_.push_back(AudioBackend::Wasapi);
    asWasapiDevices_ = WasapiOutput::enumerateDevices();
    asWasapiSel_ = 0;
    {
        auto savedId = db_.loadSetting("wasapi_device_id");
        for (int i = 0; i < (int)asWasapiDevices_.size(); i++)
            if (wideToUtf8(asWasapiDevices_[i].id) == savedId) { asWasapiSel_ = i + 1; break; }
    }
    asExclusive_ = (db_.loadSetting("wasapi_mode") == "exclusive");
#else
#ifdef MATRIX_HAVE_ALSA
    asBackendOptions_.push_back(AudioBackend::Alsa);
    asAlsaDevices_ = AlsaOutput::enumerateDevices();
    asAlsaSel_ = 0;
    {
        auto savedId = db_.loadSetting("alsa_device_id");
        for (int i = 0; i < (int)asAlsaDevices_.size(); i++)
            if (asAlsaDevices_[i].deviceId == savedId) { asAlsaSel_ = i + 1; break; }
    }
#endif
#ifdef MATRIX_HAVE_JACK
    asBackendOptions_.push_back(AudioBackend::Jack);
    {
        // Ask the LIVE client when there already is one. A second libjack
        // client in the same process is the condition jack2 handles worst, and
        // there is no reason to open one to read a graph the playback client is
        // already attached to.
        if (audioBackend_ == AudioBackend::Jack && output_) {
            asJackPorts_ = static_cast<JackOutput*>(output_.get())->enumeratePorts();
        } else {
            // Throwaway client purely to query the graph — closed HERE, not
            // left to the end of scope, so the one call that can touch a dead
            // handle is written down where it happens.
            JackOutput probe;
            asJackPorts_ = probe.enumeratePorts();
            probe.close();
        }
    }
    asJackSel_ = 0;
    {
        auto savedPort = db_.loadSetting("jack_port");
        for (int i = 0; i < (int)asJackPorts_.size(); i++)
            if (asJackPorts_[i].portName == savedPort) { asJackSel_ = i + 1; break; }
    }
#endif
#endif

    std::string backend = db_.loadSetting("audio_backend");
    asBackendSelIdx_ = 0;
    for (int i = 0; i < (int)asBackendOptions_.size(); i++) {
        AudioBackend b = asBackendOptions_[i];
        if ((b == AudioBackend::Wasapi && backend == "wasapi") ||
            (b == AudioBackend::Alsa   && backend == "alsa")   ||
            (b == AudioBackend::Jack   && backend == "jack")) {
            asBackendSelIdx_ = i;
            break;
        }
    }

    asHoverBackendRow_ = -1;
    asHoverDeviceRow_  = -1;
    asDeviceScrollY_ = 0;
    asHoverClose_ = asHoverApply_ = false;
#ifdef _WIN32
    asHoverModeRow_ = -1;
#endif
    activePanel_ = SettingsPanel::AudioSettings;
    invalidate();
}

void PlayerWindow::drawAudioSettings(Canvas& canvas, const LayoutRect& area) {
    LayoutRect content = panels::drawHeader(canvas, area, "Audio Output Settings", metrics_.scale, metrics_.text.header, asCloseRc_);
    Rect c = toRect(content);
    float pad = metrics_.space(SP_LG);
    float y = c.y + pad;

    canvas.textStyled("Output backend:", c.x + pad, y, metrics_.text.body, toColor(CLR_TEXT_DIM), FontStyle::Roman);
    y += metrics_.text.body * 1.8f;

    float rowH = metrics_.space(55.0f);
    asBackendRowRects_.assign(asBackendOptions_.size(), LayoutRect{});
    for (int i = 0; i < (int)asBackendOptions_.size(); i++) {
        LayoutRect rc = { (int)(c.x + pad), (int)y, (int)(c.x + c.w - pad), (int)(y + rowH) };
        bool sel = (i == asBackendSelIdx_);
        Rect hit = widgets::drawRadioRow(canvas, toRect(rc), sel, (i == asHoverBackendRow_),
                                         backendDisplayName(asBackendOptions_[i]),
                                         widgets::kTextFree, matrixRadioStyle());
        asBackendRowRects_[i] = toLayoutRect(hit);
        y += rowH;
    }
    y += metrics_.space(SP_MD);

    AudioBackend sel = asBackendOptions_.empty() ? AudioBackend::Usb : asBackendOptions_[asBackendSelIdx_];

    // The device list hugs its content and grows into whatever space is left
    // above the bottom-docked Apply button, instead of a fixed 6-row window.
    // With 10+ ALSA devices that fixed height hid everything past row 6 behind
    // a scroll with no affordance — drawScrollList clips silently and draws no
    // scrollbar, so a DAC in row 7 simply looked absent.
    float btnH = metrics_.space(58.0f);
    float listTop = y + metrics_.text.body * 1.6f;   // every branch draws its label first
    float listBottomLimit = (float)content.bottom - pad - btnH - pad;
#ifdef _WIN32
    if (sel == AudioBackend::Wasapi)             // the Mode radios sit below the list
        listBottomLimit -= metrics_.space(SP_MD) + metrics_.text.body * 1.6f + 2.0f * rowH + metrics_.space(SP_MD);
#endif
    float listRowH = panelRowH();
    // 3-row floor keeps the empty-state messages below readable.
    auto listHeightFor = [&](int rowCount) {
        float minH  = 3.0f * listRowH;
        float avail = std::max(listBottomLimit - listTop, minH);
        return std::clamp((float)rowCount * listRowH, minH, avail);
    };

    if (sel == AudioBackend::Usb) {
        canvas.textStyled("USB DAC:", c.x + pad, y, metrics_.text.body, toColor(CLR_TEXT_DIM), FontStyle::Roman);
        y += metrics_.text.body * 1.6f;
        std::vector<std::string> labels;
        for (auto& d : asUsbDevices_) labels.push_back(d.name);
        float listH = listHeightFor((int)labels.size());
        asDeviceListArea_ = { (int)(c.x + pad), (int)y, (int)(c.x + c.w - pad), (int)(y + listH) };
        asDeviceListRows_ = widgets::drawScrollList(canvas, toRect(asDeviceListArea_), labels,
                                                    asUsbSel_, (float)asDeviceScrollY_, listRowH,
                                                    asHoverDeviceRow_, widgets::kTextFree, matrixListStyle());
        panels::drawScrollbar(canvas, asDeviceListArea_, (int)((float)labels.size() * listRowH),
                              asDeviceScrollY_, metrics_.scale);
        if (labels.empty()) {
            Rect a = toRect(asDeviceListArea_);
            canvas.textStyled("No USB audio devices found.", a.x + metrics_.space(22.0f), a.y + metrics_.space(22.0f),
                              metrics_.text.body, toColor(CLR_TEXT_DIM), FontStyle::Italic);
        }
        y += listH + metrics_.space(SP_MD);
    }
#ifdef _WIN32
    else if (sel == AudioBackend::Wasapi) {
        canvas.textStyled("Device:", c.x + pad, y, metrics_.text.body, toColor(CLR_TEXT_DIM), FontStyle::Roman);
        y += metrics_.text.body * 1.6f;
        std::vector<std::string> labels;
        labels.push_back("(Default device)");
        for (auto& d : asWasapiDevices_) labels.push_back(wideToUtf8(d.name));
        float listH = listHeightFor((int)labels.size());
        asDeviceListArea_ = { (int)(c.x + pad), (int)y, (int)(c.x + c.w - pad), (int)(y + listH) };
        asDeviceListRows_ = widgets::drawScrollList(canvas, toRect(asDeviceListArea_), labels,
                                                    asWasapiSel_, (float)asDeviceScrollY_, listRowH,
                                                    asHoverDeviceRow_, widgets::kTextFree, matrixListStyle());
        panels::drawScrollbar(canvas, asDeviceListArea_, (int)((float)labels.size() * listRowH),
                              asDeviceScrollY_, metrics_.scale);
        y += listH + metrics_.space(SP_MD);

        canvas.textStyled("Mode:", c.x + pad, y, metrics_.text.body, toColor(CLR_TEXT_DIM), FontStyle::Roman);
        y += metrics_.text.body * 1.6f;
        static const char* kModeLabels[2] = {
            "Shared \xE2\x80\x94 other apps can play simultaneously",
            "Exclusive \xE2\x80\x94 lower latency, blocks other apps" };
        for (int i = 0; i < 2; i++) {
            LayoutRect rc = { (int)(c.x + pad), (int)y, (int)(c.x + c.w - pad), (int)(y + rowH) };
            bool s2 = (asExclusive_ == (i == 1));
            Rect hit = widgets::drawRadioRow(canvas, toRect(rc), s2, (i == asHoverModeRow_),
                                             kModeLabels[i], widgets::kTextFree, matrixRadioStyle());
            asModeRows_[i] = toLayoutRect(hit);
            y += rowH;
        }
        y += metrics_.space(SP_MD);
    }
#else
#ifdef MATRIX_HAVE_ALSA
    else if (sel == AudioBackend::Alsa) {
        canvas.textStyled("Device:", c.x + pad, y, metrics_.text.body, toColor(CLR_TEXT_DIM), FontStyle::Roman);
        y += metrics_.text.body * 1.6f;
        std::vector<std::string> labels;
        labels.push_back("(System default)");
        for (auto& d : asAlsaDevices_) labels.push_back(d.name);
        float listH = listHeightFor((int)labels.size());
        asDeviceListArea_ = { (int)(c.x + pad), (int)y, (int)(c.x + c.w - pad), (int)(y + listH) };
        asDeviceListRows_ = widgets::drawScrollList(canvas, toRect(asDeviceListArea_), labels,
                                                    asAlsaSel_, (float)asDeviceScrollY_, listRowH,
                                                    asHoverDeviceRow_, widgets::kTextFree, matrixListStyle());
        panels::drawScrollbar(canvas, asDeviceListArea_, (int)((float)labels.size() * listRowH),
                              asDeviceScrollY_, metrics_.scale);
        y += listH + metrics_.space(SP_MD);
    }
#endif
#ifdef MATRIX_HAVE_JACK
    else if (sel == AudioBackend::Jack) {
        canvas.textStyled("Starting port:", c.x + pad, y, metrics_.text.body, toColor(CLR_TEXT_DIM), FontStyle::Roman);
        y += metrics_.text.body * 1.6f;
        std::vector<std::string> labels;
        labels.push_back("(Auto-connect to first available ports)");
        for (auto& p : asJackPorts_) labels.push_back(p.portName);
        float listH = listHeightFor((int)labels.size());
        asDeviceListArea_ = { (int)(c.x + pad), (int)y, (int)(c.x + c.w - pad), (int)(y + listH) };
        asDeviceListRows_ = widgets::drawScrollList(canvas, toRect(asDeviceListArea_), labels,
                                                    asJackSel_, (float)asDeviceScrollY_, listRowH,
                                                    asHoverDeviceRow_, widgets::kTextFree, matrixListStyle());
        panels::drawScrollbar(canvas, asDeviceListArea_, (int)((float)labels.size() * listRowH),
                              asDeviceScrollY_, metrics_.scale);
        if (asJackPorts_.empty()) {
            Rect a = toRect(asDeviceListArea_);
            canvas.textStyled("No running JACK server found (or no physical playback ports).",
                              a.x + metrics_.space(22.0f), a.y + metrics_.space(98.0f),
                              metrics_.text.body, toColor(CLR_TEXT_DIM), FontStyle::Italic);
        }
        y += listH + metrics_.space(SP_MD);
    }
#endif
#endif

    int by = (int)(content.bottom - (btnH + pad));   // btnH declared above — the list is sized against it
    auto asRects = panels::layoutButtonRow(content, pad, 1, metrics_.space(196.0f), 0.0f,
                                           metrics_.space(panels::kMinActionBtnW), by, (int)btnH);
    asBtnApply_ = asRects[0];
    panels::drawButton(canvas, asBtnApply_, "Apply", asHoverApply_, metrics_.text.body, true);
}

void PlayerWindow::applyAudioSettingsPanel() {
    onStop();

    // Close the OUTGOING backend here, by name, before the new one replaces it.
    // This used to be left to the unique_ptr assignment below — so a backend's
    // entire teardown ran inside a destructor inside a mouse-click callback,
    // which is where a JACK client whose server had already died took the whole
    // app down with no message (see JackSink::live()). onStop() only stops the
    // output; nothing called close() at all. Safe for every backend:
    // UsbAudioOutput::close() is a no-op by design (audio_output.h).
    if (output_) {
        output_->stop();
        output_->close();
    }
    output_.reset();

    AudioBackend sel = asBackendOptions_.empty() ? AudioBackend::Usb : asBackendOptions_[asBackendSelIdx_];
    audioBackend_ = sel;

    if (sel == AudioBackend::Usb) {
        db_.saveSetting("audio_backend", "usb");
        if (asUsbSel_ >= 0 && asUsbSel_ < (int)asUsbDevices_.size()) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%04X", asUsbDevices_[asUsbSel_].vid);
            db_.saveSetting("usb_vid", buf);
            snprintf(buf, sizeof(buf), "%04X", asUsbDevices_[asUsbSel_].pid);
            db_.saveSetting("usb_pid", buf);
        }
        auto vidStr = db_.loadSetting("usb_vid");
        auto pidStr = db_.loadSetting("usb_pid");
        uint16_t vid = vidStr.empty() ? (uint16_t)0x32BB : (uint16_t)strtoul(vidStr.c_str(), nullptr, 16);
        uint16_t pid = pidStr.empty() ? (uint16_t)0x0004 : (uint16_t)strtoul(pidStr.c_str(), nullptr, 16);
        usbDriver_.close();
        usbOpen_ = usbDriver_.open(vid, pid);
        if (usbOpen_) {
            usbDriver_.parseDescriptors();
        } else {
            char msgBuf[96];
            snprintf(msgBuf, sizeof(msgBuf),
                "USB DAC not found (VID=%04X PID=%04X) \xE2\x80\x94 check Audio Settings.",
                vid, pid);
            audioNotice_ = msgBuf;
            invalidate();
        }
        output_ = std::make_unique<UsbAudioOutput>(usbDriver_);
    }
#ifdef _WIN32
    else if (sel == AudioBackend::Wasapi) {
        db_.saveSetting("audio_backend", "wasapi");
        std::string devId;
        if (asWasapiSel_ > 0 && asWasapiSel_ <= (int)asWasapiDevices_.size())
            devId = wideToUtf8(asWasapiDevices_[asWasapiSel_ - 1].id);
        db_.saveSetting("wasapi_device_id", devId);
        db_.saveSetting("wasapi_mode", asExclusive_ ? "exclusive" : "shared");
        wasapiDeviceId_ = utf8ToWide(devId);
        wasapiMode_ = asExclusive_ ? WasapiMode::Exclusive : WasapiMode::Shared;
        output_ = std::make_unique<WasapiOutput>(wasapiDeviceId_, wasapiMode_);
    }
#else
#ifdef MATRIX_HAVE_ALSA
    else if (sel == AudioBackend::Alsa) {
        db_.saveSetting("audio_backend", "alsa");
        std::string devId;
        if (asAlsaSel_ > 0 && asAlsaSel_ <= (int)asAlsaDevices_.size())
            devId = asAlsaDevices_[asAlsaSel_ - 1].deviceId;
        db_.saveSetting("alsa_device_id", devId);
        alsaDeviceId_ = devId.empty() ? "default" : devId;
        output_ = std::make_unique<AlsaOutput>(alsaDeviceId_);
    }
#endif
#ifdef MATRIX_HAVE_JACK
    else if (sel == AudioBackend::Jack) {
        db_.saveSetting("audio_backend", "jack");
        std::string port;
        if (asJackSel_ > 0 && asJackSel_ <= (int)asJackPorts_.size())
            port = asJackPorts_[asJackSel_ - 1].portName;
        db_.saveSetting("jack_port", port);
        jackStartPort_ = port;
        output_ = std::make_unique<JackOutput>(jackStartPort_);
    }
#endif
#endif

    closeActivePanel();
}

// ── EQ Settings panel ────────────────────────────────────────────────────────

void PlayerWindow::onEqSettings() {
    panelFromSidebar_ = false;   // the sidebar path re-arms this after calling
    eqDeviceKey_ = getActiveDeviceKey();
    markEqAssignmentDirty();     // the device just changed under the header line
    eqBitperfectActive_ = bitperfectMode_.load();
    eqSearch_.clear();
    eqSearchFocused_ = false;
    eqSelectedRow_ = -1;
    eqHoverRow_ = -1;
    eqScrollY_ = 0;
    eqHoverClose_ = eqHoverAssign_ = eqHoverClear_ = false;
    eqHoverTabAll_ = eqHoverTabMine_ = eqHoverPin_ = eqHoverRemove_ = false;
    // Opens on whichever tab has something to show: jumping a listener with a
    // saved set straight into 5000 catalogue entries buries the four rows they
    // actually use.
    reloadEqHeadphones();
    eqShowMine_ = !eqHeadphones_.empty();
    eqRefilter();
    activePanel_ = SettingsPanel::EqSettings;
    invalidate();
}

void PlayerWindow::eqRefilter() {
    ensureEqProfiles();
    eqFilteredIndices_.clear();
    std::string needle = eqSearch_;
    for (auto& ch : needle) ch = (char)std::tolower((unsigned char)ch);
    auto& all = eqProfiles_.getAll();
    for (int i = 0; i < (int)all.size(); i++) {
        // "My Drivers" is a filter over the SAME list and the same
        // selection, not a second list — one selection means Pin/Remove can
        // never act on a row other than the visibly highlighted one.
        if (eqShowMine_ &&
            !isKnownHeadphone({ all[i].name, all[i].source, all[i].form }))
            continue;
        std::string nameLower = all[i].name;
        for (auto& ch : nameLower) ch = (char)std::tolower((unsigned char)ch);
        if (!needle.empty() && nameLower.find(needle) == std::string::npos) continue;
        eqFilteredIndices_.push_back(i);
    }
    if (eqSelectedRow_ >= (int)eqFilteredIndices_.size()) eqSelectedRow_ = -1;
}

// The saved row eqSelectedRow_ points at, or nullptr — the bridge between the
// panel's profile-indexed list and eq_headphones. Pin/Remove need the saved
// entry (for its pinned flag), not just the profile.
const EqHeadphone* PlayerWindow::eqSelectedHeadphone() const {
    ensureEqProfiles();
    if (eqSelectedRow_ < 0 || eqSelectedRow_ >= (int)eqFilteredIndices_.size())
        return nullptr;
    const auto& p = eqProfiles_.getAll()[eqFilteredIndices_[eqSelectedRow_]];
    for (const auto& h : eqHeadphones_)
        if (h.name == p.name && h.source == p.source && h.form == p.form)
            return &h;
    return nullptr;
}

void PlayerWindow::drawEqSettings(Canvas& canvas, const LayoutRect& area) {
    ensureEqProfiles();
    LayoutRect content = panels::drawHeader(canvas, area, "EQ / AutoEQ Profiles", metrics_.scale, metrics_.text.header, eqCloseRc_);
    Rect c = toRect(content);
    float pad = metrics_.space(SP_LG);
    float y = c.y + pad;

    // Mono, because "32BB:0004" is an identifier, not a name. The family
    // already carries a face that says so, and it is the one with the most
    // legibility headroom of the four (9.14px floor against the serif's
    // 18.29px — see min_text_size in the root CMakeLists). Driver and album
    // names stay serif: the rule is about what the string IS, not where it sits.
    // Both header lines come from the cache: the assignment is a database read,
    // and re-running it per frame bought nothing — see eqAssignLineDirty_.
    if (eqAssignLineDirty_) {
        eqDeviceLine_ = "Device: " + eqDeviceKey_;
        EqAssignment assign;
        if (db_.loadEqAssignment(eqDeviceKey_, assign) || db_.loadEqAssignment("global", assign))
            eqAssignLine_ = "Current EQ: " + assign.name;
        else
            eqAssignLine_ = "No EQ assigned";
        eqAssignLineDirty_ = false;
    }

    canvas.textStyled(eqDeviceLine_, c.x + pad, y, metrics_.text.secondary, toColor(CLR_TEXT_DIM), FontStyle::Math);
    y += metrics_.text.secondary * 1.6f;

    canvas.textStyled(eqAssignLine_, c.x + pad, y, metrics_.text.secondary, toColor(CLR_ACCENT), FontStyle::Roman);
    y += metrics_.text.secondary * 1.8f;

    if (eqBitperfectActive_) {
        canvas.textStyled("Bitperfect mode active \xE2\x80\x94 EQ applies once Reference EQ mode is enabled.",
                          c.x + pad, y, metrics_.text.secondary, toColor(CLR_TEXT_DIM), FontStyle::Italic);
        y += metrics_.text.secondary * 1.6f;
    }

    // Two views over one list: the saved set, or the whole catalogue. This is
    // also the ONLY place a saved pair is pinned or removed — the sidebar
    // block stays a pure switcher, with no room for a per-row × at space(277).
    {
        float tabH = metrics_.space(52.0f);
        auto tabRects = panels::layoutButtonRow(content, pad, 2, metrics_.space(200.0f),
                                                metrics_.space(SP_SM), metrics_.space(panels::kMinActionBtnW),
                                                (int)y, (int)tabH, /*alignRight=*/false);
        eqTabMine_ = tabRects[0];
        eqTabAll_  = tabRects[1];
        auto tab = [&](const LayoutRect& rc, const char* label, bool active, bool hovered) {
            Rect r = toRect(rc);
            // Accent = state, hover = neutral (UI_DESIGN_SYSTEM.md §1.4).
            if (active)
                canvas.rect(r.x, r.y, r.w, r.h,
                            toColor(CLR_ACCENT, UI_SELECT_TINT_ALPHA), UI_CORNER_RADIUS);
            else if (hovered)
                canvas.rect(r.x, r.y, r.w, r.h, toColor(CLR_HOVER), UI_CORNER_RADIUS);
            if (active)
                canvas.rect(r.x, r.y + r.h - metrics_.stroke(2.0f), r.w,
                            metrics_.stroke(2.0f), toColor(CLR_ACCENT));
            float tw = canvas.textWidthStyled(label, metrics_.text.body, FontStyle::Roman);
            canvas.textStyled(label, r.x + std::max(0.0f, (r.w - tw) * 0.5f),
                              r.y + r.h * 0.5f - metrics_.text.body * 0.5f,
                              metrics_.text.body,
                              toColor(active ? CLR_ACCENT : CLR_TEXT_SECONDARY),
                              FontStyle::Roman);
        };
        tab(eqTabMine_, "My Drivers", eqShowMine_,  eqHoverTabMine_);
        tab(eqTabAll_,  "All Profiles",  !eqShowMine_, eqHoverTabAll_);
        y += tabH + metrics_.space(SP_SM);
    }

    eqSearchRc_ = { (int)(c.x + pad), (int)y, (int)(c.x + c.w - pad), (int)(y + metrics_.space(55.0f)) };
    drawSearchField(canvas, eqSearchRc_, eqSearch_, eqSearchFocused_, "Search profiles",
                    metrics_.text.body);
    y += metrics_.space(55.0f) + metrics_.space(16.0f);

    float btnH = metrics_.space(58.0f);
    LayoutRect listArea = { content.left, (int)y, content.right, (int)(content.bottom - (btnH + pad * 2)) };
    eqListArea_ = listArea;

    std::vector<std::string> labels;
    labels.reserve(eqFilteredIndices_.size());
    auto& all = eqProfiles_.getAll();
    for (int idx : eqFilteredIndices_) {
        std::string label = all[idx].name;
        if (!all[idx].form.empty()) label += "  (" + all[idx].form + ")";
        // In the saved view, say which rows are pinned — pinned is the one
        // property that changes what Remove and the prune will do to a row.
        if (eqShowMine_) {
            for (const auto& h : eqHeadphones_) {
                if (h.name == all[idx].name && h.source == all[idx].source &&
                    h.form == all[idx].form) {
                    if (h.pinned) label += "  \xE2\x80\x94 pinned";
                    break;
                }
            }
        }
        labels.push_back(label);
    }
    float eqRowH = panelRowH();
    eqListRows_ = widgets::drawScrollList(canvas, toRect(listArea), labels,
                                          eqSelectedRow_, (float)eqScrollY_, eqRowH,
                                          eqHoverRow_, widgets::kTextFree, matrixListStyle());
    panels::drawScrollbar(canvas, listArea, (int)((float)labels.size() * eqRowH), eqScrollY_, metrics_.scale);
    if (labels.empty()) {
        Rect a = toRect(listArea);
        // The saved view's empty state explains the rule rather than just
        // reporting nothing: "no profiles match" would read as a bug to
        // someone who has assigned a profile and not yet listened to it.
        const char* msg = eqShowMine_
            ? (eqSearch_.empty()
                 ? "No drivers saved yet \xE2\x80\x94 pick a profile under All Profiles "
                   "and listen for a minute."
                 : "No saved drivers match.")
            : "No profiles match.";
        canvas.textStyled(msg, a.x + metrics_.space(22.0f), a.y + metrics_.space(22.0f),
                          metrics_.text.body, toColor(CLR_TEXT_DIM), FontStyle::Italic);
    }

    int by = (int)(content.bottom - (btnH + pad));
    // Slot 0 is the PRIMARY and sits hard right, matching Manage Folders,
    // Audio Settings and the folder picker. Secondaries fill leftward. This
    // panel used to lay out left-to-right, so it was the one page of four
    // where the green button changed sides.
    int eqBtnCount = eqShowMine_ ? 3 : 2;
    auto eqBtnRects = panels::layoutButtonRow(content, pad, eqBtnCount, metrics_.space(277.0f),
                                              metrics_.space(SP_MD), metrics_.space(panels::kMinActionBtnW),
                                              by, (int)btnH);
    eqBtnAssign_ = eqBtnRects[0];
    if (eqShowMine_) {
        const EqHeadphone* sel = eqSelectedHeadphone();
        eqBtnPin_    = eqBtnRects[1];
        eqBtnRemove_ = eqBtnRects[2];
        eqBtnClear_  = {};   // not offered here; Clear belongs to the device view
        panels::drawButton(canvas, eqBtnAssign_, "Select", eqHoverAssign_, metrics_.text.body, true);
        panels::drawButton(canvas, eqBtnPin_,
                           (sel && sel->pinned) ? "Unpin" : "Pin",
                           eqHoverPin_, metrics_.text.body);
        panels::drawButton(canvas, eqBtnRemove_, "Remove", eqHoverRemove_, metrics_.text.body);
    } else {
        eqBtnClear_  = eqBtnRects[1];
        eqBtnPin_    = {};
        eqBtnRemove_ = {};
        panels::drawButton(canvas, eqBtnAssign_, "Assign to Device", eqHoverAssign_, metrics_.text.body, true);
        panels::drawButton(canvas, eqBtnClear_, "Clear", eqHoverClear_, metrics_.text.body);
    }
}

// ── Playlists section ───────────────────────────────────────────────────────
//
// Three generated lists, each of which IS its query (see core/include/core/db.h):
// nothing is stored, so nothing can drift from the listening log.
//
// This is a SECTION, not a panel — the fifth sidebar row, drawn into the same
// content area and with the same two levels the album section has: a grid of
// tiles, and one of them opened full-page. Everything outside the content area
// keeps working while it is up, which is the whole point of the change: as a
// borrowed settings overlay it swallowed every click and key in the window, so
// the four sibling rows, the Settings row and the play/stop key were all dead
// while a playlist was on screen.

namespace {
// Heavy Rotation and Forgotten Favourites are rankings and take a cap. Never
// Heard does NOT use this: its limit argument means the opposite thing (see
// loadPlaylist).
constexpr int kPlaylistLimit = 100;
// A play count that reads as a pattern rather than an accident. One play cannot
// be "forgotten" — there is nothing yet to forget; two is a false start or a
// double-tap; three is the first count at which "you used to come back to this"
// is a claim worth making.
constexpr int kForgottenMinPlays = 3;
// Deliberately INSIDE the 90-day "recent taste" window (RangePreset::Last90Days),
// so a record drops out of "recent" only after it has already been offered back.
constexpr int64_t kForgottenStaleSec = 60 * 86400;

struct RangeTab { RangePreset preset; const char* label; };
constexpr RangeTab kRangeTabs[5] = {
    { RangePreset::Last7Days,  "7 Days"    },
    { RangePreset::Last30Days, "30 Days"   },
    { RangePreset::Last90Days, "90 Days"   },
    { RangePreset::ThisYear,   "This Year" },
    { RangePreset::AllTime,    "All Time"  },
};
} // namespace

const char* PlayerWindow::playlistTitle(PlaylistKind k) {
    switch (k) {
    case PlaylistKind::HeavyRotation:       return "Heavy Rotation";
    case PlaylistKind::ForgottenFavourites: return "Forgotten Favourites";
    case PlaylistKind::NeverHeard:          return "Never Heard";
    case PlaylistKind::None:                break;
    }
    return "Playlists";
}

// A playlist's own subtitle — what the list MEANS, one line, the same text the
// tile prints under its art and the row list used to print in the chooser.
static const char* playlistSubtitle(int kindIdx) {
    switch (kindIdx) {
    case 0: return "What you have chosen and finished, most often.";
    case 1: return "Loved once, untouched for a while.";
    default: return "Everything you have never played.";
    }
}

void PlayerWindow::openPlaylistSection() {
    // Exactly what clicking Albums/EPs/Singles/Remixes does, plus the tiles'
    // artwork: leave whatever was showing, land on this section's grid.
    navSection_     = NavSection::Playlists;
    settingsOpen_   = false;
    trackPanelOpen_ = false;
    plKind_         = PlaylistKind::None;
    plEntries_.clear();
    plDurationMs_.clear();
    plListRows_.clear();
    plScrollY_       = 0;
    plHoverRow_      = -1;
    plHoverTile_     = -1;
    plHoverRangeTab_ = -1;
    gridScrollY_     = 0;
    loadPlaylistCovers();
    recalcLayout();
    invalidate();
}

void PlayerWindow::loadPlaylistCovers() {
    for (auto& c : plCovers_) c = PlaylistCover{};

    const int64_t now = (int64_t)time(nullptr);
    StatsRange range = rangeFor(plRangePreset_, now, localUtcOffsetMinutes(now));

    // Only the RANKED lists get a mosaic — a quadrant is a standing, and Never
    // Heard has none to give (see PlaylistCover's comment).
    const std::vector<TopEntry> ranked[2] = {
        db_.heavyRotation(range, kPlaylistLimit),
        db_.forgottenFavourites(kPlaylistLimit, kForgottenMinPlays,
                                now - kForgottenStaleSec),
    };

    // ONE lock for both lists, for the same reason loadPlaylist() takes one for
    // its whole resolve pass: albumsMu_ is the lock the gapless thread and
    // onPlay() contend for, and this runs on the UI thread.
    std::lock_guard<std::mutex> lk(albumsMu_);
    for (int k = 0; k < 2; k++) {
        PlaylistCover& cov = plCovers_[k];
        cov.ranked = true;
        for (const TopEntry& e : ranked[k]) {
            auto it = trackKeyIndex_.find(e.key);
            if (it == trackKeyIndex_.end()) continue;   // no copy on disk any more
            int ai = it->second.first;
            if (ai < 0 || ai >= (int)albums_.size()) continue;
            if (albums_[ai].artPath.empty()) continue;
            // Distinct RECORDS, not distinct rows: five tracks off one album
            // are five rows of the list but one cover, and painting it four
            // times would say the list is four albums deep when it is one.
            bool seen = false;
            for (int i = 0; i < cov.count && !seen; i++) seen = (cov.albums[i] == ai);
            if (seen) continue;
            if (cov.count == 4) { cov.more = true; break; }  // a fifth record: there IS more
            cov.albums[cov.count++] = ai;
        }
    }
}

void PlayerWindow::loadPlaylist(PlaylistKind kind) {
    plKind_ = kind;
    plEntries_.clear();
    plDurationMs_.clear();
    plListRows_.clear();
    plScrollY_  = 0;
    plHoverRow_ = -1;
    if (kind == PlaylistKind::None) { invalidate(); return; }

    const int64_t now = (int64_t)time(nullptr);

    switch (kind) {
    case PlaylistKind::HeavyRotation: {
        // rangeFor() reads no clock of its own — that is what makes it
        // testable — so the caller supplies both "now" and the offset.
        StatsRange range = rangeFor(plRangePreset_, now, localUtcOffsetMinutes(now));
        plEntries_ = db_.heavyRotation(range, kPlaylistLimit);
        break;
    }
    case PlaylistKind::ForgottenFavourites:
        plEntries_ = db_.forgottenFavourites(kPlaylistLimit, kForgottenMinPlays,
                                             now - kForgottenStaleSec);
        break;
    case PlaylistKind::NeverHeard:
        // 0 means NO LIMIT here, the opposite of every other query in Db (see
        // db_stats.cpp's neverHeard: capping it would quietly decide how much
        // of your own library you are allowed to meet). Written as a literal,
        // never as a shared variable with the two above, so a future edit
        // cannot collapse the two conventions into one.
        plEntries_ = db_.neverHeard(0);
        break;
    case PlaylistKind::None:
        break;
    }

    // TopEntry carries no duration, so resolve each key through the library —
    // ONCE, here, under a SINGLE albumsMu_ lock. Doing it per row per frame
    // would take the lock during every scroll frame, and albumsMu_ is the same
    // non-reentrant lock the gapless thread and onPlay() contend for.
    plDurationMs_.assign(plEntries_.size(), -1);
    {
        std::lock_guard<std::mutex> lk(albumsMu_);
        for (size_t i = 0; i < plEntries_.size(); i++) {
            auto it = trackKeyIndex_.find(plEntries_[i].key);
            if (it == trackKeyIndex_.end()) continue;   // no copy on disk any more
            int ai = it->second.first, ti = it->second.second;
            if (ai < 0 || ai >= (int)albums_.size()) continue;
            if (ti < 0 || ti >= (int)albums_[ai].tracks.size()) continue;
            plDurationMs_[i] = albums_[ai].tracks[ti].durationMs;
        }
    }
    invalidate();
}

// The 2x2 mosaic a RANKED playlist's tile wears instead of a cover it does not
// have. Quadrant numbering is the mathematical one the listener asked for, not
// reading order: quadrant 1 is TOP-RIGHT and holds first place, and the rest
// run anticlockwise — top-left, bottom-left, bottom-right. (drawVariantMosaic
// numbers its quadrants left-to-right instead; a remix group has no ranking to
// express, so nothing there points at a particular corner.)
//
// The fourth quadrant is the one that carries information: with exactly four
// records behind the list it is the fourth record's cover, and past that it
// fades to black — three covers plus "and there is more", rather than an
// arbitrary fourth pretending to be the whole list. Empty quadrants stay flat
// CLR_TILE_PLACEHOLDER, so absence reads as absence and not as a broken image.
void PlayerWindow::drawPlaylistTileArt(Canvas& canvas, int kindIdx,
                                       float x, float y, float a) {
    const PlaylistCover& cov = plCovers_[kindIdx];

    if (!cov.ranked) {
        // An UNORDERED list: a gradient, because there is no first place to put
        // in a corner. See PlaylistCover — a hand-made list would offer a
        // custom image or a solid colour here instead; a generated one has
        // nobody to ask.
        canvas.rectGradient(x, y, a, a,
                            toColor(CLR_TILE_PLACEHOLDER), toColor(CLR_BG_MAIN),
                            Canvas::GradientDir::Vertical);
        return;
    }

    const float half = a * 0.5f;
    // Rank order -> quadrant origin. Index is the rank (0 = first place).
    const float qx[4] = { x + half, x,        x,        x + half };
    const float qy[4] = { y,        y,        y + half, y + half };
    // Four records exactly: the fourth quadrant is the fourth cover. More than
    // four: it is the fade, and only three covers are drawn.
    const int covers = cov.more ? std::min(cov.count, 3) : cov.count;

    for (int q = 0; q < 4; q++) {
        if (q < covers) {
            // Half-size decode, not the tile texture scaled down — there is no
            // mip chain here (see onArtDecoded), so a 2:1 minification aliases.
            TextureHandle tex = getGridArtTexture(cov.albums[q], ArtHalf);
            if (tex != kInvalidTexture) {
                canvas.imageFg(tex, qx[q], qy[q], half, half);
                continue;
            }
            // Not decoded yet: the placeholder below, replaced on the next frame.
        }
        if (q == 3 && cov.more) {
            canvas.rectGradient(qx[q], qy[q], half, half,
                                toColor(CLR_TILE_PLACEHOLDER), toColor(CLR_BG_MAIN),
                                Canvas::GradientDir::Vertical);
        } else {
            canvas.rect(qx[q], qy[q], half, half, toColor(CLR_TILE_PLACEHOLDER));
        }
    }
}

// One tile per generated list, laid out on the album grid's own geometry
// (gridStepX_/gridArtSize_/gridPad*) so a playlist tile is the same object in
// the same place as an album tile — which is the point.
void PlayerWindow::drawPlaylistGrid(Canvas& canvas, const LayoutRect& area) {
    Rect g = toRect(area);
    canvas.setClip(g.x, g.y, g.w, g.h);

    const int tileStepX = gridStepX_;
    const int tileStepY = gridTileSize_ + gridRowGap_;

    for (int i = 0; i < 3; i++) {
        int col = i % gridCols_, row = i / gridCols_;
        float x = (float)(area.left + gridPadXpx_ + col * tileStepX
                          + (tileStepX - gridArtSize_) / 2);
        float y = (float)(area.top + gridPadYpx_ + row * tileStepY - gridScrollY_);
        float a = (float)gridArtSize_;

        if (plHoverTile_ == i)
            canvas.rect(x - metrics_.space(SP_XS), y - metrics_.space(SP_XS),
                        a + metrics_.space(12.0f), a + metrics_.space(12.0f),
                        toColor(CLR_HOVER), UI_CORNER_RADIUS);

        drawPlaylistTileArt(canvas, i, x, y, a);

        // Same text block as an album tile: name over one dim second line,
        // centred and confined to the art's width.
        float adv = titleArtistAdvance(metrics_.text.body);
        float ty  = y + a + metrics_.space(16.0f);
        auto centered = [&](const std::string& s, float yy, float sz,
                            ColorRef clr, FontStyle st) {
            float w = canvas.textWidthStyled(s, sz, st);
            canvas.textStyled(s, x + std::max(0.0f, (a - w) * 0.5f), yy,
                              sz, toColor(clr), st);
        };
        const char* name = playlistTitle((PlaylistKind)(i + 1));
        std::string l1, l2;
        splitTwoLines(canvas, name, a, metrics_.text.body, FontStyle::Bold, l1, l2);
        centered(l1, ty, metrics_.text.body, CLR_TEXT_ALBUM_TITLE, FontStyle::Bold);
        if (!l2.empty())
            centered(l2, ty + adv, metrics_.text.body, CLR_TEXT_ALBUM_TITLE, FontStyle::Bold);
        centered(truncateToWidth(canvas, playlistSubtitle(i), a,
                                 metrics_.text.secondary, FontStyle::Italic),
                 ty + adv * 2, metrics_.text.secondary, CLR_TEXT_SECONDARY, FontStyle::Italic);
    }

    canvas.clearClip();
}

// Same art-only target rule as gridHitTest(): the gaps and the text block below
// are dead space.
int PlayerWindow::playlistTileHitTest(int x, int y) const {
    if (x < rcGrid_.left || x >= rcGrid_.right || y < rcGrid_.top || y >= rcGrid_.bottom)
        return -1;
    const int tileStepX = gridStepX_;
    const int tileStepY = gridTileSize_ + gridRowGap_;
    int col = (x - rcGrid_.left - gridPadXpx_) / tileStepX;
    int row = (y - rcGrid_.top - gridPadYpx_ + gridScrollY_) / tileStepY;
    if (col < 0 || col >= gridCols_ || row < 0) return -1;
    int i = row * gridCols_ + col;
    if (i < 0 || i >= 3) return -1;

    int artX = rcGrid_.left + gridPadXpx_ + col * tileStepX + (tileStepX - gridArtSize_) / 2;
    int artY = rcGrid_.top + gridPadYpx_ + row * tileStepY - gridScrollY_;
    if (x < artX || x >= artX + gridArtSize_ ||
        y < artY || y >= artY + gridArtSize_) return -1;
    return i;
}

void PlayerWindow::drawPlaylistSection(Canvas& canvas, const LayoutRect& area) {
    if (plKind_ == PlaylistKind::None) drawPlaylistGrid(canvas, area);
    else                               drawPlaylists(canvas, area);
}

void PlayerWindow::playPlaylistFrom(int row) {
    if (row < 0 || row >= (int)plEntries_.size()) return;
    std::vector<std::string> keys;
    keys.reserve(plEntries_.size());
    for (auto& e : plEntries_) keys.push_back(e.key);
    // startQueue() resolves every key and drops what the library lost, so the
    // clicked index is re-anchored by key inside it. It sets the position but
    // does not start playback — the same "set, then onPlay()" idiom the album
    // view and grid already use.
    if (startQueue(keys, row)) onPlay();
}

// One opened list, full-page — the album view's counterpart on this side. Only
// ever called with plKind_ != None (drawPlaylistSection draws the tile grid for
// None), and it draws no Close affordance for the same reason the album view
// draws none: going back is Escape, or the mouse's back button.
void PlayerWindow::drawPlaylists(Canvas& canvas, const LayoutRect& area) {
    LayoutRect unusedCloseRc{};
    LayoutRect content = panels::drawHeader(canvas, area, playlistTitle(plKind_),
                                            metrics_.scale, metrics_.text.header,
                                            unusedCloseRc);
    Rect c = toRect(content);
    float pad = metrics_.space(SP_LG);
    float y = c.y + pad;

    // ── A list ──────────────────────────────────────────────────────────────
    // Range tabs, and ONLY for Heavy Rotation: it is the one query that takes a
    // StatsRange. Offering the control where it does nothing would be a lie
    // about what the other two lists are.
    if (plKind_ == PlaylistKind::HeavyRotation) {
        float tabH = metrics_.space(52.0f);
        float tabW = metrics_.space(150.0f);
        float gap  = metrics_.space(SP_SM);
        for (int i = 0; i < 5; i++) {
            float tx = c.x + pad + i * (tabW + gap);
            plRangeTabRc_[i] = { (int)tx, (int)y, (int)(tx + tabW), (int)(y + tabH) };
            Rect r = toRect(plRangeTabRc_[i]);
            bool active = (plRangePreset_ == kRangeTabs[i].preset);
            // Accent = state, hover = neutral (UI_DESIGN_SYSTEM.md §1.4).
            if (active)
                canvas.rect(r.x, r.y, r.w, r.h,
                            toColor(CLR_ACCENT, UI_SELECT_TINT_ALPHA), UI_CORNER_RADIUS);
            else if (plHoverRangeTab_ == i)
                canvas.rect(r.x, r.y, r.w, r.h, toColor(CLR_HOVER), UI_CORNER_RADIUS);
            if (active)
                canvas.rect(r.x, r.y + r.h - metrics_.stroke(2.0f), r.w,
                            metrics_.stroke(2.0f), toColor(CLR_ACCENT));
            float tw = canvas.textWidthStyled(kRangeTabs[i].label, metrics_.text.body, FontStyle::Roman);
            canvas.textStyled(kRangeTabs[i].label, r.x + std::max(0.0f, (r.w - tw) * 0.5f),
                              r.y + r.h * 0.5f - metrics_.text.body * 0.5f,
                              metrics_.text.body,
                              toColor(active ? CLR_ACCENT : CLR_TEXT_SECONDARY), FontStyle::Roman);
        }
        y += tabH + metrics_.space(SP_MD);
    } else {
        for (auto& rc : plRangeTabRc_) rc = {};
    }

    // A row stacks a title over an artist, so its height comes from those two
    // roles plus breathing room — not from kPanelRowH, which is sized for the
    // single line the settings panels draw and would let each row's artist
    // collide with the next row's title.
    plRowH_ = (int)(metrics_.text.body * 1.3f + metrics_.text.secondary * 1.35f
                    + metrics_.space(20.0f));

    // A bottom margin the height of a button row. It used to BE a button row
    // (the panel's Close); the list keeps the gap because running the last row
    // flush into the transport bar reads as truncation.
    float btnH = metrics_.space(58.0f);
    // Capped reading measure, the same rule (and the same number) as the album
    // view's track list: unbounded, a wide window puts a title a full screen
    // away from its own duration and the eye has to cross to pair them. The cap
    // scales with the type, so it holds a constant measure rather than a
    // constant pixel width.
    const int plListW = std::min(content.right - content.left,
                                 (int)metrics_.space(820.0f));
    plListArea_ = { content.left, (int)y, content.left + plListW,
                    (int)(content.bottom - (btnH + pad)) };

    // Same invariant the album grid needs, enforced here rather than in
    // recalcLayout() because this list's row height and viewport are measured
    // during the draw, a few lines up. A resize taller shrinks the maximum
    // scroll, and an offset past the end scrolls every row off the top.
    plScrollY_ = (int)clampScroll((float)plScrollY_,
                                  (float)(plEntries_.size() * (size_t)plRowH_),
                                  (float)(plListArea_.bottom - plListArea_.top));

    if (plEntries_.empty()) {
        plListRows_.clear();
        const char* msg = "";
        switch (plKind_) {
        case PlaylistKind::HeavyRotation:
            msg = "Nothing in heavy rotation for this range yet. "
                  "A track counts here once you have played it all the way through.";
            break;
        case PlaylistKind::ForgottenFavourites:
            msg = "No forgotten favourites yet. "
                  "Records you loved and have not touched in a while will appear here.";
            break;
        case PlaylistKind::NeverHeard:
            // Two very different reasons this list can be empty, and only the
            // GUI can tell them apart: an empty library is a dead end, having
            // heard everything is an achievement.
            msg = albums_.empty()
                ? "No music yet. Add a folder from Settings to get started."
                : "You have heard everything in your library at least once.";
            break;
        case PlaylistKind::None:
            break;
        }
        Rect a = toRect(plListArea_);
        canvas.textStyled(msg, a.x + metrics_.space(22.0f), a.y + metrics_.space(22.0f),
                          metrics_.text.body, toColor(CLR_TEXT_DIM), FontStyle::Italic);
        return;
    }

    // drawScrollList draws ONE plain string per row in ONE style, and a ranked
    // row needs four differently-styled columns. So it is used for what it is
    // genuinely good at — scroll clipping, off-screen culling, the hover pill,
    // and rects to hit-test — by handing it blank strings, and the text is
    // drawn on top. fitWidth must be off: matrixListStyle() hugs a row to its
    // text, and a blank string would hug to nothing.
    {
        auto style = matrixListStyle();
        style.fitWidth = false;
        std::vector<std::string> blanks(plEntries_.size());
        plListRows_ = widgets::drawScrollList(canvas, toRect(plListArea_), blanks,
                                              -1, (float)plScrollY_, (float)plRowH_,
                                              plHoverRow_, widgets::kTextFree, style);
    }
    panels::drawScrollbar(canvas, plListArea_, (int)plEntries_.size() * plRowH_,
                          plScrollY_, metrics_.scale);

    canvas.setClip((float)plListArea_.left, (float)plListArea_.top,
                   (float)(plListArea_.right - plListArea_.left),
                   (float)(plListArea_.bottom - plListArea_.top));
    const float rankW = metrics_.space(78.0f);
    // panels::drawScrollbar docks inside the list's right edge, so the numbers
    // are inset past it rather than tucked under it.
    const float rightInset = metrics_.space(44.0f);
    const float durW  = metrics_.space(96.0f);
    const float playsW = metrics_.space(150.0f);
    for (auto& lr : plListRows_) {
        const int i = lr.index;
        if (i < 0 || i >= (int)plEntries_.size()) continue;
        const TopEntry& e = plEntries_[i];
        const float rx = lr.rect.x + metrics_.space(20.0f);
        const float ry = lr.rect.y + lr.rect.h * 0.5f;
        // Title sits above the row's midline, artist below it, so the pair
        // reads as one block with the numbers vertically centred beside them.
        const float titleY  = ry - metrics_.text.body * 0.95f;
        const float artistY = ry + metrics_.space(4.0f);

        // A rank prefix only where there IS a ranking. Never Heard is ordered
        // artist ⨯ album ⨯ disc ⨯ track — a browsing order — and "7th" would
        // claim a standing that ordering does not confer (see ui_text.hh).
        if (plKind_ != PlaylistKind::NeverHeard) {
            std::string rank = ordinal(i + 1);
            canvas.textStyled(rank, rx, ry - metrics_.text.secondary * 0.5f,
                              metrics_.text.secondary, toColor(CLR_TEXT_DIM), FontStyle::Math);
        }

        const float textX = rx + rankW;
        const float textW = lr.rect.x + lr.rect.w - rightInset - playsW - durW - textX;

        // A track can leave the library while its plays survive in the log —
        // the key stays, the title does not. Say so rather than drawing a gap.
        bool gone = e.label.empty();
        std::string title = gone ? std::string("(no longer in the library)") : e.label;
        canvas.textStyled(truncateToWidth(canvas, title, textW, metrics_.text.body,
                                          gone ? FontStyle::Italic : FontStyle::Bold),
                          textX, titleY, metrics_.text.body,
                          toColor(gone ? CLR_TEXT_DIM : CLR_TEXT_PRIMARY),
                          gone ? FontStyle::Italic : FontStyle::Bold);
        if (!e.subLabel.empty())
            canvas.textStyled(truncateToWidth(canvas, e.subLabel, textW, metrics_.text.secondary,
                                              FontStyle::Italic),
                              textX, artistY,
                              metrics_.text.secondary, toColor(CLR_TEXT_SECONDARY),
                              FontStyle::Italic);

        // Never Heard has no plays by definition (the query hardcodes 0), so
        // printing "0 plays" on every row would be noise saying nothing.
        if (plKind_ != PlaylistKind::NeverHeard && e.plays > 0) {
            std::string plays = std::to_string(e.plays) + (e.plays == 1 ? " play" : " plays");
            float pw = canvas.textWidthStyled(plays, metrics_.text.secondary, FontStyle::Math);
            canvas.textStyled(plays, lr.rect.x + lr.rect.w - rightInset - durW - pw,
                              ry - metrics_.text.secondary * 0.5f, metrics_.text.secondary,
                              toColor(CLR_TEXT_SECONDARY), FontStyle::Math);
        }

        if (i < (int)plDurationMs_.size() && plDurationMs_[i] > 0) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%d:%02d", plDurationMs_[i] / 60000,
                     (plDurationMs_[i] % 60000) / 1000);
            float dw = canvas.textWidthStyled(buf, metrics_.text.secondary, FontStyle::Math);
            canvas.textStyled(buf, lr.rect.x + lr.rect.w - rightInset - dw,
                              ry - metrics_.text.secondary * 0.5f, metrics_.text.secondary,
                              toColor(CLR_TEXT_SECONDARY), FontStyle::Math);
        }
    }
    canvas.clearClip();
}

// Writes this section's hover state ONLY. onMouseMove has already cleared it,
// exactly as it clears hoverAlbumIdx_/hoverSidebarItem_/... before hit-testing,
// and it compares against the values it saved to decide whether to redraw — so
// there is nothing to report back from here.
void PlayerWindow::onPlaylistsMouseMove(int x, int y) {
    if (plKind_ == PlaylistKind::None) {
        plHoverTile_ = playlistTileHitTest(x, y);
        return;
    }
    if (plKind_ == PlaylistKind::HeavyRotation)
        for (int i = 0; i < 5; i++)
            if (ptInRect(plRangeTabRc_[i], x, y)) { plHoverRangeTab_ = i; break; }
    plHoverRow_ = hitTestListRows(plListRows_, x, y);
}

void PlayerWindow::onPlaylistsClick(int x, int y) {
    if (plKind_ == PlaylistKind::None) {
        int tile = playlistTileHitTest(x, y);
        if (tile >= 0) {
            loadPlaylist((PlaylistKind)(tile + 1));
            // A new destination: whatever the last goBack() remembered is no
            // longer reachable forward from here.
            navForwardValid_ = false;
            recalcLayout();
        }
        return;
    }

    if (plKind_ == PlaylistKind::HeavyRotation) {
        for (int i = 0; i < 5; i++)
            if (ptInRect(plRangeTabRc_[i], x, y)) {
                if (plRangePreset_ != kRangeTabs[i].preset) {
                    plRangePreset_ = kRangeTabs[i].preset;
                    loadPlaylist(plKind_);   // the range IS the query
                    // ...and the tile behind it is that query's top four, so
                    // the grid would still be showing the old range's covers.
                    loadPlaylistCovers();
                }
                return;
            }
    }

    int row = hitTestListRows(plListRows_, x, y);
    if (row >= 0) playPlaylistFrom(row);
}

// ── Folder picker panel (also reached via "Add Music Folder") ───────────────
// Replaces SHBrowseForFolderW on BOTH platforms — not just stubbed on Linux —
// per the decision to keep every OS-chrome surface out of this otherwise
// fully custom-rendered app (see CLAUDE.md's design-decisions table).

void PlayerWindow::fpLoadDir(const std::string& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path p = dir.empty() ? fs::path(userHomeDir()) : fs::weakly_canonical(fs::path(dir), ec);
    if (ec) p = fs::path(userHomeDir());

    fpCurrentDir_ = p.string();
    fpEntries_.clear();
    std::error_code iterEc;
    for (auto it = fs::directory_iterator(p, fs::directory_options::skip_permission_denied, iterEc);
         !iterEc && it != fs::directory_iterator(); it.increment(iterEc)) {
        std::error_code isDirEc;
        if (it->is_directory(isDirEc) && !isDirEc) {
            std::string name = it->path().filename().string();
            if (!name.empty() && name[0] != '.')  // hide dotfiles, matches typical folder pickers
                fpEntries_.push_back(name);
        }
    }
    std::sort(fpEntries_.begin(), fpEntries_.end(), [](const std::string& a, const std::string& b) {
        return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
            [](char x, char y) { return std::tolower((unsigned char)x) < std::tolower((unsigned char)y); });
    });
    fpHasParent_ = (p.parent_path() != p);
    fpScrollY_ = 0;
    fpHoverRow_ = -1;
}

void PlayerWindow::commitAddFolder(const std::string& root) {
    db_.addMusicRoot(root);
    watcher_.watchRoot(root, [this](const std::string&) {
        host_->postAppEvent(AppEvent::ScanDone, 1);
    });
    StreamerDb sdb;
    sdb.open(root);  // no-op if this root has no sibling .streamer db
    streamerDbs_[root] = std::move(sdb);
    startBackgroundScan();
}

void PlayerWindow::onAddFolder() {
    fpLoadDir(fpCurrentDir_.empty() ? userHomeDir() : fpCurrentDir_);
    fpHoverClose_ = fpHoverSelect_ = fpHoverCancel_ = false;
    activePanel_ = SettingsPanel::FolderPicker;
    invalidate();
}

void PlayerWindow::drawFolderPicker(Canvas& canvas, const LayoutRect& area) {
    LayoutRect content = panels::drawHeader(canvas, area, "Select Music Folder", metrics_.scale, metrics_.text.header, fpCloseRc_);
    Rect c = toRect(content);
    float pad = metrics_.space(SP_LG);

    // A filesystem path is machine text (same rule as the EQ device line). The
    // measure passed to truncateToWidth MUST use the style it is drawn in, or
    // the ellipsis lands at the wrong character.
    canvas.textStyled(truncateToWidth(canvas, fpCurrentDir_, c.w - 2.0f * pad, metrics_.text.secondary, FontStyle::Math),
                      c.x + pad, c.y + pad, metrics_.text.secondary, toColor(CLR_TEXT_DIM), FontStyle::Math);

    float listTop = pad * 2.0f + metrics_.text.secondary * 1.4f;
    float btnH = metrics_.space(58.0f);
    LayoutRect listArea = { content.left, (int)(content.top + listTop),
                            content.right, (int)(content.bottom - (btnH + pad * 2.0f)) };
    fpListArea_ = listArea;

    std::vector<std::string> labels;
    labels.reserve(fpEntries_.size() + 1);
    if (fpHasParent_) labels.push_back(".. (parent folder)");
    labels.insert(labels.end(), fpEntries_.begin(), fpEntries_.end());

    float fpRowH = panelRowH();
    fpListRows_ = widgets::drawScrollList(canvas, toRect(listArea), labels,
                                          -1, (float)fpScrollY_, fpRowH,
                                          fpHoverRow_, widgets::kTextFree, matrixListStyle());
    panels::drawScrollbar(canvas, listArea, (int)((float)labels.size() * fpRowH), fpScrollY_, metrics_.scale);
    if (labels.empty()) {
        Rect a = toRect(listArea);
        canvas.textStyled("No subfolders here.", a.x + metrics_.space(22.0f), a.y + metrics_.space(22.0f),
                          metrics_.text.body, toColor(CLR_TEXT_DIM), FontStyle::Italic);
    }

    float btnW = metrics_.space(326.0f);
    int by = (int)(content.bottom - (btnH + pad));
    auto fpRects = panels::layoutEdgePair(
        content, pad, btnW, btnW,
        metrics_.space(panels::kMinActionBtnW), metrics_.space(SP_MD), by, (int)btnH);
    fpBtnCancel_ = fpRects.first;
    fpBtnSelect_ = fpRects.second;
    panels::drawButton(canvas, fpBtnCancel_, "Cancel", fpHoverCancel_, metrics_.text.body);
    panels::drawButton(canvas, fpBtnSelect_, "Select This Folder", fpHoverSelect_, metrics_.text.body, true);
}

// ── Album / Track selection (simplified for custom UI) ──────────────────────

void PlayerWindow::onAlbumSelected(int idx) {
    selectedAlbumIdx_ = idx;
    trackPanelOpen_ = true;
    trackScrollY_ = 0;
    loadTrackPanelArtTexture(idx);
    recalcLayout();
    invalidate();
}

void PlayerWindow::onTrackSelected(int idx) {
    currentTrack_ = idx;
}

// ── Playback ─────────────────────────────────────────────────────────────────

std::string PlayerWindow::getActiveDeviceKey() {
    switch (audioBackend_) {
    case AudioBackend::Wasapi: {
        auto devId = db_.loadSetting("wasapi_device_id");
        return devId.empty() ? "wasapi" : "wasapi:" + devId;
    }
    case AudioBackend::Alsa: return "alsa";
    case AudioBackend::Jack: return "jack";
    case AudioBackend::Usb:
    default: {
        auto vid = db_.loadSetting("usb_vid");
        auto pid = db_.loadSetting("usb_pid");
        if (vid.empty()) vid = "32BB";
        if (pid.empty()) pid = "0004";
        return vid + ":" + pid;
    }
    }
}

std::string PlayerWindow::audioBackendLabel() const {
    switch (audioBackend_) {
    case AudioBackend::Wasapi: return "WASAPI";
    case AudioBackend::Alsa:   return "ALSA";
    case AudioBackend::Jack:   return "JACK";
    case AudioBackend::Usb:
    default:                   return "USB";
    }
}

void PlayerWindow::applyDeviceEq(int sampleRate, int channels) {
    std::string key = getActiveDeviceKey();
    EqAssignment assign;
    if (!db_.loadEqAssignment(key, assign) &&
        !db_.loadEqAssignment("global", assign)) {
        eqManager_.clear();
        eqCurrent_ = {};
        eqCurrentTentative_ = false;
        return;
    }
    // Deliberately AFTER the early return above, not at the top of the
    // function. This is the one reader on the audio path — it runs at every
    // track start — and a listener with no profile assigned has no reason to
    // wait for a database they are not using.
    ensureEqProfiles();
    auto* profile = eqProfiles_.findByKey(assign.name, assign.source, assign.form);
    if (profile) {
        eqManager_.applyProfile(profile, sampleRate, channels);
        eqCurrent_ = assign;
        // A profile that survived a restart without ever earning its minute is
        // still on trial. The ASSIGNMENT persists deliberately — dropping it
        // would silently kill the listener's EQ if they closed the app 40
        // seconds in — but the list membership has to be earned all the same.
        eqCurrentTentative_ = !isKnownHeadphone(assign);
    } else {
        eqManager_.clear();
        eqCurrent_ = {};
        eqCurrentTentative_ = false;
    }
}

// ── Headphone quick-switcher ─────────────────────────────────────────────────

void PlayerWindow::reloadEqHeadphones() {
    eqHeadphones_ = db_.loadEqHeadphones(64);
    if (!eqCurrent_.name.empty())
        eqCurrentTentative_ = !isKnownHeadphone(eqCurrent_);
}

bool PlayerWindow::isKnownHeadphone(const EqAssignment& a) const {
    for (const auto& h : eqHeadphones_)
        if (h.name == a.name && h.source == a.source && h.form == a.form)
            return true;
    return false;
}

// Bottom-anchored inside the sidebar: a header, the "No AutoEQ" row, up to
// kEqHpMaxRows saved pairs (plus the on-trial one, which is extra rather than
// displacing a saved row), then a link into the full catalogue. Rects are
// computed here and cached for hit-testing — the same contract eqListRows_
// uses — so the block follows the mode toggle and the list contents without
// anything hanging off recalcLayout().
void PlayerWindow::drawHeadphoneBlock(Canvas& canvas, const LayoutRect& sidebar) {
    hpRows_.clear();
    hpNoneRc_ = {};
    hpMoreRc_ = {};

    // Nothing to pick a profile FOR in bitperfect mode — the signal path is
    // untouched by definition, so the block would be a control that does
    // nothing. Hidden, not greyed: a disabled row still asks to be read.
    if (bitperfectMode_.load()) return;

    Rect sb = toRect(sidebar);
    const float pad     = metrics_.space(SP_SM);
    // space(36), not the 48 this block was authored with. Below Settings the
    // sidebar has room for three rows at 48 and four at 36 (the nav above is
    // eight space(65.36) rows), so at the authored height the "No AutoEQ" row
    // would have been paid for by a saved pair. At secondary-sized text this is
    // still 2.0x leading — the same order as the header's 2.2x — and it keeps
    // two saved pairs reachable in one click, which is the point of the block.
    const float rowH    = metrics_.space(36.0f);
    const float headerH = metrics_.text.secondary * 2.2f;

    // The nav must win if the window is short enough for the two to meet —
    // browsing the library is the app's primary job, EQ housekeeping is not.
    // But what gives way is the SAVED LIST, not the whole block: the header,
    // "No AutoEQ" and "Search more…" are the minimum, because a pair that
    // doesn't fit is still one click away under the latter while the off switch
    // has nowhere else to live. Clamping here instead of bailing is what stops the
    // block from disappearing outright precisely when the list is full: with a
    // full four saved pairs the old all-or-nothing test failed its budget and
    // drew NOTHING — no switcher, no way off, at the one moment the listener
    // has the most to switch between.
    const float avail =
        (sb.y + sb.h) - (rcNavSettings_.bottom + metrics_.space(SP_MD));
    const int listCapacity =
        (int)((avail - headerH - pad * 2.0f) / rowH) - 2;   // less No AutoEQ + Search more…
    if (listCapacity < 1) return;

    // The on-trial profile heads the SAVED LIST (below "No AutoEQ", above the
    // earned rows) and does NOT count against kEqHpMaxRows: it is what the
    // listener just picked, so hiding it to preserve a saved row would hide the
    // one thing they are looking for. It does count against the space that
    // actually exists, though.
    const bool showTrial = eqCurrentTentative_ && !eqCurrent_.name.empty();
    int saved = (int)eqHeadphones_.size();
    if (saved > kEqHpMaxRows) saved = kEqHpMaxRows;
    if (saved > listCapacity - (showTrial ? 1 : 0))
        saved = listCapacity - (showTrial ? 1 : 0);
    if (saved < 0) saved = 0;
    const int totalRows = saved + (showTrial ? 1 : 0);
    // An empty list still occupies one row ("Nothing saved yet"), so the height
    // budget has to count it or the block overflows past the bottom of the
    // sidebar.
    const int listRows  = totalRows > 0 ? totalRows : 1;

    // Header + the two fixed rows ("No AutoEQ", "Search more…") + the list +
    // padding above and below. Fits by construction now.
    const float blockH = headerH + (listRows + 2) * rowH + pad * 2.0f;
    float y = sb.y + sb.h - blockH;

    canvas.rect(sb.x, y, sb.w, metrics_.stroke(1.0f), toColor(CLR_SEPARATOR));
    y += pad;

    // Deliberately NOT truncated: a section header that quietly loses its tail
    // hides a fit failure. "DRIVER'S AUTOEQ" is measured against space(277)
    // minus the space(20) inset in the capture shot, not assumed to fit.
    canvas.textStyled("DRIVER'S AUTOEQ", sb.x + metrics_.space(20.0f), y,
                      metrics_.text.secondary, toColor(CLR_TEXT_DIM), FontStyle::Bold);
    y += headerH;

    const float textX   = sb.x + metrics_.space(20.0f);
    const float maxTextW = sb.w - metrics_.space(20.0f) - metrics_.space(SP_SM);

    auto drawRow = [&](const std::string& label, bool active, bool trial,
                       bool hovered, float ry) {
        const float pillX = sb.x + metrics_.space(4.0f);
        const float pillW = sb.w - metrics_.space(8.0f);
        // Accent = state, hover = neutral grey (UI_DESIGN_SYSTEM.md §1.4) —
        // the same selection family as the nav rows above.
        if (active) {
            canvas.rect(pillX, ry, pillW, rowH,
                        toColor(CLR_ACCENT, UI_SELECT_TINT_ALPHA), UI_CORNER_RADIUS);
            canvas.rect(pillX, ry, metrics_.stroke(3.0f), rowH,
                        toColor(CLR_ACCENT), UI_CORNER_RADIUS);
        } else if (hovered) {
            canvas.rect(pillX, ry, pillW, rowH, toColor(CLR_HOVER), UI_CORNER_RADIUS);
        }
        // On trial: dim + italic. The EQ is already audible; what is pending is
        // only whether this pair keeps a row.
        const FontStyle style = trial ? FontStyle::Italic : FontStyle::Roman;
        const ColorRef  clr   = trial   ? CLR_TEXT_DIM
                              : active  ? CLR_ACCENT
                                        : CLR_TEXT_SECONDARY;
        // Profile names run long ("Sennheiser HD 600 (over-ear)") and the
        // sidebar is only space(277). Without this they paint over the grid.
        canvas.textStyled(truncateToWidth(canvas, label, maxTextW,
                                          metrics_.text.secondary, style),
                          textX, ry + rowH * 0.5f - metrics_.text.secondary * 0.5f,
                          metrics_.text.secondary, toColor(clr), style);
    };

    const int rowLeft  = sidebar.left;
    const int rowRight = sidebar.right;

    // The OFF position of the switch, at the HEAD of the block — the radio-list
    // convention, where "none of these" is the first choice rather than a
    // footnote after them. It is never scrolled or clamped away: the saved list
    // below is what gives way when the sidebar runs short.
    //
    // Active whenever nothing is assigned, so "no AutoEQ" is a state the block
    // SHOWS rather than the absence of any highlight at all.
    {
        const bool none    = eqCurrent_.name.empty();
        const bool hovered = (hoverSidebarItem_ == kSidebarHpNoneHit) && !none;
        drawRow("No AutoEQ", /*active=*/none, /*trial=*/false, hovered, y);
        hpNoneRc_ = { rowLeft, (int)y, rowRight, (int)(y + rowH) };
        y += rowH;
    }

    int rowIdx = 0;
    if (showTrial) {
        const bool hovered = (hoverSidebarItem_ == kSidebarHpRowBase + rowIdx);
        drawRow(eqCurrent_.name, /*active=*/true, /*trial=*/true, hovered, y);
        hpRows_.push_back({ { rowLeft, (int)y, rowRight, (int)(y + rowH) }, -1 });
        y += rowH;
        rowIdx++;
    }
    for (int i = 0; i < saved; i++) {
        const auto& h = eqHeadphones_[i];
        const bool active = (h.name   == eqCurrent_.name &&
                             h.source == eqCurrent_.source &&
                             h.form   == eqCurrent_.form);
        const bool hovered = (hoverSidebarItem_ == kSidebarHpRowBase + rowIdx) && !active;
        drawRow(h.name, active, /*trial=*/false, hovered, y);
        hpRows_.push_back({ { rowLeft, (int)y, rowRight, (int)(y + rowH) }, i });
        y += rowH;
        rowIdx++;
    }

    if (totalRows == 0) {
        // "Nothing saved yet", not "None yet": the row directly above is the
        // OFF switch, and two adjacent lines both starting "None" read as a
        // pair of choices rather than a control and a placeholder.
        canvas.textStyled("Nothing saved yet", textX,
                          y + rowH * 0.5f - metrics_.text.secondary * 0.5f,
                          metrics_.text.secondary, toColor(CLR_TEXT_DIM), FontStyle::Italic);
        y += rowH;
    }

    // The escape hatch into the full catalogue. Sized as a row so it lines up
    // with the ones above rather than floating.
    {
        const bool hovered = (hoverSidebarItem_ == kSidebarHpMoreHit);
        if (hovered) {
            canvas.rect(sb.x + metrics_.space(4.0f), y, sb.w - metrics_.space(8.0f),
                        rowH, toColor(CLR_HOVER), UI_CORNER_RADIUS);
        }
        canvas.textStyled("Search more\xE2\x80\xA6", textX,
                          y + rowH * 0.5f - metrics_.text.secondary * 0.5f,
                          metrics_.text.secondary, toColor(CLR_TEXT_DIM), FontStyle::Roman);
        hpMoreRc_ = { rowLeft, (int)y, rowRight, (int)(y + rowH) };
    }
}

void PlayerWindow::selectEqProfile(const EqAssignment& a) {
    ensureEqProfiles();
    const EqProfile* profile = eqProfiles_.findByKey(a.name, a.source, a.form);
    if (!profile) return;

    db_.saveEqAssignment(getActiveDeviceKey(), a.name, a.source, a.form);
    markEqAssignmentDirty();
    eqCurrent_ = a;
    eqCurrentTentative_ = !isKnownHeadphone(a);

    // The new profile starts its own minute. statsMsHeard_ belongs to the
    // TRACK and is already well past zero mid-playback, so without this
    // baseline a swap late in a song would credit the new pair instantly.
    eqCreditedThisTrack_ = false;
    eqCreditBaselineMs_  = statsMsHeard_;

    // Bitperfect deliberately leaves the engine alone: the assignment is
    // recorded and takes effect the moment Reference EQ comes back.
    if (!bitperfectMode_.load()) {
        int sr = 44100, ch = 2;
        if (output_) {
            int r = output_->getConfiguredRate(), c = output_->getConfiguredChannels();
            if (r > 0) sr = r;
            if (c > 0) ch = c;
        }
        // EqManager swaps into its back buffer and flips an atomic, so this is
        // safe with audio running — the change lands on the next chunk, not
        // the next track (unlike the bitperfect toggle, which cannot be
        // switched mid-stream).
        eqManager_.applyProfile(profile, sr, ch);
    }
    invalidate();
}

// The other half of selectEqProfile(): the sidebar's "No AutoEQ" row and the EQ
// panel's Clear button, one implementation.
//
// The "global" delete is the load-bearing line. applyDeviceEq() runs at EVERY
// track start and falls back to a "global" assignment when the active device has
// none — so clearing the device key alone would let such a row resurrect the
// profile on the next song, and "No AutoEQ" would look broken rather than wrong.
// Nothing in this app has ever WRITTEN "global" (saveEqAssignment has exactly one
// call site, selectEqProfile, and it always passes getActiveDeviceKey()), so a
// row under that key can only be legacy: deleting it destroys nothing the UI is
// able to recreate.
void PlayerWindow::clearEqProfile() {
    db_.clearEqAssignment(getActiveDeviceKey());
    db_.clearEqAssignment("global");
    markEqAssignmentDirty();
    eqManager_.clear();
    eqCurrent_ = {};
    eqCurrentTentative_ = false;
    invalidate();
}

void PlayerWindow::onPlay(StartCause cause) {
    // A fresh play attempt always dismisses a stale bitperfect warning,
    // including the stale-selection early-return path just below.
    audioNotice_.clear();
    // Likewise the achieved-state badge: it must never describe a past track.
    bpState_ = BpState::Off;
    bpDetail_.clear();
    Track t;
    {
        std::lock_guard<std::mutex> lk(albumsMu_);
        if (currentAlbum_ < 0 || currentAlbum_ >= (int)albums_.size() ||
            currentTrack_ < 0 || currentTrack_ >= (int)albums_[currentAlbum_].tracks.size()) {
            printf("[Play][ERROR] stale album=%d track=%d (albums=%zu), aborting\n",
                   currentAlbum_, currentTrack_, albums_.size());
            fflush(stdout);
            return;
        }
        t = albums_[currentAlbum_].tracks[currentTrack_];
    }
    printf("[Play] album=%d track=%d path='%s'\n",
           currentAlbum_, currentTrack_, t.filePath.c_str());
    fflush(stdout);

    {
        std::lock_guard<std::mutex> lk(gaplessMu_);
        stopGapless_.store(true);
        gaplessSignal_ = false;
        gaplessCv_.notify_one();
    }
    if (gaplessThread_.joinable()) gaplessThread_.join();
    stopGapless_.store(false);

    // Stop the sink BEFORE joining the decode threads: the decode thread
    // spends most of steady playback blocked inside output_->writeXBlocking()
    // waiting for ring space, which only unblocks early once the output's own
    // running_ flag drops / hDrainEvent_ fires. Joining decoder_/nextDecoder_
    // first can stall the UI thread for the full write timeout (30s).
    if (output_) output_->stop();
    decoder_.stop();
    nextDecoder_.close();
    active_ = &decoder_;

    if (!decoder_.open(t.filePath)) return;

    // Stats before the UI state: beginTrackStats() banks the OUTGOING track,
    // and that needs seekPosMs_ as it stands now — it is zeroed a few lines
    // below for the new stream.
    //
    // Two paths reach here that are not the listener choosing a track, and
    // both have to say so or the log records a decision nobody made:
    if (playFromGapless_.exchange(false)) {
        // The previous track ran out and the next one needs a different device
        // format, so the coordinator bounced the restart through here. The
        // outgoing track finished; it was not replaced.
        flushTrackStats(EndCause::Natural);
        cause = StartCause::Gapless;
    }
    // StartCause::Resume has no producer any more (the resume it recorded was
    // removed — see create()). The value stays in the enum: play_events rows
    // already on disk carry it, and renumbering would relabel every one of
    // them. Same standing as StartCause::Shuffle, which has never had one.
    // A playlist overrides all of the above, Manual included. What matters
    // downstream is only that the play came OUT of a playlist: picking a row,
    // pressing Next in one, or shuffling it are all the same fact, and none of
    // them may feed the ranking the list was built from. See IS_AFFINITY.
    if (queueActive()) cause = StartCause::Playlist;
    beginTrackStats(t, cause);

    // Update UI state. Fresh stream: display cursor == decode cursor, and the
    // played-frames timeline restarts from zero.
    displayAlbum_ = currentAlbum_;
    displayTrack_ = currentTrack_;
    currentTitle_ = t.title;
    currentArtist_ = t.artist;

    lastPlayedAlbumName_  = albums_[currentAlbum_].name;
    lastPlayedArtistName_ = albums_[currentAlbum_].artist;
    db_.saveSetting("last_played_album", lastPlayedAlbumName_ + "\x1f" + lastPlayedArtistName_);

    int durationSec = t.durationMs > 0 ? t.durationMs / 1000
                    : static_cast<int>(active_->totalFrames() / (active_->sampleRate() ? active_->sampleRate() : 44100));
    seekTotalMs_ = durationSec * 1000;
    seekPosMs_ = 0;
    playedFrames_.store(0);
    displayTrackStartFrame_ = 0;
    { std::lock_guard<std::mutex> lk(boundariesMu_); boundaries_.clear(); }
    isPlaying_ = true;

    // Load transport art
    if (currentAlbum_ >= 0 && currentAlbum_ < (int)albums_.size()) {
        loadTransportArtTexture(albums_[currentAlbum_].artPath);
    }

    // Focus the album view on what's now playing — but never yank the page out
    // from under someone who is BROWSING. Playing a track from a playlist is
    // not a request to leave the playlist, any more than playing a track from
    // an album view is a request to leave it; the transport bar already says
    // what is playing. This was the whole shape of the old complaint: start a
    // track from a list and the app jumped to that track's album, so leaving
    // the list twice put you somewhere you had never navigated to.
    if (selectedAlbumIdx_ != currentAlbum_ && navSection_ != NavSection::Playlists)
        openAlbumView(currentAlbum_);

    invalidate();

    // output_ was already stopped above (before decoder_.stop()/join()).
    int fileSr = active_->sampleRate();
    int outSr  = fileSr;
    bool isBitperfect = bitperfectMode_.load();
    // Bit-perfect: request the file's native depth so the DAC negotiates a matched
    // format (16->16, 24->24). configure() auto-relaxes to the highest available
    // depth if the exact one is unsupported; dr_flac's s32 output is left-justified,
    // so feeding it into a wider slot stays bit-exact. Normal path always uses 32.
    int reqBits = isBitperfect ? active_->bitsPerSample() : 32;
    bool cfgOk = output_->configure(fileSr, active_->channels(), reqBits, isBitperfect);
    if (!cfgOk && !isBitperfect) {
        // Negotiate the best rate the device supports (USB: descriptor; every
        // other backend: its own probeRates(), empty/unknown by default).
        std::vector<int> supported = (audioBackend_ == AudioBackend::Usb)
            ? usbDriver_.getOutputRates()
            : output_->probeRates(active_->channels());
        outSr = pickOutputRate(fileSr, supported);
        cfgOk = output_->configure(outSr, active_->channels(), 32, false);
        printf("[Audio][WARN] %d Hz unsupported -> negotiated %d Hz\n", fileSr, outSr);
        fflush(stdout);
    }
    if (!cfgOk) {
        if (isBitperfect) {
            printf("[Bitperfect][ERROR] DAC does not support native sample rate %d Hz, aborting\n", fileSr);
            fflush(stdout);
            audioNotice_ = "DAC does not support native sample rate " + std::to_string(fileSr) +
                                  " Hz — Bitperfect playback aborted.";
            bpState_  = BpState::Degraded;
            bpDetail_ = "Device cannot run " + std::to_string(fileSr) + " Hz natively";
            invalidate();
        } else {
            // Say WHICH backend and WHY. The old message named neither, and on
            // Linux it only reached stderr — so an ALSA card held by PipeWire
            // looked exactly like the app ignoring the click. audioNotice_ puts
            // it on screen; lastError() carries the driver's own words.
            const std::string why = output_ ? output_->lastError() : std::string();
            printf("[Audio][ERROR] %s output failed to configure at %d Hz%s%s\n",
                   audioBackendLabel().c_str(), fileSr,
                   why.empty() ? "" : ": ", why.c_str());
            fflush(stdout);
            audioNotice_ = audioBackendLabel() + " output failed to start" +
                           (why.empty() ? std::string(" at ") + std::to_string(fileSr) + " Hz."
                                        : std::string(" \xE2\x80\x94 ") + why + ".");
            invalidate();
        }
        active_->stop();
        isPlaying_ = false;
        return;
    }
    outSr = output_->getConfiguredRate();

    // Query device's maximum supported bit depth for the final quantize step.
    int deviceMaxBits = 32;
    if (audioBackend_ == AudioBackend::Usb) {
        deviceMaxBits = usbDriver_.getConfiguredBitDepth();
        if (deviceMaxBits <= 0) deviceMaxBits = 32;
    }
#ifdef _WIN32
    else if (audioBackend_ == AudioBackend::Wasapi && !isBitperfect) {
        auto* wasapi = static_cast<WasapiOutput*>(output_.get());
        deviceMaxBits = wasapi->getMaxBitDepth(outSr, active_->channels());
        if (deviceMaxBits <= 0) deviceMaxBits = 32;
    }
#endif
    printf("[Audio] device max bit depth: %d\n", deviceMaxBits);
    fflush(stdout);

    // ── Report what the chain ACTUALLY achieves, never what was merely asked ──
    // Everything needed is known only here: the negotiated rate and the device's
    // real bit depth. Each branch also states its reason, so the hover readout
    // can explain itself rather than leaving the user to infer it.
    {
        const int fileBits = active_->bitsPerSample();
        char why[192];
        if (!isBitperfect) {
            bpState_ = BpState::Off;
            if (fileSr != outSr)
                snprintf(why, sizeof(why), "Reference EQ — resampling %d \xE2\x86\x92 %d Hz", fileSr, outSr);
            else
                snprintf(why, sizeof(why), "Reference EQ — %d Hz, no resampling", outSr);
        } else if (deviceMaxBits < fileBits) {
            // The one real loss bit-perfect mode can still hit: a source deeper
            // than the device. configure() relaxes the depth rather than failing.
            bpState_ = BpState::Degraded;
            snprintf(why, sizeof(why), "NOT bit-perfect — %d-bit source truncated to %d-bit device",
                     fileBits, deviceMaxBits);
        } else if (audioBackend_ == AudioBackend::Jack) {
            // Our samples reach jackd intact (float32 carries 24 bits exactly),
            // but the server owns the final conversion to the device and may be
            // mixing other clients. Not something this process can verify.
            bpState_ = BpState::ViaServer;
            snprintf(why, sizeof(why), "Bit-exact to JACK (%d-bit @ %d Hz); the server owns final conversion",
                     fileBits, outSr);
        } else {
            bpState_ = BpState::Exact;
            snprintf(why, sizeof(why), "Bit-perfect — %d-bit @ %d Hz, sample-for-sample", fileBits, outSr);
        }
        bpDetail_ = why;
        printf("[Audio] signal path: %s\n", bpDetail_.c_str());
        fflush(stdout);
    }

    int capturedOutSr  = outSr;
    int capturedFileSr = fileSr;
    int capturedDacCh  = output_->getConfiguredChannels();
    int capturedBits   = deviceMaxBits;
    auto* outPtr = output_.get();

    // Both modes decode the lossless int32 stream; Bit-Perfect writes it verbatim,
    // Reference EQ applies parametric EQ in place (double math, single rounded snap)
    // before the same int32 write. Exactly one branch below populates this callback.
    PcmS32Callback callbackI32;

  if (isBitperfect) {
    eqManager_.clear();
    if (capturedFileSr != capturedOutSr)
        printf("[Bitperfect][WARN] rate mismatch %d->%d should have aborted configure\n",
               capturedFileSr, capturedOutSr);
    printf("[Bitperfect] lossless int32 path: %d-bit source @ %d Hz -> DAC %d-bit\n",
           active_->bitsPerSample(), capturedOutSr, usbDriver_.getConfiguredBitDepth());
    callbackI32 = [this, outPtr](const int32_t* d, int n) {
        if (d == nullptr || n == 0) return;
        int srcCh  = active_->channels();
        int frames = srcCh > 0 ? n / srcCh : n;
        int got = outPtr->writeInt32Blocking(d, n);
        if (got < n) {
            static int64_t lastShortLog = 0;
            int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if ((nowMs - lastShortLog) >= 1000) {
                printf("[Bitperfect][WARN] short write: wanted=%d got=%d\n", n, got);
                fflush(stdout);
                lastShortLog = nowMs;
            }
        }
        playedFrames_.fetch_add(frames, std::memory_order_relaxed);
    };
  } else {
    // Reference EQ pipeline:
    // - Rates match: EQ in 64-bit double, single quantize to int32, write. (fast path)
    // - Rates differ: EQ to double, soxr FLOAT64_I VHQ resample, TPDF dither +
    //   quantize once to device's max bit depth. (single quantization point)
    applyDeviceEq(capturedOutSr, active_->channels());
    printf("[ReferenceEQ] %d-bit source @ %d Hz -> device %d-bit @ %d Hz, EQ %s\n",
           active_->bitsPerSample(), capturedFileSr, capturedBits, capturedOutSr,
           eqManager_.isActive() ? "active" : "bypass");
    fflush(stdout);

    const bool needsResample = (capturedFileSr != capturedOutSr);
    const int  srcCh         = active_->channels();

    // Pre-allocate buffers outside the lambda — never allocate on the audio thread.
    // kDecodeChunk is the decoder's typical output frame count per callback.
    const int kDecodeChunk = 4096;
    const size_t kMaxInFrames = (size_t)(kDecodeChunk + 256);
    auto eqBuf = std::make_shared<std::vector<double>>(kMaxInFrames * srcCh);

    // soxr's output gets its OWN buffer sized by the actual rate ratio.
    //
    // It used to be the second half of eqBuf — the same size as the input half —
    // so soxr was handed an output capacity of kMaxInFrames regardless of ratio.
    // Upsampling needs MORE room than the input, and soxr only consumes as much
    // input as it can emit, so it stopped early and the loop ignored idone: the
    // rest of the chunk was dropped on the floor. At 44.1k -> 96k that discarded
    // over half of every chunk, permanently. Rates that matched the server took
    // the !needsResample fast path and were unaffected, which is why only some
    // tracks in a mixed album broke.
    const double rsRatio = (double)capturedOutSr / (double)capturedFileSr;
    const size_t rsFrames = (size_t)std::ceil(kMaxInFrames * rsRatio) + 64;  // +64 slack
    auto rsBuf = std::make_shared<std::vector<double>>(needsResample ? rsFrames * srcCh : 0);

    size_t outBufSz = needsResample ? rsFrames * srcCh : kMaxInFrames * srcCh;
    auto outBuf = std::make_shared<std::vector<int32_t>>(outBufSz);

    std::shared_ptr<void> resamplerPtr;
    if (needsResample) {
        soxr_io_spec_t  io = soxr_io_spec(SOXR_FLOAT64_I, SOXR_FLOAT64_I);
        soxr_quality_spec_t q = soxr_quality_spec(SOXR_VHQ, 0); // 28-bit, ~140 dB SNR
        soxr_t r = soxr_create((double)capturedFileSr, (double)capturedOutSr, srcCh,
                               nullptr, &io, &q, nullptr);
        if (r) {
            resamplerPtr = std::shared_ptr<void>(r, soxr_delete);
            printf("[ReferenceEQ] soxr VHQ resampler created %d->%d Hz\n",
                   capturedFileSr, capturedOutSr);
        } else {
            printf("[ReferenceEQ][ERROR] soxr_create failed\n");
        }
        fflush(stdout);
    }

    callbackI32 = [this, outPtr, eqBuf, rsBuf, outBuf, resamplerPtr,
                   capturedBits, srcCh, needsResample, kMaxInFrames, rsFrames]
                  (const int32_t* d, int n) {
        if (d == nullptr || n == 0 || srcCh <= 0) return;

        if (!needsResample) {
            // Fast path: rates match — EQ in double, single snap to int32.
            eqManager_.processInPlaceInt32(const_cast<int32_t*>(d), n);
            outPtr->writeInt32Blocking(d, n);
            playedFrames_.fetch_add(n / srcCh, std::memory_order_relaxed);
            return;
        }

        // Resample path: single quantization point.
        // Sliced so a chunk larger than kMaxInFrames can never overrun eqBuf,
        // and looped on idone so no input frame is ever left unconsumed.
        const size_t inFrames = (size_t)n / srcCh;
        for (size_t pos = 0; pos < inFrames; ) {
            const size_t slice = std::min(inFrames - pos, kMaxInFrames);

            // 1. EQ int32 → double (no snap)
            eqManager_.processToDouble(d + pos * srcCh, eqBuf->data(), (int)(slice * srcCh));

            // 2. Resample double → double, draining until this slice is fully fed.
            size_t fed = 0;
            while (fed < slice) {
                size_t idone = 0, odone = 0;
                soxr_process(static_cast<soxr_t>(resamplerPtr.get()),
                             eqBuf->data() + fed * srcCh, slice - fed, &idone,
                             rsBuf->data(), rsFrames, &odone);
                if (idone == 0 && odone == 0) break;   // no progress: don't spin
                fed += idone;

                if (odone > 0) {
                    const int resampN = (int)(odone * srcCh);
                    // 3. TPDF dither + quantize once to the device's max bit depth
                    ditherAndQuantize(rsBuf->data(), outBuf->data(), resampN, capturedBits, srcCh);
                    outPtr->writeInt32Blocking(outBuf->data(), resampN);
                    playedFrames_.fetch_add(odone, std::memory_order_relaxed);
                }
            }
            pos += slice;
        }
    };

  } // end Reference EQ setup

    prepareNextTrack();
    active_->setDoneCallback([this] {
        std::lock_guard<std::mutex> lk(gaplessMu_);
        gaplessSignal_ = true;
        gaplessCv_.notify_one();
    });

    // Both Bit-Perfect and Reference EQ decode via the lossless int32 path.
    active_->startAsyncInt32(callbackI32);

    int preBufferSamples = output_->getPreBufferSamples();
    if (!output_->waitForData(preBufferSamples, 2000))
        printf("[Audio][WARN] pre-buffer incomplete (%zu / %d samples)\n",
               output_->ringAvailable(), preBufferSamples);

    bool startOk = output_->start();
    printf("[onPlay] output_->start() returned %s\n", startOk ? "true" : "false");
    fflush(stdout);
    if (!startOk) {
        printf("[onPlay][ERROR] Audio output failed to start\n");
        fflush(stdout);
        audioNotice_ = audioBackendLabel() + " output failed to start \xE2\x80\x94 check Audio Settings.";
        invalidate();
        active_->stop();
        isPlaying_ = false;
        return;
    }
    printf("[onPlay] USB streaming started, ring=%zu\n", output_->ringAvailable());
    fflush(stdout);

    startGaplessCoordinator(callbackI32, capturedOutSr, capturedDacCh);
    host_->startTimer(TimerId::SeekUpdate, 250);

    // Nothing seeks here. Every track starts at zero, including the one that
    // was playing when the app last closed — see create(). onSeek() survives
    // for onPrev()'s restart-this-track path, which is a return to the
    // beginning, not a jump into the middle.
}

void PlayerWindow::onStop() {
    // Before anything resets the clock: both of these read seekPosMs_.
    savePlaybackStateNow();
    flushTrackStats(EndCause::Stop);
    {
        std::lock_guard<std::mutex> lk(gaplessMu_);
        stopGapless_.store(true);
        gaplessSignal_ = false;
        gaplessCv_.notify_one();
    }
    if (gaplessThread_.joinable()) gaplessThread_.join();
    stopGapless_.store(false);

    // Nothing is playing, so the badge falls back to showing the mode toggle.
    bpState_ = BpState::Off;
    bpDetail_.clear();

    // Stop the sink BEFORE joining the decode threads — see onPlay() for why:
    // the decode thread is typically blocked inside output_->writeXBlocking(),
    // which only unblocks promptly once the output itself is stopped.
    if (output_) output_->stop();
    decoder_.stop();
    nextDecoder_.close();
    active_ = &decoder_;
    nextAlbum_ = nextTrack_ = -1;
    host_->stopTimer(TimerId::SeekUpdate);
    isPlaying_ = false;
    playedFrames_.store(0);
    displayTrackStartFrame_ = 0;
    { std::lock_guard<std::mutex> lk(boundariesMu_); boundaries_.clear(); }
    invalidate();
}

// Section walks. See the declarations in player_view.hh for why the playing
// album's own releaseType is the anchor rather than albumTypeFilter_.
int PlayerWindow::nextAlbumInSection(int album) const {
    if (album < 0 || album >= (int)albums_.size()) return -1;
    const auto type = albums_[album].releaseType;
    for (int i = album + 1; i < (int)albums_.size(); i++)
        if (albums_[i].releaseType == type && !albums_[i].tracks.empty()) return i;
    return -1;
}

int PlayerWindow::prevAlbumInSection(int album) const {
    if (album < 0 || album >= (int)albums_.size()) return -1;
    const auto type = albums_[album].releaseType;
    for (int i = album - 1; i >= 0; i--)
        if (albums_[i].releaseType == type && !albums_[i].tracks.empty()) return i;
    return -1;
}

// ── The playlist queue ──────────────────────────────────────────────────────
// A generated playlist crosses albums, which (currentAlbum_, currentTrack_ + 1)
// plus nextAlbumInSection() cannot express. When the queue is empty every path
// below falls through to exactly the old behaviour.

bool PlayerWindow::queueActive() const {
    std::lock_guard<std::mutex> lk(albumsMu_);
    return queueActiveLocked();
}

bool PlayerWindow::startQueue(const std::vector<std::string>& keys, int startIndex) {
    std::lock_guard<std::mutex> lk(albumsMu_);
    // Lay the keys down unresolved and let reresolveQueueLocked() do the
    // resolving — it is the same job it does after a rescan, and one resolver
    // means "what happens to a key the library no longer holds" is decided in
    // exactly one place.
    queue_.clear();
    queue_.reserve(keys.size());
    for (const std::string& k : keys) queue_.push_back({ k, -1, -1 });
    queuePos_ = (startIndex >= 0 && startIndex < (int)keys.size()) ? startIndex : 0;

    reresolveQueueLocked();
    if (queue_.empty()) { queuePos_ = -1; return false; }
    currentAlbum_ = queue_[queuePos_].album;
    currentTrack_ = queue_[queuePos_].track;
    return true;
}

void PlayerWindow::clearQueue() {
    std::lock_guard<std::mutex> lk(albumsMu_);
    queue_.clear();
    queuePos_ = -1;
}

void PlayerWindow::reresolveQueueLocked() {
    if (queue_.empty()) return;
    // Point every entry at the copy that should play, and drop the entries
    // whose music the library no longer has. A hole would be worse than an
    // absence: the log keeps deleted music on purpose, but a queue is a list
    // of things to PLAY and a gap in it is just a stall.
    //
    // The entry at queuePos_ is the anchor — after a rescan the listener must
    // still be on the same music, whatever moved around it. If the anchor
    // itself is gone, the queue restarts from the top, which is the only
    // honest answer when the thing it was pointing at no longer exists.
    const std::string anchor = (queuePos_ >= 0 && queuePos_ < (int)queue_.size())
                             ? queue_[queuePos_].trackKey : std::string();
    std::vector<QueueEntry> kept;
    kept.reserve(queue_.size());
    int newPos = 0;
    for (const QueueEntry& e : queue_) {
        auto it = trackKeyIndex_.find(e.trackKey);
        if (it == trackKeyIndex_.end()) continue;
        if (e.trackKey == anchor) newPos = (int)kept.size();
        kept.push_back({ e.trackKey, it->second.first, it->second.second });
    }
    if (kept.empty()) { queue_.clear(); queuePos_ = -1; return; }
    queue_    = std::move(kept);
    queuePos_ = newPos;   // always < kept.size(): it is read from it above
}

void PlayerWindow::prepareNextTrack() {
    {
        // The queue owns "what plays next" whenever it is active.
        //
        // It only READS queuePos_ here. The cursor tracks what is PLAYING, not
        // what has been preloaded, exactly as currentAlbum_/currentTrack_ do
        // beside nextAlbum_/nextTrack_ — so it advances where those advance
        // (the gapless coordinator, onNext, onPrev), never here. Advancing it
        // on preload would put the cursor one track ahead of the audio, and
        // Prev would then skip two.
        bool handled = false;
        std::string preloadPath;
        {
            std::lock_guard<std::mutex> lk(albumsMu_);
            if (queueActiveLocked()) {
                handled = true;
                const int next = queuePos_ + 1;
                nextAlbum_ = nextTrack_ = -1;
                // End of the playlist: stop rather than wrap or spill into the
                // library, mirroring nextAlbumInSection() returning -1 at the
                // end of a section.
                if (next < (int)queue_.size()) {
                    const int a = queue_[next].album, t = queue_[next].track;
                    if (a >= 0 && a < (int)albums_.size() &&
                        t >= 0 && t < (int)albums_[a].tracks.size()) {
                        nextAlbum_  = a;
                        nextTrack_  = t;
                        preloadPath = albums_[a].tracks[t].filePath;
                    }
                }
            }
        }
        // Disk I/O outside the lock, same rule as the album path below: it
        // must not be able to block onScanDone() for the length of a file open.
        if (handled) {
            if (!preloadPath.empty()) {
                Decoder* preload = (active_ == &decoder_) ? &nextDecoder_ : &decoder_;
                preload->close();
                preload->open(preloadPath);
            }
            return;
        }
    }

    int album = currentAlbum_;
    int track = currentTrack_ + 1;
    std::string preloadPath;
    {
        // Called from both the UI thread (onPlay()) and the background
        // gaplessThread_ (see startGaplessCoordinator()) — albumsMu_ keeps
        // this whole index computation atomic w.r.t. onScanDone() reassigning
        // albums_ wholesale on the UI thread mid-lookup.
        std::lock_guard<std::mutex> lk(albumsMu_);
        if (album < 0 || album >= (int)albums_.size()) {
            nextAlbum_ = nextTrack_ = -1;
            return;
        }
        // Exhausted this album — continue with the next one IN THE SAME
        // SECTION. A bare album++ walked albums_, which is the global list
        // (alphabetical, all four release types interleaved), so the queue
        // wandered into whatever sorted next library-wide.
        if (track >= (int)albums_[album].tracks.size()) {
            album = nextAlbumInSection(album);
            track = 0;
        }
        if (album < 0) {           // end of the section — nothing follows
            nextAlbum_ = nextTrack_ = -1;
            return;
        }
        nextAlbum_ = album;
        nextTrack_  = track;
        preloadPath = albums_[nextAlbum_].tracks[nextTrack_].filePath;
    }
    // Disk I/O deliberately happens outside the lock so it can't block
    // onScanDone() for the duration of a file open.
    Decoder* preload = (active_ == &decoder_) ? &nextDecoder_ : &decoder_;
    preload->close();
    preload->open(preloadPath);
}

void PlayerWindow::startGaplessCoordinator(PcmS32Callback cbI32, int outSr, int dacCh) {
    if (gaplessThread_.joinable()) gaplessThread_.join();
    // Capture the mode at play start: gapless continuation reuses the callback
    // built for this mode, so a later toggle only takes effect on the next onPlay.
    bool bitperfect = bitperfectMode_.load();
    gaplessThread_ = std::thread([this, cbI32, bitperfect, outSr, dacCh] {
        while (true) {
            std::unique_lock<std::mutex> lk(gaplessMu_);
            gaplessCv_.wait(lk, [this] { return gaplessSignal_ || stopGapless_.load(); });
            if (stopGapless_.load()) break;
            gaplessSignal_ = false;
            lk.unlock();

            printf("[%s][Gapless] EOF fired: playedFrames=%lld ring_avail=%zu\n",
                   logTs(),
                   (long long)playedFrames_.load(std::memory_order_relaxed),
                   output_->ringAvailable());
            fflush(stdout);

            if (nextAlbum_ < 0) break;

            Decoder* incoming = (active_ == &decoder_) ? &nextDecoder_ : &decoder_;
            // Compare incoming file properties against the *current* decoder, not the
            // WASAPI output rate. In shared mode outSr is the OS mix rate (often
            // 48000 Hz) which never matches a 44100 Hz file, causing every transition
            // to be incorrectly non-seamless. The real question is: does the next
            // track need a WASAPI reconfigure? Only when bit-depth changes (bitperfect)
            // or when sample rate / channel count change.
            bool rateMatch = (incoming->sampleRate() == active_->sampleRate() &&
                              incoming->channels()    == active_->channels());
            bool seamless  = rateMatch;
            if (bitperfect && incoming->bitsPerSample() != active_->bitsPerSample())
                seamless = false;
            printf("[%s][Gapless] incoming sr=%d ch=%d bits=%d | active sr=%d ch=%d bits=%d"
                   " | outSr=%d dacCh=%d | seamless=%s\n", logTs(),
                   incoming->sampleRate(), incoming->channels(), incoming->bitsPerSample(),
                   active_->sampleRate(), active_->channels(), active_->bitsPerSample(),
                   outSr, dacCh, seamless ? "true" : "false");
            fflush(stdout);

            // Advance the DECODE/nav cursor now (prepareNextTrack preloads from
            // here). The now-playing DISPLAY flip is deferred to the boundary so
            // the title/seekbar switch when the DAC actually reaches the new track.
            active_ = incoming;
            currentAlbum_ = nextAlbum_;
            currentTrack_ = nextTrack_;
            {
                // The queue's cursor advances with the decode cursor, in the
                // same breath, because prepareNextTrack() derived nextAlbum_/
                // nextTrack_ from queuePos_ + 1 and this is the moment that
                // preloaded track becomes the playing one.
                std::lock_guard<std::mutex> lk(albumsMu_);
                if (queuePos_ >= 0 && queuePos_ + 1 < (int)queue_.size()) queuePos_++;
            }
            // A seamless handoff never reaches onPlay(), so without this line
            // an automatic advance leaves no record of WHERE it went — which
            // is precisely what has to be checkable about it (it must stay
            // inside the playing album's section; see nextAlbumInSection).
            // Indices only: this runs on the gapless thread and reading
            // albums_ for a title would need albumsMu_ for a log line.
            printf("[%s][Gapless] advance -> album=%d track=%d\n",
                   logTs(), currentAlbum_, currentTrack_);
            fflush(stdout);

            if (seamless) {
                // Do NOT flush: the ring still holds the *end* of the finished
                // track. Letting the next decoder append after it is what makes
                // the transition gapless (and stops the ~3 s tail being cut).
                // Queue the boundary at the current written-frame total so the
                // timer flips the now-playing label when the DAC crosses it.
                {
                    std::lock_guard<std::mutex> lk(boundariesMu_);
                    boundaries_.push_back({ playedFrames_.load(std::memory_order_relaxed),
                                            nextAlbum_, nextTrack_ });
                }
                incoming->setDoneCallback([this] {
                    std::lock_guard<std::mutex> lk2(gaplessMu_);
                    gaplessSignal_ = true;
                    gaplessCv_.notify_one();
                });
                incoming->startAsyncInt32(cbI32);
            } else {
                // Different format -> the device must be reconfigured, which tears
                // down the stream. Drain the finished track's buffered tail first
                // so its last seconds play out instead of being discarded.
                while (output_ && output_->pendingPlaybackMs() > 30 &&
                       !stopGapless_.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                // Tell onPlay() this restart is the coordinator advancing, not
                // a listener pressing play — the two look identical from there.
                playFromGapless_.store(true);
                host_->postAppEvent(AppEvent::RequestPlay);
                break;
            }

            prepareNextTrack();
        }
    });
}

// ── Listening stats ──────────────────────────────────────────────────────────
// An event opens the moment a track reaches the transport and closes when it
// leaves. Everything here counts from seekPosMs_, which onTimer() derives from
// frames the DAC has actually rendered (writes minus what is still buffered) —
// so this measures what was heard, never what was decoded.

// Adds the step the play position just took, if it looks like playback rather
// than a seek.
//
// Summing steps is the whole point. Reading the final position instead —
// which is what this used to do — calls a skip to the last minute of a track
// "a minute heard" when none of it sounded, and forgets a stretch replayed
// after a rewind. A jump larger than a few timer ticks is a seek, not
// listening, so it resets the baseline and contributes nothing; a backward
// jump does the same.
void PlayerWindow::accrueListenTime() {
    if (statsPath_.empty()) return;
    const int pos = seekPosMs_ > 0 ? seekPosMs_ : 0;
    if (statsLastPosMs_ >= 0) {
        const int step = pos - statsLastPosMs_;
        // The seek timer runs at 250 ms (see startTimer in onPlay), so a
        // healthy tick advances by roughly that. Anything past ~2 s in one
        // tick is a discontinuity — a seek, or the app having been starved —
        // not audio that reached the DAC.
        if (step > 0 && step <= 2000) statsMsHeard_ += step;
    }
    statsLastPosMs_ = pos;

    // A headphone profile earns its sidebar row here, off the same counter —
    // there is no second timer and no second thread. The listener's stated
    // problem was a list polluted by profiles picked once by mistake, and a
    // minute of audio that actually reached the DAC is the cheapest honest
    // evidence that a pair is really in use.
    //
    // This also fires once per track for a profile already in the list, which
    // is what keeps the ordering a true most-recently-used list rather than a
    // frozen record of first contact. One UPDATE per track.
    if (!eqCreditedThisTrack_ && !eqCurrent_.name.empty() &&
        statsMsHeard_ - eqCreditBaselineMs_ >= kEqCreditMs &&
        !bitperfectMode_.load() && eqManager_.isActive()) {
        db_.creditEqHeadphone(eqCurrent_.name, eqCurrent_.source, eqCurrent_.form,
                              (int64_t)time(nullptr));
        eqCreditedThisTrack_ = true;
        reloadEqHeadphones();      // clears eqCurrentTentative_
        invalidate();
    }
}

void PlayerWindow::beginTrackStats(const Track& t, StartCause cause) {
    // The outgoing track is banked first, and as Replaced: whatever ended it,
    // it was not the listener reaching for the next-track button.
    flushTrackStats(EndCause::Replaced);
    statsPath_       = t.filePath;
    statsKey_        = t.trackKey;
    statsDurationMs_ = t.durationMs;
    statsMsHeard_    = 0;
    statsLastPosMs_  = -1;
    statsEventId_    = db_.beginPlayEvent(statsKey_, statsPath_, t.durationMs,
                                          cause, (int64_t)time(nullptr));
    // Each track buys the active profile one recency refresh, no more; the
    // baseline goes with statsMsHeard_ back to zero.
    eqCreditedThisTrack_ = false;
    eqCreditBaselineMs_  = 0;
}

void PlayerWindow::flushTrackStats(EndCause cause) {
    if (statsPath_.empty()) return;
    accrueListenTime();              // pick up the stretch since the last tick

    // "Completed" = ran essentially to the end. The 90% threshold keeps a
    // track that played to its last second — or whose tagged duration is a
    // hair longer than the audio — out of the skip count. Untagged duration
    // (0) can't be judged either way, so it is never called complete and
    // never called a skip: EndCause is what the queries read for that, and a
    // track nobody skipped away from carries a cause that says so.
    const bool completed = statsDurationMs_ > 0 &&
                           statsMsHeard_ >= (int64_t)(statsDurationMs_ * 0.9);

    db_.endPlayEvent(statsEventId_, statsMsHeard_, completed, cause);

    statsEventId_    = 0;
    statsPath_.clear();
    statsKey_.clear();
    statsDurationMs_ = 0;
    statsMsHeard_    = 0;
    statsLastPosMs_  = -1;
}

// Snapshot of WHICH file was last playing — never of how far into it.
//
// positionMs is written as 0 on purpose. Storing the offset is what made
// resume-on-launch possible, and this player does not resume: a record is
// heard from its beginning, because that is how its development was built.
// The column stays (rewriting the schema to delete it would buy nothing) and
// nothing reads it.
//
// Volume is written as unity for the same kind of reason: this player has no
// volume control, the DAC owns level, so the column is reserved rather than
// guessed — see playback_state in db.cpp's SCHEMA.
void PlayerWindow::savePlaybackStateNow() {
    if (displayAlbum_ < 0 || displayAlbum_ >= (int)albums_.size()) return;
    const auto& tracks = albums_[displayAlbum_].tracks;
    if (displayTrack_ < 0 || displayTrack_ >= (int)tracks.size()) return;
    PlaybackState st;
    st.filePath   = tracks[displayTrack_].filePath;
    st.positionMs = 0;
    st.volume     = 1.0f;
    db_.savePlaybackState(st);
}

void PlayerWindow::onSeek(int posMs) {
    if (output_) output_->flush();   // clears ring -> pendingPlaybackMs ~= 0
    active_->seekMs(posMs);
    int outSr = output_ ? output_->getConfiguredRate() : active_->sampleRate();
    if (outSr <= 0) outSr = active_->sampleRate();
    // Rebase the monotonic written-frame counter onto the displayed track so
    // (played - displayTrackStartFrame_) reads back as posMs.
    playedFrames_.store(displayTrackStartFrame_ + (int64_t)posMs * outSr / 1000);
    { std::lock_guard<std::mutex> lk(boundariesMu_); boundaries_.clear(); }
    markDirty();
}

void PlayerWindow::onTimer() {
    if (!isPlaying_) return;

    if (output_ && output_->hasFaulted()) {
        const std::string why = output_->lastError();
        printf("[Audio][ERROR] %s device fault detected, stopping playback%s%s\n",
               audioBackendLabel().c_str(), why.empty() ? "" : ": ", why.c_str());
        fflush(stdout);
        onStop();
        // Release the faulted backend rather than leaving it half-alive for a
        // later destructor to trip over — for JACK, "faulted" can mean the
        // server died, and that handle must be let go now (see JackSink::live).
        output_->close();
        audioNotice_ = audioBackendLabel() + " output stopped" +
                       (why.empty() ? " \xE2\x80\x94 device fault."
                                    : std::string(" \xE2\x80\x94 ") + why + ".");
        invalidate();
        return;
    }

    int outSr = output_ ? output_->getConfiguredRate() : 0;
    if (outSr <= 0) outSr = active_->sampleRate();
    if (outSr <= 0) return;

    int pendingMs = output_ ? output_->pendingPlaybackMs() : 0;
    // Keep the clock moving through the final drain even after the decoder stops.
    if (!active_->isRunning() && pendingMs <= 0) return;

    // Position the DAC has actually rendered = frames written minus what's still
    // buffered in the output (ring + in-flight queue).
    int64_t written = playedFrames_.load(std::memory_order_relaxed);
    int64_t played  = written - (int64_t)pendingMs * outSr / 1000;
    if (played < displayTrackStartFrame_) played = displayTrackStartFrame_;

    // Flip the now-playing label/seekbar for any boundary the DAC has crossed.
    std::vector<TrackBoundary> commit;
    {
        std::lock_guard<std::mutex> lk(boundariesMu_);
        while (!boundaries_.empty() && played >= boundaries_.front().frame) {
            commit.push_back(boundaries_.front());
            boundaries_.pop_front();
        }
    }
    for (const auto& b : commit) {
        displayTrackStartFrame_ = b.frame;
        applyTrackMetadata(b.album, b.track);
    }

    int64_t posMs = (played - displayTrackStartFrame_) * 1000 / outSr;
    if (posMs < 0) posMs = 0;
    if (seekTotalMs_ > 0 && posMs > seekTotalMs_) posMs = seekTotalMs_;
    seekPosMs_ = (int)posMs;
    accrueListenTime();
    invalidate();
}

void PlayerWindow::onArtClick() {
    if (displayAlbum_ < 0) return;
    artWinShowsArtist_ = false;
    ensureArtWindow();
    artWin_.show(albums_[displayAlbum_].artPath);
}

// Flip the now-playing display (title/artist/art/total + track-row highlight) to
// the given track. Called when the DAC crosses a gapless boundary (from onTimer)
// and on the non-seamless restart. Does NOT touch playedFrames_ (monotonic).
void PlayerWindow::applyTrackMetadata(int album, int track) {
    if (album < 0 || album >= (int)albums_.size() ||
        track < 0 || track >= (int)albums_[album].tracks.size()) return;
    const auto& nt = albums_[album].tracks[track];
    // Same ordering rule as onPlay(): bank the outgoing track while seekPosMs_
    // still describes it. At an ordinary gapless boundary it will be at ~full
    // duration, so it lands as a completed play rather than a skip.
    //
    // Natural, not Replaced: reaching here means the DAC crossed the boundary,
    // which is a track ending on its own. When onNext() routed a listener's
    // skip through this same path it already banked the outgoing track itself,
    // so this flush finds nothing and the verdict it set stands.
    flushTrackStats(EndCause::Natural);
    beginTrackStats(nt, gaplessStartCause_);
    // Consumed; back to the default — and inside a playlist the default is
    // Playlist, not Gapless. THIS IS LOAD-BEARING. The seamless path never
    // reaches onPlay(), so setting the cause there alone would exclude only
    // the FIRST track of a playlist and let every chained advance after it
    // feed the ranking — which is precisely the self-reinforcing loop
    // IS_AFFINITY exists to prevent. See db_stats.cpp.
    gaplessStartCause_ = queueActive() ? StartCause::Playlist
                                       : StartCause::Gapless;
    const int prevDisplayAlbum = displayAlbum_;
    displayAlbum_   = album;
    displayTrack_   = track;
    currentTitle_  = nt.title;
    currentArtist_ = nt.artist;
    seekTotalMs_    = nt.durationMs > 0 ? nt.durationMs : 0;
    seekPosMs_      = 0;
    loadTransportArtTexture(albums_[album].artPath);
    // Follow the music only where the page was already following it. A queue
    // crossing into another record while the listener is reading a DIFFERENT
    // album's page would swap that page out from under them mid-sentence —
    // same rule as onPlay()'s: the transport bar is what says what is playing.
    if (!trackPanelOpen_ || selectedAlbumIdx_ == prevDisplayAlbum) {
        selectedAlbumIdx_ = album;
        loadTrackPanelArtTexture(album);
    }
    invalidate();
}

// ── Background scan ──────────────────────────────────────────────────────────

void PlayerWindow::setupWatchers() {
    Host* host = host_.get();
    streamerDbs_.clear();
    for (auto& root : db_.loadMusicRoots()) {
        watcher_.watchRoot(root, [host](const std::string&) {
            host->postAppEvent(AppEvent::ScanDone, 1);
        });
        StreamerDb sdb;
        sdb.open(root);  // no-op if this root has no sibling .streamer db
        streamerDbs_[root] = std::move(sdb);
    }
}

// Make the AutoEQ database safe to read, blocking if the parse is still going.
//
// Every read of eqProfiles_ goes through here. It is cheap after the first
// call — joining an already-finished thread, then a joinable() test that is
// false forever after — so call sites do not need to reason about whether they
// are the first.
//
// Note this can genuinely block, and that is the correct behaviour rather than
// a flaw: the alternative is answering a question about the listener's saved
// profile before the answer has been read off disk. The worst case is a
// listener who presses play within ~150 ms of launch, who waits exactly as long
// as they used to; everyone else waits for nothing, because the parse finished
// while the window was still being put on screen.
void PlayerWindow::ensureEqProfiles() const {
    if (eqProfilesThread_.joinable()) eqProfilesThread_.join();
}

// Build the art window the first time it is actually asked for.
//
// It used to be built in create(), which meant every launch paid for a second
// Vulkan instance/device/swapchain and a second full set of font faces —
// whether or not the listener ever opened fullscreen art. Its own faces are now
// opened over the main window's bytes (RasterFont::openSharedWith), so what
// remains is the Vulkan setup.
//
// The two windows genuinely cannot share more than the bytes: they have
// separate Renderers and therefore separate VkDevices, and an atlas image
// belongs to one device. Cells, packer and atlas stay per-window.
//
// A failure is remembered rather than retried. On the headless capture tool
// Host::secondaryWindowHandle() is null by design, so create() declines every
// time, and retrying on each show() would be a pointless stall.
void PlayerWindow::ensureArtWindow() {
    if (artWinReady_ || artWinFailed_) return;
    if (artWin_.create(host_.get(), &msdfFont_)) artWinReady_ = true;
    else                                         artWinFailed_ = true;
}

void PlayerWindow::startBackgroundScan() {
    if (scanning_.load()) return;
    if (scanThread_.joinable()) scanThread_.join();

    scanning_.store(true);
    Host* host = host_.get();

    scanThread_ = std::thread([this, host]() {
        auto roots = db_.loadMusicRoots();
        auto cache = db_.loadFileCache();

        std::vector<Album> allAlbums;
        int totalScanned = 0, totalSkipped = 0, totalRemoved = 0;

        for (auto& root : roots) {
            auto result = scanLibraryIncremental(root, cache);
            totalScanned += result.filesScanned;
            totalSkipped += result.filesSkipped;

            auto full = scanLibraryParallel(root);
            for (auto& a : full) {
                Album* ex = nullptr;
                for (auto& ea : allAlbums)
                    if (ea.name == a.name && ea.artist == a.artist) { ex = &ea; break; }
                if (!ex) { allAlbums.push_back(std::move(a)); continue; }
                for (auto& t : a.tracks) {
                    bool dup = false;
                    for (auto& et : ex->tracks)
                        if (et.filePath == t.filePath) { dup = true; break; }
                    if (!dup) ex->tracks.push_back(std::move(t));
                }
                ex->sortTracks();
            }
        }

        purgeStaleFiles(allAlbums, totalRemoved);

        printf("[Scan] Done: %d scanned, %d skipped, %d removed\n",
               totalScanned, totalSkipped, totalRemoved);

        {
            std::lock_guard<std::mutex> lk(scanMu_);
            scanResult_ = std::move(allAlbums);
        }
        scanning_.store(false);
        host_->postAppEvent(AppEvent::ScanDone, 0);
    });
}

void PlayerWindow::onScanDone() {
    if (scanning_.load()) return;

    std::vector<Album> newAlbums;
    {
        std::lock_guard<std::mutex> lk(scanMu_);
        newAlbums = std::move(scanResult_);
    }

    if (newAlbums.empty() && albums_.empty()) return;

    { std::lock_guard<std::mutex> lk(albumsMu_); albums_ = std::move(newAlbums); }

    std::vector<Track> allTracks;
    for (auto& a : albums_)
        for (auto& t : a.tracks) allTracks.push_back(t);
    db_.saveTracks(allTracks);
    db_.saveAlbums(albums_);

    clearGridArtTexCache();
    trackPanelArtTexAlbum_ = -1;
    if (trackPanelArtTex_ != kInvalidTexture) { renderer_->destroy_texture(trackPanelArtTex_); trackPanelArtTex_ = kInvalidTexture; }

    // A rescan can introduce text in a script never seen before (the first
    // Korean album added after launch). refreshGlyphs() is cheap when there is
    // nothing new — every cell already present is skipped — so it just always
    // re-checks here, and re-uploads only if it actually baked something.
    refreshGlyphs();

    rebuildAlbumGroups();  // albums_ changed — regroup before the tile mapping
    rebuildGridIndices();  // albums_ changed — refresh the (possibly filtered) tile mapping
    {
        // Every index into albums_ is dead now, the queue's included. Re-point
        // it by trackKey rather than dropping it: a background rescan finishing
        // mid-playlist is not a reason to stop the music the listener chose.
        // rebuildAlbumGroups() has just refreshed trackKeyIndex_, so this reads
        // the new mapping.
        std::lock_guard<std::mutex> lk(albumsMu_);
        reresolveQueueLocked();
    }
    // A playlist tile holds ALBUM INDICES, which every index above just
    // invalidated — stale ones would paint some other record's cover into a
    // rank quadrant. Rebuilt from the queries, same as the grid above.
    // (loadPlaylistCovers takes albumsMu_ itself, so it must run outside the
    // scope that just held it — the lock is not reentrant.)
    if (navSection_ == NavSection::Playlists) {
        loadPlaylistCovers();
        if (plKind_ != PlaylistKind::None) loadPlaylist(plKind_);
    }
    recalcLayout();
    invalidate();

    printf("[Library] Updated: %d albums, %d tracks\n",
           (int)albums_.size(), (int)allTracks.size());
}

// ── Message loop ─────────────────────────────────────────────────────────────

// Message-loop/WM_*-dispatch (wndProc/handleMsg) now lives in
// os/windows_host.cc / os/linux_host.cc — see host.hh. What follows are the
// portable pieces that used to be inline in handleMsg's switch: hotkey
// dispatch, search-box text entry, and playback-key handling, all called
// from both hosts' input-translation code, plus the once-WM_DESTROY teardown
// sequence (now shutdown(), called by both hosts before the window/renderer
// actually go away).

void PlayerWindow::onHotkey(int hotkeyId) {
    if (hotkeyId == kHotkeyToggleMode) toggleUiMode();
    else                               snapToEdge(hotkeyId);
}

void PlayerWindow::onCharPortable(uint32_t codepoint) {
    if (activePanel_ != SettingsPanel::None) { onPanelChar(codepoint); return; }
    if (!searchFocused_) return;
    // Tab and Enter accept the highlighted suggestion (the first row when
    // nothing is highlighted, which is the one the ranking put there). Both
    // keys, because the box behaves like a completion field and there is no
    // reason to make the listener remember which of the two this one wants.
    if (codepoint == 0x09 || codepoint == 0x0D) {
        acceptSuggestion(searchSuggestSel_ >= 0 ? searchSuggestSel_ : 0);
        return;
    }
    if (codepoint == 0x08) {  // backspace: pop one UTF-8 codepoint
        // On an empty box, backspace takes back the last CHIP whole. A chip is
        // one indivisible choice — deleting it letter by letter would mean
        // passing through half-values that were never valid queries.
        if (searchQuery_.empty()) {
            if (!searchChips_.empty()) removeChip((int)searchChips_.size() - 1);
            return;
        }
        while (!searchQuery_.empty() && (searchQuery_.back() & 0xC0) == 0x80)
            searchQuery_.pop_back();
        if (!searchQuery_.empty()) searchQuery_.pop_back();
    } else if (codepoint >= 0x20 && codepoint != 0x7F) {
        // Encode the codepoint as UTF-8 directly — portable, no wide-char
        // detour (Windows' WM_CHAR delivers UTF-16 code units, which for the
        // BMP codepoints a search box actually sees are numerically the same
        // value this function receives).
        appendUtf8(searchQuery_, codepoint);
    } else {
        return;  // control chars: consumed, no query change
    }
    refreshSuggestions();
    rebuildGridIndices();
    gridScrollY_ = 0;
    recalcLayout();
    invalidate();
}

void PlayerWindow::onPanelChar(uint32_t codepoint) {
    if (activePanel_ != SettingsPanel::EqSettings || !eqSearchFocused_) return;
    if (codepoint == 0x08) {
        while (!eqSearch_.empty() && (eqSearch_.back() & 0xC0) == 0x80)
            eqSearch_.pop_back();
        if (!eqSearch_.empty()) eqSearch_.pop_back();
    } else if (codepoint >= 0x20 && codepoint != 0x7F) {
        appendUtf8(eqSearch_, codepoint);
    } else {
        return;
    }
    eqRefilter();
    eqScrollY_ = 0;
    invalidate();
}

bool PlayerWindow::onPanelKeyDown(int keyCode) {
    if (activePanel_ == SettingsPanel::None) return false;
    if (keyCode == key::Escape) {
        closeActivePanel();
        return true;
    }
    // Play/stop is NOT part of what a settings panel owns. Everything else is
    // swallowed (no search-box interaction bleeding through to the view behind
    // it — the native dialogs' EnableWindow(false) behaviour), but stopping
    // the music is a transport action, the transport bar is right there and
    // still drawn, and a dialog that takes the key away leaves a listener with
    // no way to silence the room without first closing it. The one exception
    // is a panel with a text field focused, where the space is a character.
    if (keyCode == key::Space && !eqSearchFocused_) return false;
    return true;
}

// ONE step out. Escape and the mouse's back button both come through here, so
// the two cannot describe different shapes of "back". Order is outermost
// first: the thing most recently laid over the view is the thing that leaves.
bool PlayerWindow::goBack() {
    ViewState before = captureViewState();
    bool moved = false;
    // ViewState describes the five sections and the two views inside them, not
    // which settings panel was open — so closing a panel is a step back that
    // has no forward. Recording one would send the forward button to the
    // Settings PAGE the panel was borrowing, which is not where it came from.
    bool recordForward = true;

    if (activePanel_ != SettingsPanel::None) {
        closeActivePanel();
        moved = true;
        recordForward = false;
    } else if (searchFocused_ || !searchQuery_.empty()) {
        // Leave the box and clear the filter — the grid underneath is a
        // different grid while a query is live, so this IS a step back.
        searchFocused_ = false;
        searchQuery_.clear();
        rebuildGridIndices();
        gridScrollY_ = 0;
        recalcLayout();
        invalidate();
        moved = true;
    } else if (settingsOpen_) {
        // Settings replaces the content area, so leaving it hands back
        // whichever section was underneath — exactly what closeActivePanel()
        // does for a panel the sidebar opened.
        settingsOpen_ = false;
        recalcLayout();
        invalidate();
        moved = true;
    } else if (navSection_ == NavSection::Playlists && plKind_ != PlaylistKind::None) {
        // Two levels deep, same as the album section: leaving the LIST is not
        // leaving the section.
        plKind_ = PlaylistKind::None;
        plEntries_.clear();
        plDurationMs_.clear();
        plListRows_.clear();
        plScrollY_  = 0;
        plHoverRow_ = -1;
        recalcLayout();
        invalidate();
        moved = true;
    } else if (trackPanelOpen_) {
        trackPanelOpen_ = false;
        recalcLayout();
        invalidate();
        moved = true;
    }

    if (moved && recordForward) {
        navForward_      = before;
        navForwardValid_ = true;
    } else if (moved) {
        navForwardValid_ = false;
    }
    return moved;
}

PlayerWindow::ViewState PlayerWindow::captureViewState() const {
    ViewState s;
    s.section        = navSection_;
    s.filter         = albumTypeFilter_;
    s.settingsOpen   = settingsOpen_;
    s.trackPanelOpen = trackPanelOpen_;
    s.selectedAlbum  = selectedAlbumIdx_;
    s.plKind         = plKind_;
    return s;
}

void PlayerWindow::applyViewState(const ViewState& s) {
    navSection_   = s.section;
    settingsOpen_ = s.settingsOpen;

    if (s.section == NavSection::Playlists) {
        trackPanelOpen_ = false;
        if (s.plKind != plKind_) {
            if (s.plKind == PlaylistKind::None) {
                plKind_ = PlaylistKind::None;
                plEntries_.clear();
                plDurationMs_.clear();
                plListRows_.clear();
                plScrollY_ = 0;
            } else {
                loadPlaylist(s.plKind);   // the query IS the list; re-run it
            }
        }
        if (plKind_ == PlaylistKind::None) loadPlaylistCovers();
        recalcLayout();
        invalidate();
        return;
    }

    if (s.filter != albumTypeFilter_) {
        albumTypeFilter_ = s.filter;
        rebuildGridIndices();
        gridScrollY_ = 0;
    }
    // The album may have gone (a rescan between the back and the forward) —
    // then there is nothing to re-enter, and the grid is the honest landing.
    if (s.trackPanelOpen && s.selectedAlbum >= 0 &&
        s.selectedAlbum < (int)albums_.size()) {
        openAlbumView(s.selectedAlbum);
    } else {
        trackPanelOpen_ = false;
        recalcLayout();
        invalidate();
    }
}

void PlayerWindow::onNavBack() {
    if (uiMode_ == UiMode::Essential) return;   // nothing to go back FROM
    goBack();
}

void PlayerWindow::onNavForward() {
    if (uiMode_ == UiMode::Essential) return;
    if (!navForwardValid_) return;
    ViewState s = navForward_;
    navForwardValid_ = false;   // one step forward, then the stack is spent
    applyViewState(s);
}

void PlayerWindow::onKeyDownPortable(int keyCode) {
    if (onPanelKeyDown(keyCode)) return;
    switch (keyCode) {
    case key::Space:
        if (searchFocused_) return;  // typing a space, not play/stop
        if (isPlaying_) onStop(); else if (currentAlbum_ >= 0) onPlay();
        return;
    case key::Escape:
        goBack();
        return;
    }
}

void PlayerWindow::shutdown() {
    // Banked before onStop() so the log says the app closed rather than that
    // the listener pressed stop — closing mid-track is not a verdict on the
    // music, and the queries treat the two differently. Doing it here rather
    // than leaving the row open for Db::open()'s crash repair also keeps the
    // real ms_heard, which that repair has no way to recover.
    flushTrackStats(EndCause::AppExit);
    // onStop() saves the resume point and would bank the stats itself (now a
    // no-op) — it must run while albums_/seekPosMs_ are still valid, which is
    // why it stays first.
    onStop();
    watcher_.unwatchAll();
    if (scanThread_.joinable()) scanThread_.join();
    // The EQ parse writes into eqProfiles_, which is a member: it must not
    // still be running when this object is destroyed, whether or not anything
    // ever read from it.
    if (eqProfilesThread_.joinable()) eqProfilesThread_.join();
    // Join the art-decode worker before tearing down textures/renderer — it
    // never touches the Renderer, but a decode completing after this point
    // would notify a dying window.
    stopArtDecodeThread();
    // Same rule as applyAudioSettingsPanel(): close the backend by name rather
    // than leaving it to ~PlayerWindow, so teardown order is stated here.
    if (output_) {
        output_->stop();
        output_->close();
    }
    output_.reset();
    usbDriver_.close();

    // Texture caches must be torn down before the Renderer that owns their
    // VkImages/descriptor sets.
    clearGridArtTexCache();
    if (trackPanelArtTex_ != kInvalidTexture) renderer_->destroy_texture(trackPanelArtTex_);
    if (transportArtTex_ != kInvalidTexture) renderer_->destroy_texture(transportArtTex_);
    if (artistImgTex_ != kInvalidTexture) renderer_->destroy_texture(artistImgTex_);

    // Destroy the Vulkan swapchain/renderer while the host's native window
    // still exists — host_ (and the SurfaceProvider it owns) is torn down
    // after this returns.
    // Before the device goes: the baker owns pipelines and ~150 MB of scratch.
    glyphBaker_.destroy();
    renderer_.reset();
}

#ifdef MATRIX_UI_CAPTURE
// ── Headless UI capture (tools/ui_capture) ───────────────────────────────────
//
// Compiled only into the capture tool. Everything here drives the app through
// its ORDINARY entry points — the same onLButtonDown() the Wayland backend
// calls, the same drawFrame() run() calls — so a capture is a photograph of the
// app, not a re-staging of it. The one thing being a member buys is access to
// the rects recalcLayout() already computed; a tool that hardcoded sidebar
// coordinates would silently photograph the wrong pixels the first time the
// sidebar moved.

namespace {
// Center of a rect, which is where a user aims.
inline void centerOf(const LayoutRect& r, int& x, int& y) {
    x = (r.left + r.right) / 2;
    y = (r.top + r.bottom) / 2;
}
}

bool PlayerWindow::captureFrame(std::vector<uint8_t>& rgba, uint32_t& w, uint32_t& h) {
    // Album art is decoded on a worker thread and delivered by
    // host_->postAppEvent(AppEvent::ArtDecoded) — which a headless Host has
    // nowhere to deliver to. So settle the frame here instead: drawing is what
    // QUEUES the decodes (getGridArtTexture() enqueues for the tiles actually
    // on screen), onArtDecoded() takes delivery and uploads the textures, and
    // the next draw finally has them. Without this loop every screenshot shows
    // the placeholder rectangles — which is to say, none of the artwork the
    // grid is built around.
    //
    // Glyphs settle here too, and explicitly. A cell the layout asks for and
    // does not have is baked by the NEXT drawFrame() (see the top of it), so
    // the loop has to keep going until the cache has stopped asking — not
    // merely until the artwork has landed. This used to rest on the single
    // trailing drawFrame() below happening to be enough, which is true only
    // when a scenario needs exactly one bake and no more.
    for (int i = 0; i < 250; i++) {
        drawFrame();
        onArtDecoded();
        if (artDecodePending_.empty() && !msdfFont_.hasMisses()) break;
        if (artDecodePending_.empty()) continue;   // glyphs only: no need to sleep
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    drawFrame();
    return renderer_->readbackLastFrame(rgba, w, h);
}

bool PlayerWindow::captureGoTo(const std::string& state) {
    // Every state starts from the grid, so a capture list is order-independent
    // (a panel left open by the previous scenario would otherwise leak into
    // the next screenshot).
    auto click = [&](const LayoutRect& r) {
        int x, y; centerOf(r, x, y);
        onMouseMove(x, y);       // hover first — the real pointer always does
        onLButtonDown(x, y);
    };
    // A playlist tile lives on the album grid's geometry and is only a rect
    // while it draws, so there is nothing stored to click — the same math the
    // draw and the hit-test share (see drawPlaylistGrid/playlistTileHitTest).
    auto clickPlaylistTile = [&](int i) {
        int tileStepX = gridStepX_, tileStepY = gridTileSize_ + gridRowGap_;
        int col = i % gridCols_, row = i / gridCols_;
        int x = rcGrid_.left + gridPadXpx_ + col * tileStepX
                + (tileStepX - gridArtSize_) / 2 + gridArtSize_ / 2;
        int y = rcGrid_.top + gridPadYpx_ + row * tileStepY - gridScrollY_
                + gridArtSize_ / 2;
        onMouseMove(x, y);
        onLButtonDown(x, y);
    };
    auto reset = [&] {
        activePanel_    = SettingsPanel::None;
        settingsOpen_   = false;
        trackPanelOpen_ = false;
        navSection_     = NavSection::Albums;
        plKind_         = PlaylistKind::None;
        searchFocused_  = false;
        searchQuery_.clear();
        searchChips_.clear();
        searchSuggest_.clear();
        searchSuggestSel_ = -1;
        rebuildGridIndices();
        recalcLayout();
    };

    reset();

    if (state == "10-grid-albums")  { click(rcNavAlbum_);  return true; }
    if (state == "11-grid-eps")     { click(rcNavEp_);     return true; }
    if (state == "12-grid-singles") { click(rcNavSingle_); return true; }
    if (state == "13-grid-compilations") { click(rcNavCompilation_); return true; }
    if (state == "14-grid-live")    { click(rcNavLive_);   return true; }
    if (state == "15-grid-remixes") { click(rcNavRemix_);  return true; }

    if (state == "16-grid-hover") {
        // The grid tile hover state — one of the few pieces of the visual
        // language that never appears in a static shot otherwise.
        click(rcNavAlbum_);
        if (gridIndices_.empty()) return false;
        drawFrame();
        onMouseMove(rcGrid_.left + (rcGrid_.right - rcGrid_.left) / 4,
                    rcGrid_.top + gridTileSize_ / 2 + gridPadYpx_);
        return true;
    }

    // The multi-script tail of the grid. Latin sorts first, so on a real
    // library every Han/Hangul/Cyrillic title lands at the BOTTOM — the first
    // screenful is all Latin, and a shot of it says nothing about the scripts
    // most likely to be broken. That is not hypothetical: the atlas silently
    // stopped baking Japanese and Korean (both fallback faces rejected for
    // exceeding 4096px of sheet) and every capture still looked correct.
    // gridTotalHeight_ is already set by reset()'s recalcLayout(), so one
    // oversized wheel notch clamps straight to the end.
    if (state == "17-grid-multiscript") {
        click(rcNavAlbum_);
        if (gridIndices_.empty()) return false;
        int cx, cy; centerOf(rcGrid_, cx, cy);
        onMouseWheel(cx, cy, -1000000);
        return true;
    }

    if (state == "20-album-view") {
        click(rcNavAlbum_);
        if (gridIndices_.empty()) return false;
        onAlbumSelected(gridIndices_[0]);
        return true;
    }

    // The same view on a non-Latin record: its TRACK TITLES are the smallest
    // type role in the app, and a glyph the atlas never baked draws as nothing
    // at all — an empty row, not a wrong-looking one. Chosen by content rather
    // than by index so it keeps working on any library, and Hangul is
    // preferred over Han because the fallback faces are baked in the order
    // Chinese -> Japanese -> Korean (see ensureFallbackGlyphs): whatever runs
    // out of atlas does so from the BACK of that list, so Korean is the first
    // script to disappear and the last to come back.
    if (state == "21-album-view-multiscript") {
        click(rcNavAlbum_);
        auto scan = [](const std::string& s, uint32_t lo, uint32_t hi) {
            for (size_t i = 0; i < s.size(); ) {
                uint32_t cp = utf8::nextCodepoint(s, i);
                if (cp >= lo && cp <= hi) return true;
            }
            return false;
        };
        // displayName, not name: name is the raw folder key, which in a
        // downloader-managed library is an opaque hash and never carries the
        // script the record is actually titled in.
        auto pick = [&](uint32_t lo, uint32_t hi) {
            for (int idx : gridIndices_) {
                const Album& a = albums_[(size_t)idx];
                if (scan(a.displayName, lo, hi) || scan(a.artist, lo, hi)) return idx;
                for (const Track& t : a.tracks)
                    if (scan(t.title, lo, hi)) return idx;
            }
            return -1;
        };
        int idx = pick(0xAC00, 0xD7AF);                     // Hangul syllables
        if (idx < 0) idx = pick(0x3040, 0x30FF);            // Kana
        if (idx < 0) idx = pick(0x1100, 0x10FFFF);          // anything past Latin/Greek/Cyrillic
        if (idx < 0) return false;
        onAlbumSelected(idx);
        return true;
    }

    if (state == "30-settings") { click(rcNavSettings_); return true; }

    if (state == "31-manage-folders") {
        click(rcNavSettings_); drawFrame();
        click(rcSettingsManage_);
        return true;
    }
    if (state == "32-audio-settings") {
        click(rcNavSettings_); drawFrame();
        click(rcSettingsAudio_);
        return true;
    }
    if (state == "33-eq-settings") {
        click(rcNavSettings_); drawFrame();
        click(rcSettingsEq_);
        return true;
    }
    if (state == "34-eq-all-profiles") {
        // The panel opens on "My Drivers" (33 above), so this is the other
        // tab: the full 8600-profile catalogue, which is the state that shows
        // what a long list looks like in this design.
        click(rcNavSettings_); drawFrame();
        click(rcSettingsEq_);
        // The tab rects are written while the panel draws (the same
        // draw-then-hit-test pattern the album view uses), so the panel has to
        // have been on screen once before its tabs can be clicked.
        drawFrame();
        click(eqTabAll_);
        return true;
    }
    if (state == "35-folder-picker") {
        click(rcNavSettings_); drawFrame();
        click(rcSettingsAddFolder_);
        return true;
    }

    // The tile grid, then each list. Reaching a list means clicking a tile, so
    // the grid has to have been drawn once first — the same draw-then-click
    // order the EQ tabs need.
    if (state == "36-playlists") { click(rcNavPlaylists_); return true; }
    if (state == "37-playlists-heavy-rotation") {
        click(rcNavPlaylists_); drawFrame();
        clickPlaylistTile(0);
        return true;
    }
    if (state == "38-playlists-forgotten-favourites") {
        click(rcNavPlaylists_); drawFrame();
        clickPlaylistTile(1);
        return true;
    }
    if (state == "39-playlists-never-heard") {
        click(rcNavPlaylists_); drawFrame();
        clickPlaylistTile(2);
        return true;
    }
    if (state == "3a-playlists-row-hover") {
        // The hover pill is the one thing a static shot cannot show, and it is
        // exactly where a wrong fitWidth collapses the highlight to a sliver.
        click(rcNavPlaylists_); drawFrame();
        clickPlaylistTile(2);            // Never Heard — the list with rows on a fresh log
        drawFrame();
        if (plListRows_.empty()) return false;
        const auto& r = plListRows_[0];
        onMouseMove((int)(r.rect.x + r.rect.w * 0.4f), (int)(r.rect.y + r.rect.h * 0.5f));
        return true;
    }

    if (state == "40-search") {
        click(rcSearch_);
        for (char c : std::string("love")) onCharPortable((uint32_t)c);
        return true;
    }
    // Both guided-search states are reached through the REAL input path —
    // click, type, press Tab — rather than by assigning to searchChips_. A
    // capture that set the state directly could not catch the suggestion
    // ranking or the accept path breaking.
    if (state == "41-search-suggest") {
        click(rcSearch_);
        for (char c : std::string("199")) onCharPortable((uint32_t)c);
        return true;
    }
    if (state == "42-search-chips") {
        click(rcSearch_);
        for (char c : std::string("199")) onCharPortable((uint32_t)c);
        onCharPortable(0x09);                       // Tab: accept the top row
        for (char c : std::string("24")) onCharPortable((uint32_t)c);
        onCharPortable(0x09);
        return true;
    }

    return false;
}
#endif  // MATRIX_UI_CAPTURE

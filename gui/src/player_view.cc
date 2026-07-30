#include "player_view.hh"
#include "log_util.h"
#include "art_texture.hh"
#include "img_decode.hh"
#include "text_util.hh"
#include "utf8.hh"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <chrono>
#include <vector>
#include <algorithm>
#include <stdexcept>
#include <filesystem>
#include <fstream>

#include <soxr.h>

static int pickOutputRate(int inRate, const std::vector<int>& supported) {
    for (int r : supported)
        if (r % inRate == 0) return r;
    for (int r : supported)
        if (r > inRate) return r;
    return supported.empty() ? 48000 : supported.back();
}

// Fast LCG for TPDF dither noise generation.
static uint32_t s_lcgState = 0x9E3779B9u;
static inline uint32_t lcgNext() {
    s_lcgState = s_lcgState * 1664525u + 1013904223u;
    return s_lcgState;
}

// TPDF dither + single quantize to the device's max bit depth.
// Output is always int32 wire format (lower bits zeroed for 16/24-bit targets).
static void ditherAndQuantize(const double* in, int32_t* out, int n, int bits) {
    double scale = (bits == 16) ? 32767.0   * (double)(1 << 16)
                 : (bits == 24) ? 8388607.0 * (double)(1 << 8)
                 :                2147483647.0;
    double ditherAmp = (bits < 32) ? (1.0 / scale) : 0.0;
    for (int i = 0; i < n; i++) {
        // TPDF: triangular distribution from two uniform random values
        double r = ditherAmp * ((double)(int32_t)(lcgNext() >> 1) -
                                (double)(int32_t)(lcgNext() >> 1)) * (1.0 / 1073741824.0);
        double s = in[i] + r;
        if (s >  1.0) s =  1.0;
        if (s < -1.0) s = -1.0;
        long long q = llround(s * scale);
        if (q >  2147483647LL) q =  2147483647LL;
        if (q < -2147483648LL) q = -2147483648LL;
        out[i] = (int32_t)q;
    }
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

// Album/track names in this library carry a trailing parenthesized
// "modifier" — "(Are You Coming)" deluxe-style reissue tags, "(Deluxe)",
// "(PA)", "(from the Netflix Series ...)". The base name is what the user
// actually scans for; the modifier is secondary. Split them so callers can
// give the base name layout priority (own line / never ellipsized ahead of
// the modifier). Returns true and fills base/mod (mod without its parens)
// only for a well-formed trailing " (...)" that isn't the whole string;
// otherwise base = s, mod empty.
// Strips ONE trailing bracketed group off `s`, honoring nesting: for
// "Tears (feat. Sleepnet & Joker)" or "Mixed Signals [VIP (Extended)]" it
// returns the group's opener index and its inner text. Both () and [] count
// — Skrillex-style feat. credits use square brackets. Returns npos if the
// string doesn't end in a well-formed group preceded by a space.
static size_t trailingGroup(const std::string& s, std::string& inner) {
    if (s.size() < 4) return std::string::npos;
    char close = s.back();
    if (close != ')' && close != ']') return std::string::npos;
    char open = (close == ')') ? '(' : '[';
    int depth = 0;
    for (size_t i = s.size(); i-- > 0; ) {
        if (s[i] == close) depth++;
        else if (s[i] == open) {
            depth--;
            if (depth == 0) {
                // Must be a *suffix* group, not the whole string
                // ("(What's The Story) Morning Glory?" keeps its parens).
                if (i == 0) return std::string::npos;
                inner = s.substr(i + 1, s.size() - i - 2);
                if (s[i - 1] != ' ') {
                    // No space before the bracket: only accept groups that
                    // are unmistakably credits ("Name(feat. X)" happens in
                    // sloppy tags), never bare ones — "R(A)W" stays whole.
                    std::string low = inner.substr(0, 5);
                    for (auto& ch : low) ch = (char)tolower((unsigned char)ch);
                    bool credit = low.rfind("feat", 0) == 0 || low.rfind("ft.", 0) == 0 ||
                                  low.rfind("ft ", 0) == 0  || low.rfind("with ", 0) == 0;
                    if (!credit) { inner.clear(); return std::string::npos; }
                }
                return i;
            }
        }
    }
    return std::string::npos;
}

// Album/track names in this library carry trailing bracketed "modifiers" —
// "(Are You Coming)" deluxe-style reissue tags, "(Deluxe)", "(PA)",
// "[feat. Sleepnet & Joker]", "(from the Netflix Series ...)". The base name
// is what the user scans for; modifiers are secondary. Peels up to three
// stacked groups ("Tears (with X) [feat. Y]" → mod "with X · feat. Y") so
// callers can give the base name layout priority. Returns true only when
// something was split; otherwise base = s, mod empty.
static bool splitNameModifier(const std::string& s, std::string& base, std::string& mod) {
    base = s;
    mod.clear();
    // Metadata strings often carry invisible trailing whitespace
    // ("... (feat. X) " / a stray \r) — without this trim the last char
    // isn't ')' and the group detection misses entirely, so the whole
    // title renders as the base name (the "Tears (feat. Sleepnet &
    // Joker) rendered all-white" bug).
    while (!base.empty() && (base.back() == ' ' || base.back() == '\t' ||
                             base.back() == '\r' || base.back() == '\n'))
        base.pop_back();
    for (int pass = 0; pass < 3; pass++) {
        std::string inner;
        size_t at = trailingGroup(base, inner);
        if (at == std::string::npos || inner.empty()) break;
        std::string rest = base.substr(0, at);
        while (!rest.empty() && rest.back() == ' ') rest.pop_back();
        if (rest.empty()) break;
        base = rest;
        // '·' (U+00B7) is in the baked Latin-1 range.
        mod = mod.empty() ? inner : inner + " \xC2\xB7 " + mod;
    }
    // No group found: keep the (whitespace-trimmed) name as the base.
    return !mod.empty();
}

// ── Window creation ──────────────────────────────────────────────────────────
// MAIN_CLASS, kFixedWindowStyle/kFixedWindowExStyle, and the window-creation/
// message-loop/monitor-rect logic that used to live in this section now live
// in os/windows_host.cc (WindowsHost) / os/linux_host.cc (LinuxHost) — see
// host.hh. Hotkey IDs are in hotkey_ids.hh (shared by both hosts).

bool PlayerWindow::create() {
    host_ = make_host();

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
    db_.open(exeDir + "matrix_player.db");

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
        rebuildGridIndices();
    }

    // UI font (fonts/ is copied next to the exe by the CMake build step).
    {
        auto toUtf8Path = [&](const char* rel) -> std::string {
            return exeDir + rel;
        };

        std::string fontPath = toUtf8Path("fonts/lm/lmroman10-regular.otf");
        uiFont_.load(fontPath.c_str());
        fontsDir_ = toUtf8Path("fonts/");

        // Generate MSDF atlas from the same OTF (cached to disk for fast reload).
        std::string cachePath = toUtf8Path("fonts/lmroman10-regular.msdf.cache");
        msdfCachePath_ = cachePath;

        FileByteReader loader;
        if (msdfFont_.generate(loader, fontPath.c_str(), cachePath.c_str())) {
            // Bold (headers/titles), Italic (artist/secondary text), and Mono
            // (repurposing the unused Math slot — this app never renders math)
            // for numeric readouts so digits don't jitter as they change.
            // Skipped if a warm cache already baked them (hasStyle()), so a
            // fresh addStyle() rebake only happens once per cache generation.
            bool addedStyle = false;
            if (!msdfFont_.hasStyle(FontStyle::Bold))
                addedStyle |= msdfFont_.addStyle(loader, toUtf8Path("fonts/lm/lmroman10-bold.otf").c_str(), FontStyle::Bold);
            if (!msdfFont_.hasStyle(FontStyle::Italic))
                addedStyle |= msdfFont_.addStyle(loader, toUtf8Path("fonts/lm/lmroman10-italic.otf").c_str(), FontStyle::Italic);
            if (!msdfFont_.hasStyle(FontStyle::Math))
                addedStyle |= msdfFont_.addStyle(loader, toUtf8Path("fonts/lm/lmmono10-regular.otf").c_str(), FontStyle::Math);

            // Cyrillic/Greek (unconditional) + whatever CJK/Hangul/Kana the
            // now-loaded library actually contains — see bakeFallbackGlyphs().
            bool addedFallback = bakeFallbackGlyphs();

            if (addedStyle || addedFallback) msdfFont_.saveCache(cachePath.c_str());

            renderer_->initMsdf(msdfFont_);
            // The CPU atlas copy (~8 MB) just went to the GPU and is saved
            // on disk — drop it from RAM. Re-bakes re-hydrate it from the
            // cache first (see onScanDone()).
            msdfFont_.releaseAtlasPixels();
        }
    }

    artWin_.create(host_.get());
    recalcLayout();

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

    // Load EQ profiles
    eqProfiles_.load(exeDir + "eq_profiles.json");

    setupWatchers();
    startBackgroundScan();

    // Load audio mode
    bitperfectMode_.store(db_.loadSetting("audio_mode") == "bitperfect");

    // Load audio backend
    {
        std::string backend = db_.loadSetting("audio_backend");
        audioBackend_ = AudioBackend::Usb;
#ifdef _WIN32
        if (backend == "wasapi") audioBackend_ = AudioBackend::Wasapi;
#else
#ifdef MATRIX_HAVE_ALSA
        if (backend == "alsa") audioBackend_ = AudioBackend::Alsa;
#endif
#ifdef MATRIX_HAVE_JACK
        if (backend == "jack") audioBackend_ = AudioBackend::Jack;
#endif
#endif
    }
#ifdef _WIN32
    wasapiMode_ = (db_.loadSetting("wasapi_mode") == "exclusive")
                  ? WasapiMode::Exclusive : WasapiMode::Shared;
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
            char msgBuf[512];
            snprintf(msgBuf, sizeof(msgBuf),
                "USB DAC not found (VID=%04X PID=%04X).\n\n"
                "Steps to fix:\n"
                "1. Open Zadig\n"
                "2. Select your USB DAC interface MI_00\n"
                "3. Install libusbK driver\n"
                "4. Restart this app\n\n"
                "Use Audio Settings to select a different device\n"
                "or switch to a secondary backend.", vid, pid);
            host_->showErrorMessage("USB DAC not found", msgBuf);
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

bool PlayerWindow::bakeFallbackGlyphs() {
    if (!msdfFont_.valid()) return false;
    // Baking APPENDS rows to the CPU atlas pixels, which are released from
    // RAM after each GPU upload (releaseAtlasPixels()). Re-hydrate them from
    // the disk cache first — appending to an empty buffer would zero out
    // every existing glyph and then saveCache() would wreck the good cache.
    if (!msdfFont_.atlasResident() &&
        !msdfFont_.ensureAtlasLoaded(msdfCachePath_.empty() ? nullptr
                                                            : msdfCachePath_.c_str()))
        return false;
    FileByteReader loader;
    bool anyNew = false;

    // Greek (U+0370-U+03FF) + Cyrillic (U+0400-U+04FF): cheap and bounded
    // (~700 codepoints), baked unconditionally — covers most European-
    // language metadata outright with no library scan needed. Baked from
    // New Computer Modern, the same Computer Modern lineage as the Latin
    // Modern base font, so Cyrillic/Greek text renders in a visually
    // consistent serif instead of jumping to a system UI font.
    {
        std::vector<uint32_t> cps;
        for (uint32_t cp = 0x0370; cp <= 0x03FF; cp++) cps.push_back(cp);
        for (uint32_t cp = 0x0400; cp <= 0x04FF; cp++) cps.push_back(cp);
        std::string newcmPath = fontsDir_ + "newcomputermodern/NewCM10-Regular.otf";
        if (msdfFont_.bakeCodepoints(loader, newcmPath.c_str(), cps) > 0)
            anyNew = true;
    }

    // Everything else (CJK, Hangul, Kana, ...): baking the full Han block
    // eagerly (~20,000+ glyphs) isn't proportionate to what a music
    // library's metadata will ever actually contain, so only bake
    // codepoints that genuinely appear in the scanned library.
    std::vector<uint32_t> exotic;
    {
        auto scan = [&](const std::string& s) {
            for (size_t i = 0; i < s.size(); ) {
                uint32_t cp = utf8::nextCodepoint(s, i);
                if (cp >= 0x500 && !msdfFont_.hasCodepoint(cp))
                    exotic.push_back(cp);
            }
        };
        for (auto& a : albums_) {
            scan(a.name);
            scan(a.artist);
            for (auto& t : a.tracks) {
                scan(t.title);
                scan(t.artist);
                scan(t.albumArtist);
            }
        }
    }
    if (!exotic.empty()) {
        // New Computer Modern's coverage extends well past Greek/Cyrillic
        // (Latin Extended-A/B, general punctuation incl. „ " smart quotes,
        // etc.), so anything ≥U+0500 it happens to have (German/French/
        // Polish/... metadata using those blocks) stays in the same serif
        // look instead of jumping to a mismatched font. Only what it
        // genuinely lacks (CJK/Hangul/Kana) falls through to the bundled
        // per-script serif faces below — bakeCodepoints() skips whatever's
        // already covered, so this whole chain is purely additive.
        std::string newcmPath = fontsDir_ + "newcomputermodern/NewCM10-Regular.otf";
        if (msdfFont_.bakeCodepoints(loader, newcmPath.c_str(), exotic) > 0)
            anyNew = true;

        // Dedicated CJK/Hangul serif faces bundled alongside Latin Modern —
        // Song/Mincho/Batang are all serif designs, the closest visual match
        // to Latin Modern's serif Latin text (vs. a sans-serif system font).
        const std::string kCjkFallbacks[] = {
            fontsDir_ + "fandol/FandolSong-Regular.otf",         // Simplified Chinese
            fontsDir_ + "haranoaji/HaranoAjiMincho-Regular.otf", // Japanese
            fontsDir_ + "unfonts-core/UnBatang.ttf",             // Korean
        };
        for (const auto& path : kCjkFallbacks) {
            if (msdfFont_.bakeCodepoints(loader, path.c_str(), exotic) > 0)
                anyNew = true;
        }
    }

    return anyNew;
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
    }
}

// UI icons drawn as native vector shapes (triangle/rect/segment) straight
// into the curve pass — replaces the old SVG-rasterize-to-texture path
// (nanosvg + 7 texture uploads at startup). Same geometry as the old SVG
// viewBox coordinates (36-unit grid, 24 for Close), so they look identical
// but stay crisp at any button size and cost zero VRAM/upload. They draw
// after the hover rects in frame order, so they composite on top exactly
// like text does.
enum class UiIcon { Play, Stop, Prev, Next, Settings };

static void drawUiIcon(Canvas& c, const LayoutRect& rc, UiIcon icon, Color col) {
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
    }
}

// Same 36-unit-grid vector construction as drawUiIcon, for the bitperfect
// warning banner's triangle-with-exclamation-mark glyph.
static void drawWarningIcon(Canvas& c, const LayoutRect& rc, Color col) {
    Rect r = toRect(rc);
    float s = std::min(r.w, r.h);
    float ox = r.x + (r.w - s) * 0.5f, oy = r.y + (r.h - s) * 0.5f;
    auto X = [&](float u) { return ox + u / 36.0f * s; };
    auto Y = [&](float v) { return oy + v / 36.0f * s; };
    c.triangle(X(18), Y(4), X(4), Y(32), X(32), Y(32), col);
    c.rect(X(16), Y(13), s * 4 / 36, s * 12 / 36, col);   // "!" bar
    c.rect(X(16), Y(28), s * 4 / 36, s * 4 / 36, col);    // "!" dot
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
        canvas.rect(sb.x + sb.w - 1, sb.y, 1, sb.h, toColor(CLR_SEPARATOR));

        // Brand must stay inside the sidebar — it once painted over the
        // first grid column's art because the sidebar width didn't scale
        // with the text (see recalcLayout()). The truncate is a backstop.
        canvas.textStyled(truncateToWidth(canvas, "MATRIX PLAYER", sb.w - 32,
                                          metrics_.text.body, FontStyle::Bold),
                          16, rcBrand_.bottom * 0.5f - metrics_.text.body * 0.5f,
                          metrics_.text.body, toColor(CLR_ACCENT), FontStyle::Bold);

        // Search box — filters the album grid live as the user types.
        drawSearchField(canvas, rcSearch_, searchQuery_, searchFocused_, "Search",
                        metrics_.text.secondary);

        struct NavItem { const char* label; LayoutRect rc; AlbumTypeFilter filter; };
        NavItem items[] = {
            { "Albums",  rcNavAlbum_,  AlbumTypeFilter::Album  },
            { "EPs",     rcNavEp_,     AlbumTypeFilter::Ep     },
            { "Singles", rcNavSingle_, AlbumTypeFilter::Single },
            { "Remixes", rcNavRemix_,  AlbumTypeFilter::Remix  },
        };
        for (auto& item : items) {
            bool active = (!settingsOpen_ && albumTypeFilter_ == item.filter);
            bool hovered = (hoverSidebarItem_ == (int)item.filter && !active);
            Rect r = toRect(item.rc);
            if (active) {
                // Selected: accent-tint fill + left bar, full height + square —
                // matches the hover highlight exactly (one selection family).
                canvas.rect(r.x + 4, r.y, r.w - 8, r.h,
                            toColor(CLR_ACCENT, UI_SELECT_TINT_ALPHA), UI_CORNER_RADIUS);
                canvas.rect(r.x + 4, r.y, 3.0f, r.h, toColor(CLR_ACCENT), UI_CORNER_RADIUS);
            } else if (hovered) {
                canvas.rect(r.x + 4, r.y, r.w - 8, r.h, toColor(CLR_HOVER), UI_CORNER_RADIUS);
            }
            canvas.text(item.label, r.x + 20, r.y + r.h * 0.5f - metrics_.text.body * 0.5f,
                       metrics_.text.body, toColor(active ? CLR_ACCENT : CLR_TEXT_SECONDARY));
        }

        // Settings gear — spatially separated below a hairline, never mixed
        // into the content-type list above (the user's explicit ask: a
        // music player should read as albums-and-music first, configuration
        // second).
        canvas.rect((float)rcNavGear_.left, rcNavGear_.top - 4.0f * uiScale_, sb.w, 1, toColor(CLR_SEPARATOR));
        {
            bool hovered = (hoverSidebarItem_ == kSidebarGearHit && !settingsOpen_);
            Rect r = toRect(rcNavGear_);
            if (settingsOpen_) {
                canvas.rect(r.x + 4, r.y, r.w - 8, r.h,
                            toColor(CLR_ACCENT, UI_SELECT_TINT_ALPHA), UI_CORNER_RADIUS);
                canvas.rect(r.x + 4, r.y, 3.0f, r.h, toColor(CLR_ACCENT), UI_CORNER_RADIUS);
            } else if (hovered) {
                canvas.rect(r.x + 4, r.y, r.w - 8, r.h, toColor(CLR_HOVER), UI_CORNER_RADIUS);
            }
            float iconHalf = 9.0f * uiScale_;
            LayoutRect gearIconRc = { (int)(rcNavGear_.left + 16.0f * uiScale_), (int)(r.y + r.h * 0.5f - iconHalf),
                                      (int)(rcNavGear_.left + 16.0f * uiScale_ + iconHalf * 2), (int)(r.y + r.h * 0.5f + iconHalf) };
            drawUiIcon(canvas, gearIconRc, UiIcon::Settings,
                      toColor(settingsOpen_ ? CLR_ACCENT : CLR_TEXT_SECONDARY));
        }

        // (The now-playing mini card that used to fill the space below the
        // nav items was removed: it duplicated the transport bar's art,
        // title, and artist — the transport bar is the single now-playing
        // readout now, including the format line next to the BITPERFECT badge.)
    }

    // ── Main content: album grid, settings page, or (below) the full-page
    // album view that replaces the grid while an album is focused ─────────
    if (!settingsOpen_ && !trackPanelOpen_) {
        Rect g = toRect(rcGrid_);
        canvas.rect(g.x, g.y, g.w, g.h, toColor(CLR_BG_MAIN));

        if (albums_.empty()) {
            canvas.text("No albums yet. Use the gear icon below to add a music folder.",
                       g.x + g.w * 0.5f - 160, g.y + 100, metrics_.text.body, toColor(CLR_TEXT_DIM));
        } else if (gridIndices_.empty()) {
            std::string msg;
            if (!searchQuery_.empty()) {
                msg = "No matches for \"" + searchQuery_ + "\"";
            } else {
                const char* filterLabel =
                    albumTypeFilter_ == AlbumTypeFilter::Ep     ? "EPs" :
                    albumTypeFilter_ == AlbumTypeFilter::Single ? "Singles" :
                    albumTypeFilter_ == AlbumTypeFilter::Remix  ? "Remixes" : "Albums";
                msg = std::string("No ") + filterLabel + " yet";
            }
            canvas.text(msg, g.x + g.w * 0.5f - 120, g.y + 100, metrics_.text.body, toColor(CLR_TEXT_DIM));
        } else {
            canvas.setClip(g.x, g.y, g.w, g.h);
            int tileSpaceW = rcGrid_.right - rcGrid_.left - gridPadX_ * 2;
            int tileStepX = gridCols_ > 1 ? tileSpaceW / gridCols_ : gridTileSize_;
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

                    float x = (float)(rcGrid_.left + gridPadX_ + col * tileStepX + (tileStepX - gridArtSize_) / 2);
                    float y = (float)(rcGrid_.top + gridPadY_ + row * tileStepY - gridScrollY_);
                    float a = (float)gridArtSize_;

                    bool nowPlaying = isPlaying_ && idx == displayAlbum_;

                    // Hover: neutral grey focus frame (hover is never accent —
                    // accent signals state only). A grey rounded rect slightly
                    // larger than the art reads as a border halo once the art
                    // (drawn above the vector layer) covers its center.
                    if (hoverAlbumIdx_ == idx && !nowPlaying && selectedAlbumIdx_ != idx) {
                        canvas.rect(x - 6, y - 6, a + 12, a + 12, toColor(CLR_HOVER), UI_CORNER_RADIUS);
                    }
                    // Now-playing: unmistakable green glow border (stronger
                    // than hover, stronger than selection).
                    if (nowPlaying) {
                        canvas.rect(x - 9, y - 9, a + 18, a + 18, toColor(CLR_ACCENT, 0.20f), 12.0f);
                        canvas.rect(x - 6, y - 6, a + 12, a + 12, toColor(CLR_ACCENT, 0.45f), 10.0f);
                        canvas.rect(x - 3, y - 3, a + 6,  a + 6,  toColor(CLR_ACCENT),        8.0f);
                    } else if (selectedAlbumIdx_ == idx) {
                        canvas.rect(x - 3, y - 3, a + 6, a + 6, toColor(CLR_ACCENT, 0.8f), 8.0f);
                    }

                    // Quality-color frame — objective audio-quality metadata,
                    // hugging the art's own bounds (not the outer state rings
                    // above, which sit further out and never overlap this).
                    QualityColor qc = qualityColorFor(alb.avgSampleRate, alb.hasDsd);
                    if (qc.hasColor) {
                        float bw = 2.0f * uiScale_;
                        canvas.rect(x - bw, y - bw, a + bw * 2, bw, toColor(qc.color));
                        canvas.rect(x - bw, y + a, a + bw * 2, bw, toColor(qc.color));
                        canvas.rect(x - bw, y - bw, bw, a + bw * 2, toColor(qc.color));
                        canvas.rect(x + a, y - bw, bw, a + bw * 2, toColor(qc.color));
                    }

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
                        canvas.rect(x, y + a + 2, a, 2, toColor(CLR_ACCENT, 0.4f));

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
                    float ty = y + a + 10.0f * uiScale_;
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
                    // aligns across tiles whether titles wrapped or not.
                    centered(truncateToWidth(canvas, alb.artist, textMaxW, metrics_.text.secondary, FontStyle::Italic),
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
        // renders MSDF, and its baseline convention differs, so labels came
        // out visibly off-center both ways.
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
            float bt = isActiveModeRow ? 2.0f : 1.0f;
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

            float pad = SP_XL * uiScale_;
            float scroll = (float)trackScrollY_;
            float artSize = std::min(tp.w * 0.32f, tp.h * 0.55f);
            float artX = tp.x + pad;
            float artY = tp.y + pad + 16 - scroll;
            // The art scrolls with the page. imageFg isn't clipped by
            // setClip, but the art sits at the top of the content, so
            // scrolling only ever moves it up off the window — never down
            // over the transport bar.
            drawArtOrPlaceholder(canvas, trackPanelArtTex_, artX, artY, artSize, artSize);

            // Right column: title block + track list.
            float colX = artX + artSize + 40.0f * uiScale_;
            float colW = tp.x + tp.w - pad - colX;
            float y = artY + 4;

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
            std::string badge = formatQualityBadge(maxRate, maxBit);
            if (!badge.empty()) {
                canvas.textStyled(badge, colX, y, metrics_.text.caption,
                                  toColor(CLR_TEXT_DIM), FontStyle::Math);
                y += metrics_.text.caption * 1.8f;
            }
            y += 6;
            canvas.rect(colX, y, colW, 1, toColor(CLR_SEPARATOR));
            y += 12;

            // Hit-test anchors for trackPanelHitTest() (scroll-0 baseline).
            trackListTop_   = (int)(y + scroll);
            trackListLeft_  = (int)colX;
            trackListRight_ = (int)(colX + colW);

            // Duration column width measured once (widest realistic stamp),
            // so titles reserve real space instead of a guessed constant.
            float durColW = canvas.textWidthStyled("88:88", metrics_.text.secondary, FontStyle::Math);

            // Quality-color "aura": if every track shares the same tier, the
            // whole list gets one border below; otherwise each row gets its
            // own (drawn per-row in the loop). Recomputed live each time the
            // album view opens — cheap (O(track count), already in RAM),
            // exactly like the Android reference does (it doesn't cache
            // this check either — only the per-album inputs are cached).
            bool qualityMixed = false;
            QualityColor unifiedQuality{};
            bool unifiedSet = false;
            for (auto& t : album.tracks) {
                QualityColor tc = qualityColorFor(t.sampleRate, false);
                if (!unifiedSet) { unifiedQuality = tc; unifiedSet = true; }
                else if (tc.hasColor != unifiedQuality.hasColor || tc.color != unifiedQuality.color) {
                    qualityMixed = true;
                    break;
                }
            }

            for (int i = 0; i < (int)album.tracks.size(); i++) {
                float rowY = y + i * trackRowHeight_;
                if (rowY + trackRowHeight_ < tp.y) continue;
                if (rowY > tp.y + tp.h) break;

                bool isPlayingRow = (displayAlbum_ == selectedAlbumIdx_ && displayTrack_ == i && isPlaying_);
                float rpx = colX - 12, rpw = colW + 24;
                if (isPlayingRow) {
                    // Playing row: accent-tint pill + left bar (one selection family) —
                    // full height + square, matching the hover highlight exactly.
                    canvas.rect(rpx, rowY, rpw, (float)trackRowHeight_,
                                toColor(CLR_ACCENT, UI_SELECT_TINT_ALPHA), UI_CORNER_RADIUS);
                    canvas.rect(rpx, rowY, 3.0f, (float)trackRowHeight_,
                                toColor(CLR_ACCENT), UI_CORNER_RADIUS);
                } else if (hoverTrackIdx_ == i) {
                    canvas.rect(rpx, rowY, rpw, (float)trackRowHeight_, toColor(CLR_HOVER), UI_CORNER_RADIUS);
                }

                if (qualityMixed) {
                    QualityColor tc = qualityColorFor(album.tracks[i].sampleRate, false);
                    if (tc.hasColor) {
                        float bw = 1.5f * uiScale_;
                        canvas.rect(rpx, rowY, rpw, bw, toColor(tc.color));
                        canvas.rect(rpx, rowY + trackRowHeight_ - bw, rpw, bw, toColor(tc.color));
                        canvas.rect(rpx, rowY, bw, (float)trackRowHeight_, toColor(tc.color));
                        canvas.rect(rpx + rpw - bw, rowY, bw, (float)trackRowHeight_, toColor(tc.color));
                    }
                }

                // Track number / duration are numeric readouts: Mono (repurposed
                // Math style slot) keeps digits from jittering column-to-column.
                int trackNum = album.tracks[i].trackNumber > 0 ? album.tracks[i].trackNumber : i + 1;
                std::string trackNumStr = std::to_string(trackNum);
                // Baselines centered by the actual text size (the old "-6"
                // magic offset drifted across resolutions), columns scaled.
                float numColW = 30.0f * uiScale_, titleX = 46.0f * uiScale_;
                float trackNumW = canvas.textWidthStyled(trackNumStr, metrics_.text.body, FontStyle::Math);
                canvas.textStyled(trackNumStr, colX + numColW - trackNumW,
                                rowY + trackRowHeight_ * 0.5f - metrics_.text.body * 0.5f,
                                metrics_.text.body, toColor(isPlayingRow ? CLR_ACCENT : CLR_TEXT_SECONDARY), FontStyle::Math);
                // Base-name priority: only the trailing "(from the Netflix
                // Series...)" modifier ever gets truncated, never the name.
                float titleMaxW = colW - titleX - durColW - 16.0f * uiScale_;
                FontStyle rowStyle = isPlayingRow ? FontStyle::Bold : FontStyle::Roman;
                drawNameWithModifier(canvas, album.tracks[i].title,
                                     colX + titleX,
                                     rowY + trackRowHeight_ * 0.5f - metrics_.text.body * 0.5f,
                                     titleMaxW, metrics_.text.body,
                                     isPlayingRow ? CLR_ACCENT : CLR_TEXT_PRIMARY, rowStyle);

                int durMs = album.tracks[i].durationMs;
                if (durMs > 0) {
                    char durBuf[16];
                    snprintf(durBuf, sizeof(durBuf), "%d:%02d", durMs / 60000, (durMs % 60000) / 1000);
                    float durW = canvas.textWidthStyled(durBuf, metrics_.text.secondary, FontStyle::Math);
                    canvas.textStyled(durBuf, colX + colW - durW,
                                    rowY + trackRowHeight_ * 0.5f - metrics_.text.secondary * 0.5f,
                                    metrics_.text.secondary, toColor(CLR_TEXT_SECONDARY), FontStyle::Math);
                }
            }
            float tracksBottom = y + (float)album.tracks.size() * trackRowHeight_;

            if (!qualityMixed && unifiedQuality.hasColor) {
                float lb = 2.0f * uiScale_;
                float lx = (float)trackListLeft_, rx = (float)trackListRight_;
                canvas.rect(lx - lb, y - lb, (rx - lx) + lb * 2, lb, toColor(unifiedQuality.color));
                canvas.rect(lx - lb, tracksBottom, (rx - lx) + lb * 2, lb, toColor(unifiedQuality.color));
                canvas.rect(lx - lb, y - lb, lb, (tracksBottom - y) + lb * 2, toColor(unifiedQuality.color));
                canvas.rect(rx, y - lb, lb, (tracksBottom - y) + lb * 2, toColor(unifiedQuality.color));
            }

            // ── Sidecar text sections (album description, artist bio) ──
            float sectY = std::max(tracksBottom, artY + artSize) + 36.0f;
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
                yy += 28.0f;
            };
            drawSection("ABOUT THIS ALBUM", albumDescLines_, sectY);
            if (!artistBioLines_.empty()) {
                // Artist image above the bio (only when it fits above the
                // panel's bottom edge — imageFg composites over the
                // transport bar otherwise).
                if (artistImgTex_ != kInvalidTexture) {
                    float imgSize = 120.0f * uiScale_;
                    if (sectY + imgSize <= tp.y + tp.h && sectY + imgSize > tp.y)
                        canvas.imageFg(artistImgTex_, tp.x + pad, sectY, imgSize, imgSize);
                    sectY += imgSize + 16.0f;
                }
                drawSection(album.artist.empty() ? std::string("ABOUT THE ARTIST")
                                                 : album.artist, artistBioLines_, sectY);
            }
            albumViewContentH_ = (int)(sectY + scroll - tp.y + pad);

            canvas.clearClip();
        }

        // No on-screen close button — Escape closes the album view.
    }

    // ── Transport bar ────────────────────────────────────────────────────
    {
        Rect t = toRect(rcTransport_);
        canvas.rect(t.x, t.y, t.w, t.h, toColor(CLR_BG_TRANSPORT));
        canvas.rect(t.x, t.y, t.w, 1, toColor(CLR_SEPARATOR));

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
        // (source format » DSP stage » output backend). '»' (U+00BB) is in
        // the baked Latin-1 range; '→' (U+2192) is not, so don't swap it in.
        {
            bool bp = bitperfectMode_.load();
            const char* dsp = bp ? "BITPERFECT" : "REF EQ";
            ColorRef dspClr = bp ? CLR_ACCENT : CLR_TEXT_DIM;
            float rightEdge = t.x + t.w - 16;
            float cy = t.y + t.h * 0.5f;
            float tagW = canvas.textWidthStyled(dsp, metrics_.text.caption, FontStyle::Math);

            // Hover hit rect always tracks the compact tag's home (with a
            // little slop), so the hover state stays stable while the
            // expanded readout is showing.
            rcDspBadge_ = { (int)(rightEdge - tagW - 8), (int)(cy - metrics_.text.caption),
                            (int)(rightEdge + 8),        (int)(cy + metrics_.text.caption) };

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
                canvas.textStyled(timeBuf, rightEdge - tagW - 24 - timeW,
                                  cy - metrics_.text.secondary * 0.5f,
                                  metrics_.text.secondary, toColor(CLR_TEXT_SECONDARY), FontStyle::Math);
            }
        }
    }

    // (No on-screen mode toggle — Alt+L switches Essential/Complete.)

    // ── Bitperfect warning banner (non-modal, both platforms) ─────────────
    if (!bitperfectWarning_.empty()) {
        Rect w = toRect(rcBitperfectWarning_);
        canvas.rect(w.x, w.y, w.w, w.h, toColor(CLR_WARNING, UI_SELECT_TINT_ALPHA));
        canvas.rect(w.x, w.y, w.w, 1, toColor(CLR_WARNING));               // top hairline
        canvas.rect(w.x, w.y + w.h - 1, w.w, 1, toColor(CLR_WARNING));     // bottom hairline

        float iconSize = w.h - 8.0f;
        LayoutRect iconRc = { (int)(w.x + 8), (int)(w.y + 4),
                              (int)(w.x + 8 + iconSize), (int)(w.y + 4 + iconSize) };
        drawWarningIcon(canvas, iconRc, toColor(CLR_WARNING));

        float textX = iconRc.right + 8.0f;
        float textY = w.y + w.h * 0.5f - metrics_.text.secondary * 0.5f;
        canvas.text(bitperfectWarning_, textX, textY, metrics_.text.secondary, toColor(CLR_WARNING));
    }

    renderer_->draw(frameCurves_, /*overlay_rotation_deg=*/0, frameImages_, frameImagesFg_, msdfQuads_, frameShapes_);
}

// ── Layout ───────────────────────────────────────────────────────────────────

void PlayerWindow::recalcLayout() {
    int W = (int)renderer_->width(), H = (int)renderer_->height();

    // Type roles + geometry factor, both from the window's content height.
    metrics_ = computeUiMetrics((float)H);

    // LEGACY, being retired: reproduce the old factor exactly so that call
    // sites not yet migrated keep rendering at their current size. 13.0f was
    // the old "nav" role's calibration size and 661.0f its reference height —
    // both are gone from the type scale now, so the formula is spelled out
    // here rather than derived. Deleted once nothing multiplies by uiScale_.
    uiScale_ = std::max(13.0f / 661.0f * (float)H, kMinReadableTextSizePx) / 13.0f;

    if (uiMode_ == UiMode::Essential) {
        // Phone-shaped panel (see computeEssentialWindowRect()): art fills
        // most of it, title band below, prev/play-stop/next centered near
        // the bottom. No seek bar, no artist text, per design.
        int margin = std::max(12, W / 20);
        int bottomReserve = std::max(120, H / 5);
        int artSize = std::min({ W - margin * 2, H - bottomReserve - margin, H });
        int artX = (W - artSize) / 2;
        int artY = margin;  // (no corner toggle button to clear anymore)
        rcEssentialArt_ = { artX, artY, artX + artSize, artY + artSize };

        int titleY = rcEssentialArt_.bottom + 12;
        rcEssentialTitle_ = { margin, titleY, W - margin, titleY + 30 };

        int btnSize = std::max(40, W / 8);
        int btnGap  = std::max(16, W / 12);
        int totalBtnW = btnSize * 3 + btnGap * 2;
        int btnX = (W - totalBtnW) / 2;
        int btnY = H - margin - btnSize;
        rcEssentialPrev_     = { btnX, btnY, btnX + btnSize, btnY + btnSize };
        btnX += btnSize + btnGap;
        rcEssentialPlayStop_ = { btnX, btnY, btnX + btnSize, btnY + btnSize };
        btnX += btnSize + btnGap;
        rcEssentialNext_     = { btnX, btnY, btnX + btnSize, btnY + btnSize };
        return;
    }

    // Every fixed-pixel value below is authored at the 1080 reference height
    // and passed through metrics_.space() — see ui_metrics.hh. Values that used
    // to read `X * us` were re-authored as trunc(X * 1.63389), the old factor at
    // 1080: TRUNCATED, not rounded, because the original code cast with (int).
    // Rounding instead shifts these by a pixel each and compounds to ~8px on the
    // settings rows. Values consumed as floats and accumulated (navRowH, navTop,
    // gearOffset) keep their fraction. Values that were bare kept their number.
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

    int gridW = rcGrid_.right - rcGrid_.left - (int)metrics_.space((float)gridPadX_) * 2;
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

    // Tile text block height from the ACTUAL text sizes (two title lines +
    // artist + breathing room) — see gridRowGap_'s comment in the header.
    gridRowGap_ = (int)(titleArtistAdvance(metrics_.text.body) * 2.0f
                        + metrics_.text.secondary * 1.35f + metrics_.space(29.41f));

    // Track rows likewise scale with their text.
    trackRowHeight_ = (int)metrics_.space(65.0f);

    int albumRows = ((int)gridIndices_.size() + gridCols_ - 1) / gridCols_;
    gridTotalHeight_ = albumRows * (gridTileSize_ + gridRowGap_) + (int)metrics_.space((float)gridPadY_);

    // Sidebar items — search box sits between the brand and the nav. All
    // Y positions scale with the text (fixed values put "Albums" visibly
    // adrift of the search box across resolutions).
    rcBrand_       = { 0, 0, sidebarW, (int)metrics_.space(81.0f) };
    const int searchInset = (int)metrics_.space(12.0f);
    rcSearch_      = { searchInset, (int)metrics_.space(94.0f),
                       sidebarW - searchInset, (int)metrics_.space(147.0f) };
    float navRowH = metrics_.space(65.3556f), navTop = metrics_.space(166.6568f);
    rcNavAlbum_  = { 0, (int)(navTop),               sidebarW, (int)(navTop + navRowH) };
    rcNavEp_     = { 0, (int)(navTop + navRowH),     sidebarW, (int)(navTop + navRowH * 2) };
    rcNavSingle_ = { 0, (int)(navTop + navRowH * 2), sidebarW, (int)(navTop + navRowH * 3) };
    rcNavRemix_  = { 0, (int)(navTop + navRowH * 3), sidebarW, (int)(navTop + navRowH * 4) };
    const float gearOffset = metrics_.space(13.0711f);
    rcNavGear_   = { 0, (int)(navTop + navRowH * 4 + gearOffset),
                        sidebarW, (int)(navTop + navRowH * 5 + gearOffset) };

    // Transport sub-regions — proportional to the (scaled) bar height.
    int tTop = rcTransport_.top;
    int tPad = (int)metrics_.space(19.0f);
    int artSide = transportH - 2 * tPad;
    rcTransportArt_  = { tPad, tTop + tPad, tPad + artSide, tTop + tPad + artSide };

    // Bitperfect-mismatch warning strip: a full-width overlay directly above
    // the transport bar. Doesn't reserve/shrink grid space — this is a rare,
    // transient event, not worth a permanent layout dependency.
    int warnH = (int)metrics_.space(45.0f);
    rcBitperfectWarning_ = { 0, tTop - warnH, W, tTop };

    // Center buttons: the app's primary interactive elements (44px at the
    // reference window; scaled like everything else). Three of them:
    // prev / play-stop / next (no pause, no separate stop).
    int btnSize = (int)metrics_.space(71.0f);
    int btnGap = (int)metrics_.space(19.0f);
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

TextureHandle PlayerWindow::getGridArtTexture(int albumIdx) {
    auto it = gridArtTexCache_.find(albumIdx);
    if (it != gridArtTexCache_.end()) {
        gridArtLastUse_[albumIdx] = ++artUseTick_;
        return it->second;
    }
    if (albumIdx < 0 || albumIdx >= (int)albums_.size()) return kInvalidTexture;

    // Not cached: queue an async decode (once) and show the placeholder this
    // frame — onArtDecoded() invalidates when the texture is ready.
    if (artDecodePending_.emplace(albumIdx, 1).second) {
        if (!artDecodeThread_.joinable())
            artDecodeThread_ = std::thread([this]{ artDecodeWorker(); });
        {
            std::lock_guard<std::mutex> lk(artDecodeMu_);
            artDecodeQueue_.push_back({albumIdx, albums_[albumIdx].artPath,
                                       gridArtSize_, artCacheGen_.load()});
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
        res.albumIdx = job.albumIdx;
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
        artDecodePending_.erase(r.albumIdx);
        if (r.gen != artCacheGen_.load()) continue;  // rescan invalidated it
        if (gridArtTexCache_.count(r.albumIdx)) continue;  // duplicate job — keep the existing texture
        // Failed decodes cache kInvalidTexture so the tile keeps its
        // placeholder without re-queuing a doomed decode every frame —
        // same behavior the old synchronous path had.
        TextureHandle tex = kInvalidTexture;
        if (!r.rgba.empty())
            // Grid art is decoded to the tile size it's drawn at (≤2x) —
            // skip the mip chain: −33% VRAM per tile, no blit pass.
            tex = renderer_->create_texture(r.rgba.data(), (uint32_t)r.w, (uint32_t)r.h,
                                            /*mips=*/false);
        gridArtTexCache_[r.albumIdx] = tex;
        gridArtLastUse_[r.albumIdx] = ++artUseTick_;
        anyNew = true;
    }

    // LRU eviction: keep VRAM bounded on libraries with more albums than the
    // cap. Only entries not drawn recently get dropped, so everything visible
    // (stamped this frame via getGridArtTexture) always survives.
    while (gridArtTexCache_.size() > kMaxGridArtTextures) {
        int oldestIdx = -1;
        uint64_t oldestTick = UINT64_MAX;
        for (auto& [idx, tex] : gridArtTexCache_) {
            auto u = gridArtLastUse_.find(idx);
            uint64_t tick = (u != gridArtLastUse_.end()) ? u->second : 0;
            if (tick < oldestTick) { oldestTick = tick; oldestIdx = idx; }
        }
        if (oldestIdx < 0) break;
        auto e = gridArtTexCache_.find(oldestIdx);
        if (e->second != kInvalidTexture) renderer_->destroy_texture(e->second);
        gridArtTexCache_.erase(e);
        gridArtLastUse_.erase(oldestIdx);
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
    // Layout first: loadTrackPanelArtTexture() sizes its texture from
    // rcTrackPanel_, which recalcLayout() just grew to the full page.
    recalcLayout();
    loadTrackPanelArtTexture(albumIdx);
    loadAlbumViewContent(albumIdx);
    invalidate();
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
                if (!info->imagePath.empty()) {
                    FileByteReader reader;
                    artistImgTex_ = createTextureFromImageFile(*renderer_, reader,
                                                               info->imagePath.c_str(), 256, 256);
                }
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
            if (!artistImg.empty()) {
                FileByteReader reader;
                artistImgTex_ = createTextureFromImageFile(*renderer_, reader,
                                                           artistImg.c_str(), 256, 256);
            }
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
    // window in sync while it's open.
    if (artWin_.isVisible()) artWin_.updateImage(artPath);
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

void PlayerWindow::rebuildGridIndices() {
    gridIndices_.clear();
    gridIndices_.reserve(albums_.size());
    for (int i = 0; i < (int)albums_.size(); i++) {
        const Album& a = albums_[i];
        if ((int)a.releaseType != (int)albumTypeFilter_) continue;
        if (!searchQuery_.empty()) {
            bool hit = containsNoCase(a.displayName, searchQuery_) ||
                       containsNoCase(a.artist, searchQuery_);
            for (size_t t = 0; !hit && t < a.tracks.size(); t++)
                hit = containsNoCase(a.tracks[t].title, searchQuery_);
            if (!hit) continue;
        }
        gridIndices_.push_back(i);
    }
}

int PlayerWindow::gridHitTest(int x, int y) const {
    if (trackPanelOpen_) return -1;  // grid is hidden behind the album view
    if (x < rcGrid_.left || x >= rcGrid_.right || y < rcGrid_.top || y >= rcGrid_.bottom)
        return -1;
    int gridW = rcGrid_.right - rcGrid_.left;
    int tileSpaceW = gridW - gridPadX_ * 2;
    int tileStepX = gridCols_ > 1 ? tileSpaceW / gridCols_ : gridTileSize_;
    int tileStepY = gridTileSize_ + gridRowGap_;

    int col = (x - rcGrid_.left - gridPadX_) / tileStepX;
    int row = (y - rcGrid_.top - gridPadY_ + gridScrollY_) / tileStepY;
    if (col < 0 || col >= gridCols_ || row < 0) return -1;
    int idx = row * gridCols_ + col;
    if (idx >= (int)gridIndices_.size()) return -1;

    // Only the artwork itself is a target — the gaps, the text block below,
    // and the cell margins are dead space. Same art-position math as the
    // grid draw block in drawFrame().
    int artX = rcGrid_.left + gridPadX_ + col * tileStepX + (tileStepX - gridArtSize_) / 2;
    int artY = rcGrid_.top + gridPadY_ + row * tileStepY - gridScrollY_;
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
    // trackListTop_ is the scroll-0 window Y of row 0 (written by the album
    // view draw block); rows scroll with the page.
    int listTopNow = trackListTop_ - trackScrollY_;
    if (y < listTopNow) return -1;
    int row = (y - listTopNow) / trackRowHeight_;
    if (selectedAlbumIdx_ < 0 || selectedAlbumIdx_ >= (int)albums_.size()) return -1;
    if (row < 0 || row >= (int)albums_[selectedAlbumIdx_].tracks.size()) return -1;
    return row;
}

int PlayerWindow::sidebarHitTest(int x, int y) const {
    if (ptInRect(rcNavAlbum_, x, y))  return (int)AlbumTypeFilter::Album;
    if (ptInRect(rcNavEp_, x, y))     return (int)AlbumTypeFilter::Ep;
    if (ptInRect(rcNavSingle_, x, y)) return (int)AlbumTypeFilter::Single;
    if (ptInRect(rcNavRemix_, x, y))  return (int)AlbumTypeFilter::Remix;
    if (ptInRect(rcNavGear_, x, y))   return kSidebarGearHit;
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

    if (uiMode_ == UiMode::Essential) {
        int oldHoverEssential = hoverEssentialBtn_;
        hoverEssentialBtn_ = essentialHitTest(x, y);
        if (hoverEssentialBtn_ != oldHoverEssential)
            invalidate();
        return;
    }

    int oldHoverAlbum = hoverAlbumIdx_;
    int oldHoverTrack = hoverTrackIdx_;
    int oldHoverSidebar = hoverSidebarItem_;
    int oldHoverTransBtn = hoverTransportBtn_;
    int oldHoverSettings = hoverSettingsItem_;
    bool oldHoverDsp = hoverDspBadge_;

    hoverAlbumIdx_ = -1;
    hoverTrackIdx_ = -1;
    hoverSidebarItem_ = -1;
    hoverTransportBtn_ = -1;
    hoverSettingsItem_ = -1;
    hoverDspBadge_ = false;


    if (ptInRect(rcSidebar_, x, y)) {
        hoverSidebarItem_ = sidebarHitTest(x, y);
    } else if (ptInRect(rcTransport_, x, y)) {
        hoverTransportBtn_ = transportBtnHitTest(x, y);
        hoverDspBadge_ = ptInRect(rcDspBadge_, x, y) != 0;
    } else if (trackPanelOpen_ && !settingsOpen_ && ptInRect(rcTrackPanel_, x, y)) {
        hoverTrackIdx_ = trackPanelHitTest(x, y);
    } else if (ptInRect(rcGrid_, x, y)) {
        if (!settingsOpen_)
            hoverAlbumIdx_ = gridHitTest(x, y);
        else
            hoverSettingsItem_ = settingsHitTest(x, y);
    }

    bool changed = (hoverAlbumIdx_ != oldHoverAlbum ||
                    hoverTrackIdx_ != oldHoverTrack ||
                    hoverSidebarItem_ != oldHoverSidebar ||
                    hoverTransportBtn_ != oldHoverTransBtn ||
                    hoverSettingsItem_ != oldHoverSettings ||
                    hoverDspBadge_ != oldHoverDsp);
    if (changed)
        invalidate();
}

void PlayerWindow::onMouseLeave() {
    mouseTracking_ = false;
    hoverAlbumIdx_ = -1;
    hoverTrackIdx_ = -1;
    hoverSidebarItem_ = -1;
    hoverTransportBtn_ = -1;
    hoverSettingsItem_ = -1;
    hoverDspBadge_ = false;
    hoverEssentialBtn_ = -1;
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

    // Search box focus: clicking it starts typing; clicking anywhere else
    // releases focus (the query itself stays, still filtering).
    {
        bool wasFocused = searchFocused_;
        searchFocused_ = ptInRect(rcSearch_, x, y) != 0;
        if (searchFocused_ != wasFocused) invalidate();
        if (searchFocused_) return;
    }

    // Bitperfect warning banner: click anywhere on it dismisses.
    if (!bitperfectWarning_.empty() && ptInRect(rcBitperfectWarning_, x, y)) {
        bitperfectWarning_.clear();
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

    // Sidebar
    if (ptInRect(rcSidebar_, x, y)) {
        int nav = sidebarHitTest(x, y);
        if (nav == kSidebarGearHit) {
            if (!settingsOpen_) { settingsOpen_ = true; invalidate(); }
        } else if (nav >= 0 &&
                   (settingsOpen_ || albumTypeFilter_ != (AlbumTypeFilter)nav)) {
            settingsOpen_ = false;
            trackPanelOpen_ = false;
            albumTypeFilter_ = (AlbumTypeFilter)nav;
            rebuildGridIndices();
            gridScrollY_ = 0;
            recalcLayout();
            invalidate();
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
            currentAlbum_ = selectedAlbumIdx_;
            currentTrack_ = track;
            onPlay();
        }
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
    int wanted = currentTrack_ + 1;
    if (wanted >= (int)albums_[currentAlbum_].tracks.size()) return;

    // Seamless path: if we're playing, the prepared nextDecoder_ matches the
    // requested track, and its (sampleRate, channels) match the running USB
    // output, hand off via the gapless coordinator. The output stream stays
    // alive — no stop()/configure()/start() cycle, no working-set re-lock
    // dance, no cold-start transient.
    //
    // Decoder::stop() does not fire the done callback (only natural EOF
    // does), so we signal gaplessSignal_ ourselves after stopping the
    // current decoder. flush() drops the ~3 s of stale tail still queued in
    // the ring so the user actually hears the next track promptly.
    if (isPlaying_ && active_ && output_ &&
        nextAlbum_ == currentAlbum_ && nextTrack_ == wanted) {
        Decoder* incoming = (active_ == &decoder_) ? &nextDecoder_ : &decoder_;
        if (incoming->sampleRate() == output_->getConfiguredRate() &&
            incoming->channels()   == output_->getConfiguredChannels()) {
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

    currentTrack_ = wanted;
    onPlay();
}

void PlayerWindow::onPrev() {
    if (currentAlbum_ < 0 || currentTrack_ < 0) return;
    if (seekPosMs_ > 3000) {
        onSeek(0);   // rebases playedFrames_ to the current track's start
        return;
    }
    if (currentTrack_ > 0) {
        currentTrack_--;
        onPlay();
    }
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

        if (ptInRect(eqBtnAssign_, x, y)) {
            if (eqSelectedRow_ >= 0 && eqSelectedRow_ < (int)eqFilteredIndices_.size()) {
                int idx = eqFilteredIndices_[eqSelectedRow_];
                auto& p = eqProfiles_.getAll()[idx];
                db_.saveEqAssignment(eqDeviceKey_, p.name, p.source, p.form);
                if (!eqBitperfectActive_) {
                    auto* profile = eqProfiles_.findByKey(p.name, p.source, p.form);
                    int sr = 44100, ch = 2;
                    if (output_) {
                        int r = output_->getConfiguredRate(), c2 = output_->getConfiguredChannels();
                        if (r > 0) sr = r;
                        if (c2 > 0) ch = c2;
                    }
                    if (profile) eqManager_.applyProfile(profile, sr, ch);
                }
                invalidate();
            }
            return;
        }
        if (ptInRect(eqBtnClear_, x, y)) {
            db_.clearEqAssignment(eqDeviceKey_);
            eqManager_.clear();
            invalidate();
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
        int contentH = (int)mfRoots_.size() * kPanelRowH;
        mfScrollY_ = std::clamp(mfScrollY_ - delta, 0, std::max(0, contentH - listH));
        invalidate();
        return;
    }
    case SettingsPanel::EqSettings: {
        int listH = eqListArea_.bottom - eqListArea_.top;
        int contentH = (int)eqFilteredIndices_.size() * kPanelRowH;
        eqScrollY_ = std::clamp(eqScrollY_ - delta, 0, std::max(0, contentH - listH));
        invalidate();
        return;
    }
    case SettingsPanel::FolderPicker: {
        int rowCount = (int)fpEntries_.size() + (fpHasParent_ ? 1 : 0);
        int listH = fpListArea_.bottom - fpListArea_.top;
        int contentH = rowCount * kPanelRowH;
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
        int contentH = rowCount * kPanelRowH;
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
    LayoutRect content = panels::drawHeader(canvas, area, "Music Folders", uiScale_, metrics_.text.header, mfCloseRc_);
    float pad = SP_LG * uiScale_;
    float btnH = 36.0f * uiScale_;

    LayoutRect listArea = { content.left, (int)(content.top + pad),
                            content.right, (int)(content.bottom - (btnH + pad * 2)) };
    mfListArea_ = listArea;
    mfListRows_ = widgets::drawScrollList(canvas, toRect(listArea), mfRoots_,
                                          mfSelectedRow_, (float)mfScrollY_, (float)kPanelRowH,
                                          mfHoverRow_, widgets::kTextFree, matrixListStyle());
    panels::drawScrollbar(canvas, listArea, (int)mfRoots_.size() * kPanelRowH, mfScrollY_, uiScale_);
    if (mfRoots_.empty()) {
        Rect a = toRect(listArea);
        canvas.textStyled("No music folders added yet.", a.x + 14.0f * uiScale_, a.y + 14.0f * uiScale_,
                          metrics_.text.body, toColor(CLR_TEXT_DIM), FontStyle::Italic);
    }

    float btnW = 170.0f * uiScale_;
    int by = (int)(content.bottom - (btnH + pad));
    mfBtnRemove_ = { content.left + (int)pad, by, (int)(content.left + pad + btnW), (int)(by + btnH) };
    mfBtnDone_   = { (int)(content.right - pad - btnW), by, content.right - (int)pad, (int)(by + btnH) };
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
        JackOutput probe;
        asJackPorts_ = probe.enumeratePorts();   // opens a throwaway client just to query the graph
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
    LayoutRect content = panels::drawHeader(canvas, area, "Audio Output Settings", uiScale_, metrics_.text.header, asCloseRc_);
    Rect c = toRect(content);
    float pad = SP_LG * uiScale_;
    float y = c.y + pad;

    canvas.textStyled("Output backend:", c.x + pad, y, metrics_.text.body, toColor(CLR_TEXT_DIM), FontStyle::Roman);
    y += metrics_.text.body * 1.8f;

    float rowH = 34.0f * uiScale_;
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
    y += 12.0f * uiScale_;

    AudioBackend sel = asBackendOptions_.empty() ? AudioBackend::Usb : asBackendOptions_[asBackendSelIdx_];

    // The device list hugs its content and grows into whatever space is left
    // above the bottom-docked Apply button, instead of a fixed 6-row window.
    // With 10+ ALSA devices that fixed height hid everything past row 6 behind
    // a scroll with no affordance — drawScrollList clips silently and draws no
    // scrollbar, so a DAC in row 7 simply looked absent.
    float btnH = 36.0f * uiScale_;
    float listTop = y + metrics_.text.body * 1.6f;   // every branch draws its label first
    float listBottomLimit = (float)content.bottom - pad - btnH - pad;
#ifdef _WIN32
    if (sel == AudioBackend::Wasapi)             // the Mode radios sit below the list
        listBottomLimit -= 12.0f * uiScale_ + metrics_.text.body * 1.6f + 2.0f * rowH + 12.0f * uiScale_;
#endif
    // 3-row floor keeps the empty-state messages below readable.
    auto listHeightFor = [&](int rowCount) {
        float minH  = 3.0f * kPanelRowH;
        float avail = std::max(listBottomLimit - listTop, minH);
        return std::clamp((float)rowCount * kPanelRowH, minH, avail);
    };

    if (sel == AudioBackend::Usb) {
        canvas.textStyled("USB DAC:", c.x + pad, y, metrics_.text.body, toColor(CLR_TEXT_DIM), FontStyle::Roman);
        y += metrics_.text.body * 1.6f;
        std::vector<std::string> labels;
        for (auto& d : asUsbDevices_) labels.push_back(d.name);
        float listH = listHeightFor((int)labels.size());
        asDeviceListArea_ = { (int)(c.x + pad), (int)y, (int)(c.x + c.w - pad), (int)(y + listH) };
        asDeviceListRows_ = widgets::drawScrollList(canvas, toRect(asDeviceListArea_), labels,
                                                    asUsbSel_, (float)asDeviceScrollY_, (float)kPanelRowH,
                                                    asHoverDeviceRow_, widgets::kTextFree, matrixListStyle());
        panels::drawScrollbar(canvas, asDeviceListArea_, (int)labels.size() * kPanelRowH,
                              asDeviceScrollY_, uiScale_);
        if (labels.empty()) {
            Rect a = toRect(asDeviceListArea_);
            canvas.textStyled("No USB audio devices found.", a.x + 14.0f * uiScale_, a.y + 14.0f * uiScale_,
                              metrics_.text.body, toColor(CLR_TEXT_DIM), FontStyle::Italic);
        }
        y += listH + 12.0f * uiScale_;
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
                                                    asWasapiSel_, (float)asDeviceScrollY_, (float)kPanelRowH,
                                                    asHoverDeviceRow_, widgets::kTextFree, matrixListStyle());
        panels::drawScrollbar(canvas, asDeviceListArea_, (int)labels.size() * kPanelRowH,
                              asDeviceScrollY_, uiScale_);
        y += listH + 12.0f * uiScale_;

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
        y += 12.0f * uiScale_;
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
                                                    asAlsaSel_, (float)asDeviceScrollY_, (float)kPanelRowH,
                                                    asHoverDeviceRow_, widgets::kTextFree, matrixListStyle());
        panels::drawScrollbar(canvas, asDeviceListArea_, (int)labels.size() * kPanelRowH,
                              asDeviceScrollY_, uiScale_);
        y += listH + 12.0f * uiScale_;
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
                                                    asJackSel_, (float)asDeviceScrollY_, (float)kPanelRowH,
                                                    asHoverDeviceRow_, widgets::kTextFree, matrixListStyle());
        panels::drawScrollbar(canvas, asDeviceListArea_, (int)labels.size() * kPanelRowH,
                              asDeviceScrollY_, uiScale_);
        if (asJackPorts_.empty()) {
            Rect a = toRect(asDeviceListArea_);
            canvas.textStyled("No running JACK server found (or no physical playback ports).",
                              a.x + 14.0f * uiScale_, a.y + 60.0f * uiScale_,
                              metrics_.text.body, toColor(CLR_TEXT_DIM), FontStyle::Italic);
        }
        y += listH + 12.0f * uiScale_;
    }
#endif
#endif

    float btnW = 120.0f * uiScale_;   // btnH declared above — the list is sized against it
    int by = (int)(content.bottom - (btnH + pad));
    asBtnApply_ = { (int)(content.right - pad - btnW), by, content.right - (int)pad, (int)(by + btnH) };
    panels::drawButton(canvas, asBtnApply_, "Apply", asHoverApply_, metrics_.text.body, true);
}

void PlayerWindow::applyAudioSettingsPanel() {
    onStop();

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
        if (usbOpen_) usbDriver_.parseDescriptors();
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
    eqDeviceKey_ = getActiveDeviceKey();
    eqBitperfectActive_ = bitperfectMode_.load();
    eqSearch_.clear();
    eqSearchFocused_ = false;
    eqSelectedRow_ = -1;
    eqHoverRow_ = -1;
    eqScrollY_ = 0;
    eqHoverClose_ = eqHoverAssign_ = eqHoverClear_ = false;
    eqRefilter();
    activePanel_ = SettingsPanel::EqSettings;
    invalidate();
}

void PlayerWindow::eqRefilter() {
    eqFilteredIndices_.clear();
    std::string needle = eqSearch_;
    for (auto& ch : needle) ch = (char)std::tolower((unsigned char)ch);
    auto& all = eqProfiles_.getAll();
    for (int i = 0; i < (int)all.size(); i++) {
        std::string nameLower = all[i].name;
        for (auto& ch : nameLower) ch = (char)std::tolower((unsigned char)ch);
        if (!needle.empty() && nameLower.find(needle) == std::string::npos) continue;
        eqFilteredIndices_.push_back(i);
    }
    if (eqSelectedRow_ >= (int)eqFilteredIndices_.size()) eqSelectedRow_ = -1;
}

void PlayerWindow::drawEqSettings(Canvas& canvas, const LayoutRect& area) {
    LayoutRect content = panels::drawHeader(canvas, area, "EQ / AutoEQ Profiles", uiScale_, metrics_.text.header, eqCloseRc_);
    Rect c = toRect(content);
    float pad = SP_LG * uiScale_;
    float y = c.y + pad;

    canvas.textStyled("Device: " + eqDeviceKey_, c.x + pad, y, metrics_.text.secondary, toColor(CLR_TEXT_DIM), FontStyle::Roman);
    y += metrics_.text.secondary * 1.6f;

    EqAssignment assign;
    std::string assignLine = "No EQ assigned";
    if (db_.loadEqAssignment(eqDeviceKey_, assign) || db_.loadEqAssignment("global", assign))
        assignLine = "Current EQ: " + assign.name;
    canvas.textStyled(assignLine, c.x + pad, y, metrics_.text.secondary, toColor(CLR_ACCENT), FontStyle::Roman);
    y += metrics_.text.secondary * 1.8f;

    if (eqBitperfectActive_) {
        canvas.textStyled("Bitperfect mode active \xE2\x80\x94 EQ applies once Reference EQ mode is enabled.",
                          c.x + pad, y, metrics_.text.secondary, toColor(CLR_TEXT_DIM), FontStyle::Italic);
        y += metrics_.text.secondary * 1.6f;
    }

    eqSearchRc_ = { (int)(c.x + pad), (int)y, (int)(c.x + c.w - pad), (int)(y + 34.0f * uiScale_) };
    drawSearchField(canvas, eqSearchRc_, eqSearch_, eqSearchFocused_, "Search profiles",
                    metrics_.text.body);
    y += 34.0f * uiScale_ + 10.0f * uiScale_;

    float btnH = 36.0f * uiScale_;
    LayoutRect listArea = { content.left, (int)y, content.right, (int)(content.bottom - (btnH + pad * 2)) };
    eqListArea_ = listArea;

    std::vector<std::string> labels;
    labels.reserve(eqFilteredIndices_.size());
    auto& all = eqProfiles_.getAll();
    for (int idx : eqFilteredIndices_) {
        std::string label = all[idx].name;
        if (!all[idx].form.empty()) label += "  (" + all[idx].form + ")";
        labels.push_back(label);
    }
    eqListRows_ = widgets::drawScrollList(canvas, toRect(listArea), labels,
                                          eqSelectedRow_, (float)eqScrollY_, (float)kPanelRowH,
                                          eqHoverRow_, widgets::kTextFree, matrixListStyle());
    panels::drawScrollbar(canvas, listArea, (int)labels.size() * kPanelRowH, eqScrollY_, uiScale_);
    if (labels.empty()) {
        Rect a = toRect(listArea);
        canvas.textStyled("No profiles match.", a.x + 14.0f * uiScale_, a.y + 14.0f * uiScale_,
                          metrics_.text.body, toColor(CLR_TEXT_DIM), FontStyle::Italic);
    }

    float btnW = 170.0f * uiScale_;
    int by = (int)(content.bottom - (btnH + pad));
    eqBtnAssign_ = { content.left + (int)pad, by, (int)(content.left + pad + btnW), (int)(by + btnH) };
    eqBtnClear_  = { (int)(content.left + pad + btnW + 12.0f * uiScale_), by,
                     (int)(content.left + pad + 2.0f * btnW + 12.0f * uiScale_), (int)(by + btnH) };
    panels::drawButton(canvas, eqBtnAssign_, "Assign to Device", eqHoverAssign_, metrics_.text.body, true);
    panels::drawButton(canvas, eqBtnClear_, "Clear", eqHoverClear_, metrics_.text.body);
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
    LayoutRect content = panels::drawHeader(canvas, area, "Select Music Folder", uiScale_, metrics_.text.header, fpCloseRc_);
    Rect c = toRect(content);
    float pad = SP_LG * uiScale_;

    canvas.textStyled(truncateToWidth(canvas, fpCurrentDir_, c.w - 2.0f * pad, metrics_.text.secondary, FontStyle::Roman),
                      c.x + pad, c.y + pad, metrics_.text.secondary, toColor(CLR_TEXT_DIM), FontStyle::Roman);

    float listTop = pad * 2.0f + metrics_.text.secondary * 1.4f;
    float btnH = 36.0f * uiScale_;
    LayoutRect listArea = { content.left, (int)(content.top + listTop),
                            content.right, (int)(content.bottom - (btnH + pad * 2.0f)) };
    fpListArea_ = listArea;

    std::vector<std::string> labels;
    labels.reserve(fpEntries_.size() + 1);
    if (fpHasParent_) labels.push_back(".. (parent folder)");
    labels.insert(labels.end(), fpEntries_.begin(), fpEntries_.end());

    fpListRows_ = widgets::drawScrollList(canvas, toRect(listArea), labels,
                                          -1, (float)fpScrollY_, (float)kPanelRowH,
                                          fpHoverRow_, widgets::kTextFree, matrixListStyle());
    panels::drawScrollbar(canvas, listArea, (int)labels.size() * kPanelRowH, fpScrollY_, uiScale_);
    if (labels.empty()) {
        Rect a = toRect(listArea);
        canvas.textStyled("No subfolders here.", a.x + 14.0f * uiScale_, a.y + 14.0f * uiScale_,
                          metrics_.text.body, toColor(CLR_TEXT_DIM), FontStyle::Italic);
    }

    float btnW = 200.0f * uiScale_;
    int by = (int)(content.bottom - (btnH + pad));
    fpBtnCancel_ = { content.left + (int)pad, by, (int)(content.left + pad + btnW), (int)(by + btnH) };
    fpBtnSelect_ = { (int)(content.right - pad - btnW), by, content.right - (int)pad, (int)(by + btnH) };
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
        return;
    }
    auto* profile = eqProfiles_.findByKey(assign.name, assign.source, assign.form);
    if (profile)
        eqManager_.applyProfile(profile, sampleRate, channels);
    else
        eqManager_.clear();
}

void PlayerWindow::onPlay() {
    // A fresh play attempt always dismisses a stale bitperfect warning,
    // including the stale-selection early-return path just below.
    bitperfectWarning_.clear();
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

    // Focus the album view on what's now playing
    if (selectedAlbumIdx_ != currentAlbum_)
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
            bitperfectWarning_ = "DAC does not support native sample rate " + std::to_string(fileSr) +
                                  " Hz — Bitperfect playback aborted.";
            invalidate();
        } else {
            printf("[Audio][ERROR] Output failed to configure at %d Hz\n", fileSr);
            fflush(stdout);
            host_->showErrorMessage("Audio configure failed",
                "Audio output failed to configure.\nCheck Audio Settings.");
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
    // eqBuf holds EQ'd doubles; second half used as soxr output (in-place reuse).
    // kDecodeChunk is the decoder's typical output frame count per callback.
    const int kDecodeChunk = 4096;
    const size_t eqHalf  = (size_t)(kDecodeChunk + 256) * srcCh;
    auto eqBuf  = std::make_shared<std::vector<double>>(eqHalf * 2);

    size_t outBufSz = needsResample
        ? (size_t)ceil((double)(kDecodeChunk + 256) * capturedOutSr / capturedFileSr + 256) * srcCh
        : (size_t)(kDecodeChunk + 256) * srcCh;
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

    callbackI32 = [this, outPtr, eqBuf, outBuf, resamplerPtr,
                   capturedFileSr, capturedOutSr, capturedBits, srcCh, needsResample, eqHalf]
                  (const int32_t* d, int n) {
        if (d == nullptr || n == 0) return;
        int frames = srcCh > 0 ? n / srcCh : n;

        if (!needsResample) {
            // Fast path: rates match — EQ in double, single snap to int32.
            eqManager_.processInPlaceInt32(const_cast<int32_t*>(d), n);
            outPtr->writeInt32Blocking(d, n);
            playedFrames_.fetch_add(frames, std::memory_order_relaxed);
            return;
        }

        // Resample path: single quantization point.
        // 1. EQ int32 → double (no snap)
        eqManager_.processToDouble(d, eqBuf->data(), n);

        // 2. Resample double → double using second half of eqBuf as output
        size_t idone = 0, odone = 0;
        soxr_process(static_cast<soxr_t>(resamplerPtr.get()),
                     eqBuf->data(),        n / srcCh, &idone,
                     eqBuf->data() + eqHalf, eqHalf / srcCh, &odone);
        int resampN = (int)(odone * srcCh);

        // Grow outBuf if needed (soxr may produce more than estimated on first call)
        if (resampN > (int)outBuf->size())
            outBuf->resize(resampN);

        // 3. TPDF dither + quantize once to device's max bit depth
        ditherAndQuantize(eqBuf->data() + eqHalf, outBuf->data(), resampN, capturedBits);

        outPtr->writeInt32Blocking(outBuf->data(), resampN);
        playedFrames_.fetch_add(resampN / srcCh, std::memory_order_relaxed);
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
        host_->showErrorMessage("Audio start failed",
            "Audio output failed to start.\nCheck Audio Settings.");
        active_->stop();
        isPlaying_ = false;
        return;
    }
    printf("[onPlay] USB streaming started, ring=%zu\n", output_->ringAvailable());
    fflush(stdout);

    startGaplessCoordinator(callbackI32, capturedOutSr, capturedDacCh);
    host_->startTimer(TimerId::SeekUpdate, 250);
}

void PlayerWindow::onStop() {
    {
        std::lock_guard<std::mutex> lk(gaplessMu_);
        stopGapless_.store(true);
        gaplessSignal_ = false;
        gaplessCv_.notify_one();
    }
    if (gaplessThread_.joinable()) gaplessThread_.join();
    stopGapless_.store(false);

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

void PlayerWindow::prepareNextTrack() {
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
        // Advance to next album if we've exhausted this one
        if (track >= (int)albums_[album].tracks.size()) {
            album++;
            track = 0;
        }
        if (album >= (int)albums_.size() || albums_[album].tracks.empty()) {
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
                host_->postAppEvent(AppEvent::RequestPlay);
                break;
            }

            prepareNextTrack();
        }
    });
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
        printf("[Audio][ERROR] Audio device fault detected, stopping playback\n");
        fflush(stdout);
        onStop();
        host_->showErrorMessage("Audio device error",
            "Audio device error (driver fault).\n"
            "Playback stopped. Try unplugging/replugging the DAC.");
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
    invalidate();
}

void PlayerWindow::onArtClick() {
    if (displayAlbum_ < 0) return;
    artWin_.show(albums_[displayAlbum_].artPath);
}

// Flip the now-playing display (title/artist/art/total + track-row highlight) to
// the given track. Called when the DAC crosses a gapless boundary (from onTimer)
// and on the non-seamless restart. Does NOT touch playedFrames_ (monotonic).
void PlayerWindow::applyTrackMetadata(int album, int track) {
    if (album < 0 || album >= (int)albums_.size() ||
        track < 0 || track >= (int)albums_[album].tracks.size()) return;
    const auto& nt = albums_[album].tracks[track];
    displayAlbum_   = album;
    displayTrack_   = track;
    currentTitle_  = nt.title;
    currentArtist_ = nt.artist;
    seekTotalMs_    = nt.durationMs > 0 ? nt.durationMs : 0;
    seekPosMs_      = 0;
    loadTransportArtTexture(albums_[album].artPath);
    selectedAlbumIdx_ = album;
    loadTrackPanelArtTexture(album);
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

    // A rescan can introduce text in a script never seen before (e.g. the
    // first Chinese/Cyrillic album added after launch) — bakeFallbackGlyphs()
    // is cheap when there's nothing new (every codepoint already resolves),
    // so it's safe to just always re-check here.
    if (bakeFallbackGlyphs()) {
        if (!msdfCachePath_.empty()) msdfFont_.saveCache(msdfCachePath_.c_str());
        renderer_->initMsdf(msdfFont_);  // re-entrant: rebuilds the grown atlas (see Renderer::initMsdf())
    }
    // Whether or not anything new was baked, the CPU atlas pixels (hydrated
    // by bakeFallbackGlyphs above) are dead weight now — GPU + disk have it.
    msdfFont_.releaseAtlasPixels();

    rebuildGridIndices();  // albums_ changed — refresh the (possibly filtered) tile mapping
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
    if (codepoint == 0x08) {  // backspace: pop one UTF-8 codepoint
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
    if (keyCode == key::Escape) { closeActivePanel(); return true; }
    return true;  // swallow every other key while a panel is open — no
                  // play/stop or search-box interaction bleeding through to
                  // the main view behind it (mirrors the native dialogs'
                  // EnableWindow(false) modal behavior).
}

void PlayerWindow::onKeyDownPortable(int keyCode) {
    if (onPanelKeyDown(keyCode)) return;
    switch (keyCode) {
    case key::Space:
        if (searchFocused_) return;  // typing a space, not play/stop
        if (isPlaying_) onStop(); else if (currentAlbum_ >= 0) onPlay();
        return;
    case key::Escape:
        if (searchFocused_ || !searchQuery_.empty()) {
            // First Escape: leave the box and clear the filter.
            searchFocused_ = false;
            searchQuery_.clear();
            rebuildGridIndices();
            gridScrollY_ = 0;
            recalcLayout();
            invalidate();
        } else if (trackPanelOpen_) {
            trackPanelOpen_ = false;
            recalcLayout();
            invalidate();
        }
        return;
    }
}

void PlayerWindow::shutdown() {
    onStop();
    watcher_.unwatchAll();
    if (scanThread_.joinable()) scanThread_.join();
    // Join the art-decode worker before tearing down textures/renderer — it
    // never touches the Renderer, but a decode completing after this point
    // would notify a dying window.
    stopArtDecodeThread();
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
    renderer_.reset();
}

#include "player_view.hh"
#include "log_util.h"
#include "art_texture.hh"
#include "img_decode.hh"
#include "text_util.hh"
#include "utf8.hh"
#include <windowsx.h>
#include <commdlg.h>
#include <shlobj.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <algorithm>
#include <stdexcept>
#include <filesystem>
#include <fstream>
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")

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

static std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    return w;
}

// Canvas::text() wants UTF-8; currentTitleW_/currentArtistW_ are wide (set
// from metadata elsewhere) — convert back for drawFrame().
static std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

// ── GDI -> Canvas bridges (Phase 6) ──────────────────────────────────────────
// recalcLayout() keeps computing the same int RECTs it always did; these just
// let drawFrame() consume them (and the existing COLORREF palette) without
// re-deriving layout math or a parallel color table.
static Rect toRect(const RECT& r) {
    return { (float)r.left, (float)r.top, (float)(r.right - r.left), (float)(r.bottom - r.top) };
}
static Color toColor(COLORREF c, float a = 1.0f) {
    return { GetRValue(c) / 255.0f, GetGValue(c) / 255.0f, GetBValue(c) / 255.0f, a };
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

static const wchar_t* MAIN_CLASS = L"MatrixPlayerMain";

// Borderless: no title bar/icon/min-max-close buttons at all. Both UI modes
// are fixed sizes the app sets itself (see toggleUiMode()), so there's no
// resize/maximize to offer anyway; positioning is via the Alt+F/J/C/U edge-
// snap hotkeys (see snapToEdge()) instead of title-bar dragging. Taskbar/
// Alt-Tab presence (and the app icon there) still comes from
// WS_EX_APPWINDOW — only the on-window chrome is gone. Alt+F4 and the
// taskbar icon's right-click "Close window"/single-click-to-minimize still
// work; neither is tied to WS_CAPTION/WS_SYSMENU.
static constexpr DWORD kFixedWindowStyle = WS_POPUP;
static constexpr DWORD kFixedWindowExStyle = WS_EX_APPWINDOW;

enum {
    kHotkeySnapLeft = 1,
    kHotkeySnapRight,
    kHotkeySnapBottom,
    kHotkeySnapTop,
    kHotkeySnapCenterG,  // Alt+G and Alt+H both center — two ids, same action
    kHotkeySnapCenterH,
    kHotkeyToggleMode,   // Alt+L: Essential <-> Complete (replaces the old corner button)
};

// ── Window creation ──────────────────────────────────────────────────────────

bool PlayerWindow::create(HINSTANCE hInst) {
    hInst_ = hInst;

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_DBLCLKS;
    wc.lpfnWndProc   = wndProc;
    wc.hInstance     = hInst;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = MAIN_CLASS;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon         = LoadIconW(hInst, L"IDI_APPICON");
    wc.hIconSm       = LoadIconW(hInst, L"IDI_APPICON");
    RegisterClassExW(&wc);

    // Fixed, non-resizable window (no WS_THICKFRAME/WS_MAXIMIZEBOX) — both UI
    // modes are fixed sizes the app itself sets via SetWindowPos on toggle
    // (toggleUiMode()), never left to interactive resize/maximize.
    //
    // Starting mode: Complete (true fullscreen, sized to the monitor's own
    // resolution — see computeCompleteWindowRect()) if the monitor is tall
    // enough to clear kMinWindowContentH, the font's geometric legibility
    // floor (player_window.h); otherwise fall back to Essential, which always
    // fits by construction (its size is *derived* from the monitor's own
    // dimensions — see computeEssentialWindowRect()).
    HMONITOR primaryMon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO primaryMi = { sizeof(primaryMi) };
    GetMonitorInfoW(primaryMon, &primaryMi);
    int monitorH = primaryMi.rcMonitor.bottom - primaryMi.rcMonitor.top;

    RECT startRect;
    if (monitorH >= (int)std::ceil(kMinWindowContentH)) {
        uiMode_ = UiMode::Complete;
        startRect = computeCompleteWindowRect(primaryMon);
    } else {
        uiMode_ = UiMode::Essential;
        startRect = computeEssentialWindowRect(primaryMon);
    }

    hwnd_ = CreateWindowExW(kFixedWindowExStyle, MAIN_CLASS, L"Matrix Player",
        kFixedWindowStyle, startRect.left, startRect.top,
        startRect.right - startRect.left, startRect.bottom - startRect.top,
        nullptr, nullptr, hInst, this);
    if (!hwnd_) return false;
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, (LONG_PTR)this);
    lastMonitor_ = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);

    // Edge-snap hotkeys — the window-move mechanism now that there's no
    // title bar to drag: Alt+F/J snap to the left/right edge (horizontal
    // monitor use), Alt+C/U snap to the bottom/top edge (vertical monitor
    // use), Alt+G/H re-center. All work regardless of current monitor
    // orientation (snapToEdge() just aligns to that edge/center) —
    // MOD_NOREPEAT so holding the keys doesn't spam WM_HOTKEY.
    RegisterHotKey(hwnd_, kHotkeySnapLeft,    MOD_ALT | MOD_NOREPEAT, 'F');
    RegisterHotKey(hwnd_, kHotkeySnapRight,   MOD_ALT | MOD_NOREPEAT, 'J');
    RegisterHotKey(hwnd_, kHotkeySnapBottom,  MOD_ALT | MOD_NOREPEAT, 'C');
    RegisterHotKey(hwnd_, kHotkeySnapTop,     MOD_ALT | MOD_NOREPEAT, 'U');
    RegisterHotKey(hwnd_, kHotkeySnapCenterG, MOD_ALT | MOD_NOREPEAT, 'G');
    RegisterHotKey(hwnd_, kHotkeySnapCenterH, MOD_ALT | MOD_NOREPEAT, 'H');
    // Alt+L toggles Essential/Complete — keyboard only; the on-screen
    // corner-bracket button was removed (visual clutter, per design).
    RegisterHotKey(hwnd_, kHotkeyToggleMode,  MOD_ALT | MOD_NOREPEAT, 'L');

    // Vulkan rendering (vk_canvas). Must come after hwnd_ exists (the surface
    // provider wraps it) and before ShowWindow, so the first frame presents
    // as soon as the window is visible.
    vkSurface_ = std::make_unique<Win32SurfaceProvider>(hwnd_);
    try {
        // 3 swapchain images: enough for MAILBOX on desktop (the default 4
        // is an Android compositor-hitch allowance) — saves one full-screen
        // RGBA8 image (~8 MB at 1080p).
        renderer_ = std::make_unique<Renderer>(*vkSurface_, vkAssets_,
                                               /*desiredSwapchainImages=*/3);
    } catch (const std::exception& e) {
        MessageBoxA(hwnd_, e.what(), "Vulkan initialization failed", MB_ICONERROR);
        return false;
    }

    // (UI icons are drawn as native vector shapes each frame — see
    // drawUiIcon(). No SVG rasterization or icon textures at startup.)

    // Open DB and restore the library BEFORE font setup below — baking
    // script-fallback glyphs (Cyrillic/Greek/CJK/…) needs to scan the
    // library's actual track/album/artist text for which non-Latin
    // codepoints it must cover.
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring dbPathW = exePath;
    dbPathW = dbPathW.substr(0, dbPathW.rfind(L'\\') + 1) + L"matrix_player.db";
    int dbLen = WideCharToMultiByte(CP_UTF8, 0, dbPathW.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string dbPath(dbLen, '\0');
    WideCharToMultiByte(CP_UTF8, 0, dbPathW.c_str(), -1, dbPath.data(), dbLen, nullptr, nullptr);
    if (!dbPath.empty() && dbPath.back() == '\0') dbPath.pop_back();
    db_.open(dbPath);

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
        fontsDir_ = toUtf8Path(L"fonts\\");

        // Generate MSDF atlas from the same OTF (cached to disk for fast reload).
        std::string cachePath = toUtf8Path(L"fonts\\lmroman10-regular.msdf.cache");
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
                addedStyle |= msdfFont_.addStyle(loader, toUtf8Path(L"fonts\\lm\\lmroman10-bold.otf").c_str(), FontStyle::Bold);
            if (!msdfFont_.hasStyle(FontStyle::Italic))
                addedStyle |= msdfFont_.addStyle(loader, toUtf8Path(L"fonts\\lm\\lmroman10-italic.otf").c_str(), FontStyle::Italic);
            if (!msdfFont_.hasStyle(FontStyle::Math))
                addedStyle |= msdfFont_.addStyle(loader, toUtf8Path(L"fonts\\lm\\lmmono10-regular.otf").c_str(), FontStyle::Math);

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

    artWin_.create(hInst);
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
    {
        std::wstring eqPathW = exePath;
        eqPathW = eqPathW.substr(0, eqPathW.rfind(L'\\') + 1) + L"eq_profiles.json";
        int eqLen = WideCharToMultiByte(CP_UTF8, 0, eqPathW.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string eqPath(eqLen, '\0');
        WideCharToMultiByte(CP_UTF8, 0, eqPathW.c_str(), -1, eqPath.data(), eqLen, nullptr, nullptr);
        if (!eqPath.empty() && eqPath.back() == '\0') eqPath.pop_back();
        eqProfiles_.load(eqPath);
    }

    setupWatchers();
    startBackgroundScan();

    // Load audio mode
    bitperfectMode_.store(db_.loadSetting("audio_mode") == "bitperfect");

    // Load audio backend
    useWasapi_ = (db_.loadSetting("audio_backend") == "wasapi");
    wasapiMode_ = (db_.loadSetting("wasapi_mode") == "exclusive")
                  ? WasapiMode::Exclusive : WasapiMode::Shared;
    auto devIdUtf8 = db_.loadSetting("wasapi_device_id");
    wasapiDeviceId_ = utf8ToWide(devIdUtf8);

    if (useWasapi_) {
        output_ = std::make_unique<WasapiOutput>(wasapiDeviceId_, wasapiMode_);
        printf("[Audio] WASAPI backend selected (%s mode)\n",
               wasapiMode_ == WasapiMode::Exclusive ? "exclusive" : "shared");
    } else {
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
            wchar_t msgBuf[512];
            swprintf_s(msgBuf, sizeof(msgBuf)/sizeof(wchar_t),
                L"USB DAC not found (VID=%04X PID=%04X).\n\n"
                L"Steps to fix:\n"
                L"1. Open Zadig\n"
                L"2. Select your USB DAC interface MI_00\n"
                L"3. Install libusbK driver\n"
                L"4. Restart this app\n\n"
                L"Use Audio Settings to select a different device\n"
                L"or switch to WASAPI.", vid, pid);
            MessageBoxW(hwnd_, msgBuf, L"USB DAC not found", MB_OK | MB_ICONWARNING);
        }
        output_ = std::make_unique<UsbAudioOutput>(usbDriver_);
    }

    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
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
        std::string newcmPath = fontsDir_ + "newcomputermodern\\NewCM10-Regular.otf";
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
        std::string newcmPath = fontsDir_ + "newcomputermodern\\NewCM10-Regular.otf";
        if (msdfFont_.bakeCodepoints(loader, newcmPath.c_str(), exotic) > 0)
            anyNew = true;

        // Dedicated CJK/Hangul serif faces bundled alongside Latin Modern —
        // Song/Mincho/Batang are all serif designs, the closest visual match
        // to Latin Modern's serif Latin text (vs. a sans-serif system font).
        const std::string kCjkFallbacks[] = {
            fontsDir_ + "fandol\\FandolSong-Regular.otf",         // Simplified Chinese
            fontsDir_ + "haranoaji\\HaranoAjiMincho-Regular.otf", // Japanese
            fontsDir_ + "unfonts-core\\UnBatang.ttf",             // Korean
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
    // Guard against calls that land before renderer_ exists: WM_SIZE fires
    // synchronously during CreateWindowExW (before create() constructs
    // renderer_ a few lines later), and that WM_SIZE handler calls invalidate().
    pendingFrames_ = renderer_ ? renderer_->swapchainImageCount() + 1 : 1;
}

void PlayerWindow::invalidate(const RECT* rc) {
    InvalidateRect(hwnd_, rc, FALSE);
    markDirty();
}

void PlayerWindow::run() {
    // Dirty-flag render-on-demand: only draw while pendingFrames_ (armed by
    // markDirty()/invalidate()) is nonzero; otherwise block in
    // MsgWaitForMultipleObjects instead of busy-spinning, so the app drops to
    // ~0% CPU whenever nothing on screen actually needs to change. WM_TIMER
    // (playback position) and WM_APP_* (async completions) wake the wait
    // normally and mark dirty from their own handlers.
    MSG msg;
    while (running_) {
        bool haveWork = pendingFrames_ > 0 || artWin_.hasPendingFrames();
        MsgWaitForMultipleObjects(0, nullptr, FALSE, haveWork ? 0 : INFINITE, QS_ALLINPUT);

        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running_ = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running_) break;

        if (pendingFrames_ > 0) { drawFrame(); pendingFrames_--; }
        // ArtWindow is a second HWND on this same thread with its own
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
enum class UiIcon { Play, Stop, Prev, Next };

static void drawUiIcon(Canvas& c, const RECT& rc, UiIcon icon, Color col) {
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
    }
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
                                 COLORREF baseColor, FontStyle baseStyle) {
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
        std::string titleStr = currentTitleW_.empty() ? "No track" : wideToUtf8(currentTitleW_);
        float titleW = canvas.textWidthStyled(titleStr, textSizes_.transportTitle, FontStyle::Bold);
        canvas.textStyled(titleStr, titleR.x + std::max(0.0f, (titleR.w - titleW) * 0.5f), titleR.y,
                          textSizes_.transportTitle, toColor(CLR_TEXT_PRIMARY), FontStyle::Bold);

        // Single combined Play/Stop button (per design: not a separate
        // resume-vs-restart-from-zero distinction in Essential mode).
        struct EBtn { RECT rc; int idx; UiIcon icon; COLORREF clr; };
        EBtn ebuttons[] = {
            { rcEssentialPrev_,     0, UiIcon::Prev, CLR_TEXT_PRIMARY },
            { rcEssentialPlayStop_, 1, isPlaying_ ? UiIcon::Stop : UiIcon::Play,
                                       isPlaying_ ? CLR_TEXT_PRIMARY : CLR_ACCENT },
            { rcEssentialNext_,     2, UiIcon::Next, CLR_TEXT_PRIMARY },
        };
        for (auto& b : ebuttons) {
            if (hoverEssentialBtn_ == b.idx) {
                Rect r = toRect(b.rc);
                canvas.rect(r.x, r.y, r.w, r.h, toColor(CLR_HOVER), 8.0f);
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
                                          textSizes_.nav, FontStyle::Bold),
                          16, rcBrand_.bottom * 0.5f - textSizes_.nav * 0.5f,
                          textSizes_.nav, toColor(CLR_ACCENT), FontStyle::Bold);

        // Search box — filters the album grid live as the user types.
        {
            Rect s = toRect(rcSearch_);
            canvas.rect(s.x, s.y, s.w, s.h, toColor(RGB(24, 24, 24)), 6.0f);
            canvas.rect(s.x, s.y + s.h - 1, s.w, 1,
                        toColor(searchFocused_ ? CLR_ACCENT : CLR_SEPARATOR));
            std::string shown = searchQuery_.empty() && !searchFocused_
                                ? "Search" : searchQuery_;
            std::string caret = searchFocused_ ? "|" : "";
            COLORREF clr = searchQuery_.empty() && !searchFocused_
                           ? CLR_TEXT_DIM : CLR_TEXT_PRIMARY;
            std::string fit = truncateToWidth(canvas, shown, s.w - 16 - 8,
                                              textSizes_.secondary, FontStyle::Roman);
            canvas.text(fit + caret, s.x + 8, s.y + s.h * 0.5f - textSizes_.secondary * 0.5f,
                        textSizes_.secondary, toColor(clr));
        }

        struct NavItem { const char* label; RECT rc; int idx; };
        NavItem items[] = {
            { "Albums",   rcNavAlbums_,   0 },
            { "Settings", rcNavSettings_, 1 },
        };
        for (auto& item : items) {
            bool active = (activeNavItem_ == item.idx);
            bool hovered = (hoverSidebarItem_ == item.idx && !active);
            Rect r = toRect(item.rc);
            if (hovered) canvas.rect(r.x + 4, r.y, r.w - 8, r.h, toColor(CLR_HOVER), 6.0f);
            if (active) canvas.rect(r.x, r.y + 6, 2, r.h - 12, toColor(CLR_ACCENT));
            canvas.text(item.label, r.x + 20, r.y + r.h * 0.5f - textSizes_.nav * 0.5f,
                       textSizes_.nav, toColor(active ? CLR_TEXT_PRIMARY : CLR_TEXT_SECONDARY));
        }

        // (The now-playing mini card that used to fill the space below the
        // nav items was removed: it duplicated the transport bar's art,
        // title, and artist — the transport bar is the single now-playing
        // readout now, including the format line next to the BITPERFECT badge.)
    }

    // ── Main content: album grid, settings page, or (below) the full-page
    // album view that replaces the grid while an album is focused ─────────
    if (activeNavItem_ == 0 && !trackPanelOpen_) {
        Rect g = toRect(rcGrid_);
        canvas.rect(g.x, g.y, g.w, g.h, toColor(CLR_BG_MAIN));

        if (albums_.empty()) {
            canvas.text("No albums yet. Go to Settings to add a music folder.",
                       g.x + g.w * 0.5f - 160, g.y + 100, textSizes_.nav, toColor(CLR_TEXT_DIM));
        } else if (gridIndices_.empty()) {
            canvas.text("No matches for \"" + searchQuery_ + "\"",
                       g.x + g.w * 0.5f - 120, g.y + 100, textSizes_.nav, toColor(CLR_TEXT_DIM));
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

                    // Hover: soft accent glow instead of the old grey slab —
                    // two stacked low-alpha rounded rects read as a halo once
                    // the art covers their centers.
                    if (hoverAlbumIdx_ == idx && !nowPlaying) {
                        canvas.rect(x - 8, y - 8, a + 16, a + 16, toColor(CLR_ACCENT, 0.10f), 10.0f);
                        canvas.rect(x - 4, y - 4, a + 8,  a + 8,  toColor(CLR_ACCENT, 0.22f), 8.0f);
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

                    drawArtOrPlaceholder(canvas, getGridArtTexture(idx), x, y, a, a);

                    // Last-played / now-playing marker: a thin accent bar
                    // hugging the art's bottom edge, exactly the art's width
                    // (the art itself can't carry a badge: imageFg composites
                    // above the vector layer). Replaces the old offset dot,
                    // which broke the grid's column alignment.
                    bool lastPlayed = !nowPlaying &&
                        alb.name == lastPlayedAlbumName_ &&
                        alb.artist == lastPlayedArtistName_;
                    if (nowPlaying || lastPlayed)
                        canvas.rect(x, y + a + 2, a, 2,
                                   toColor(CLR_ACCENT, nowPlaying ? 1.0f : 0.4f));

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
                                        COLORREF clr, FontStyle st) {
                        float w = canvas.textWidthStyled(s, sz, st);
                        canvas.textStyled(s, x + std::max(0.0f, (a - w) * 0.5f), yy,
                                          sz, toColor(clr), st);
                    };
                    float adv = titleArtistAdvance(textSizes_.body);
                    float ty = y + a + 10.0f * uiScale_;
                    std::string base, mod;
                    if (splitNameModifier(alb.displayName, base, mod)) {
                        centered(truncateToWidth(canvas, base, textMaxW, textSizes_.body, FontStyle::Bold),
                                 ty, textSizes_.body, CLR_TEXT_ALBUM_TITLE, FontStyle::Bold);
                        centered(truncateToWidth(canvas, mod, textMaxW, textSizes_.secondary, FontStyle::Italic),
                                 ty + adv, textSizes_.secondary, CLR_TEXT_DIM, FontStyle::Italic);
                    } else {
                        std::string l1, l2;
                        splitTwoLines(canvas, alb.displayName, textMaxW, textSizes_.body, FontStyle::Bold, l1, l2);
                        centered(l1, ty, textSizes_.body, CLR_TEXT_ALBUM_TITLE, FontStyle::Bold);
                        if (!l2.empty())
                            centered(l2, ty + adv, textSizes_.body, CLR_TEXT_ALBUM_TITLE, FontStyle::Bold);
                    }
                    // Artist sits in a fixed slot (below 2 title lines) so it
                    // aligns across tiles whether titles wrapped or not.
                    centered(truncateToWidth(canvas, alb.artist, textMaxW, textSizes_.secondary, FontStyle::Italic),
                             ty + adv * 2, textSizes_.secondary, CLR_TEXT_SECONDARY, FontStyle::Italic);
                }
            }
            canvas.clearClip();
        }
    } else if (activeNavItem_ != 0) {
        Rect g = toRect(rcGrid_);
        canvas.rect(g.x, g.y, g.w, g.h, toColor(CLR_BG_MAIN));

        // Centering is done by measuring the styled text ourselves —
        // Canvas::textCentered() measures with the curve font while the UI
        // renders MSDF, and its baseline convention differs, so labels came
        // out visibly off-center both ways.
        auto centeredIn = [&](const std::string& s, const Rect& r, float sz,
                              COLORREF clr, FontStyle st) {
            float w = canvas.textWidthStyled(s, sz, st);
            canvas.textStyled(s, r.x + std::max(0.0f, (r.w - w) * 0.5f),
                              r.y + r.h * 0.5f - sz * 0.5f, sz, toColor(clr), st);
        };
        {
            Rect hdr = { g.x, g.y + 24, g.w, textSizes_.header };
            centeredIn("Settings", hdr, textSizes_.header, CLR_TEXT_PRIMARY, FontStyle::Bold);
        }

        bool bp = bitperfectMode_.load();
        std::string modeLabel = bp
            ? "Mode: Bitperfect - click to switch to Reference EQ"
            : "Mode: Reference EQ - click to switch to Bitperfect";

        struct SettItem { RECT rc; std::string label; int idx; };
        SettItem items[] = {
            { rcSettingsAddFolder_, "Add Music Folder",      0 },
            { rcSettingsManage_,    "Manage Music Folders",  1 },
            { rcSettingsAudio_,     "Audio Output Settings", 2 },
            { rcSettingsEq_,        "EQ / AutoEQ Profiles",  3 },
            { rcSettingsBitperfect_, modeLabel,              4 },
        };
        for (auto& item : items) {
            Rect r = toRect(item.rc);
            if (hoverSettingsItem_ == item.idx)
                canvas.rect(r.x, r.y, r.w, r.h, toColor(CLR_HOVER), 8.0f);
            bool isActiveModeRow = (item.idx == 4 && bp);
            COLORREF borderClr = isActiveModeRow ? CLR_ACCENT : CLR_SEPARATOR;
            float borderThick = isActiveModeRow ? 2.0f : 1.0f;
            canvas.rect(r.x, r.y, r.w, borderThick, toColor(borderClr));
            canvas.rect(r.x, r.y + r.h - borderThick, r.w, borderThick, toColor(borderClr));
            COLORREF textClr = (item.idx == 3 && bp) ? CLR_TEXT_DIM : CLR_TEXT_PRIMARY;
            if (isActiveModeRow) textClr = CLR_ACCENT;
            centeredIn(item.label, r, textSizes_.nav, textClr, FontStyle::Roman);
        }
    }

    // ── Album view (full page — replaces the grid while open) ───────────
    // Clicking an album focuses it: the rest of the library disappears and
    // the whole content area belongs to this one album — big art on the
    // left, track list on the right, album description and artist bio (from
    // the sidecar files next to the music) below. The page scrolls as one.
    if (activeNavItem_ == 0 && trackPanelOpen_) {
        Rect tp = toRect(rcTrackPanel_);
        canvas.rect(tp.x, tp.y, tp.w, tp.h, toColor(CLR_BG_TRACKPANEL));

        if (selectedAlbumIdx_ >= 0 && selectedAlbumIdx_ < (int)albums_.size()) {
            const Album& album = albums_[selectedAlbumIdx_];
            canvas.setClip(tp.x, tp.y, tp.w, tp.h);

            float pad = 40.0f * uiScale_;
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
                wrapText(canvas, base, colW, textSizes_.trackPanelTitle, FontStyle::Bold, titleLines);
                for (auto& ln : titleLines) {
                    canvas.textStyled(ln, colX, y, textSizes_.trackPanelTitle,
                                      toColor(CLR_TEXT_PRIMARY), FontStyle::Bold);
                    y += titleArtistAdvance(textSizes_.trackPanelTitle);
                }
            }
            if (!mod.empty()) {
                std::vector<std::string> modLines;
                wrapText(canvas, mod, colW, textSizes_.secondary, FontStyle::Italic, modLines);
                for (auto& ln : modLines) {
                    canvas.textStyled(ln, colX, y, textSizes_.secondary,
                                      toColor(CLR_TEXT_DIM), FontStyle::Italic);
                    y += textSizes_.secondary * 1.35f;
                }
            }
            if (!album.artist.empty()) {
                canvas.textStyled(truncateToWidth(canvas, album.artist, colW, textSizes_.secondary, FontStyle::Italic),
                                  colX, y, textSizes_.secondary, toColor(CLR_TEXT_SECONDARY), FontStyle::Italic);
                y += textSizes_.secondary * 1.35f;
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
                canvas.textStyled(badge, colX, y, textSizes_.badge,
                                  toColor(CLR_TEXT_DIM), FontStyle::Math);
                y += textSizes_.badge * 1.8f;
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
            float durColW = canvas.textWidthStyled("88:88", textSizes_.secondary, FontStyle::Math);
            for (int i = 0; i < (int)album.tracks.size(); i++) {
                float rowY = y + i * trackRowHeight_;
                if (rowY + trackRowHeight_ < tp.y) continue;
                if (rowY > tp.y + tp.h) break;

                bool isPlayingRow = (displayAlbum_ == selectedAlbumIdx_ && displayTrack_ == i && isPlaying_);
                if (hoverTrackIdx_ == i)
                    canvas.rect(colX - 12, rowY, colW + 24, (float)trackRowHeight_, toColor(CLR_HOVER), 6.0f);

                // Track number / duration are numeric readouts: Mono (repurposed
                // Math style slot) keeps digits from jittering column-to-column.
                int trackNum = album.tracks[i].trackNumber > 0 ? album.tracks[i].trackNumber : i + 1;
                std::string trackNumStr = std::to_string(trackNum);
                // Baselines centered by the actual text size (the old "-6"
                // magic offset drifted across resolutions), columns scaled.
                float numColW = 30.0f * uiScale_, titleX = 46.0f * uiScale_;
                float trackNumW = canvas.textWidthStyled(trackNumStr, textSizes_.body, FontStyle::Math);
                canvas.textStyled(trackNumStr, colX + numColW - trackNumW,
                                rowY + trackRowHeight_ * 0.5f - textSizes_.body * 0.5f,
                                textSizes_.body, toColor(isPlayingRow ? CLR_ACCENT : CLR_TEXT_SECONDARY), FontStyle::Math);
                // Base-name priority: only the trailing "(from the Netflix
                // Series...)" modifier ever gets truncated, never the name.
                float titleMaxW = colW - titleX - durColW - 16.0f * uiScale_;
                FontStyle rowStyle = isPlayingRow ? FontStyle::Bold : FontStyle::Roman;
                drawNameWithModifier(canvas, album.tracks[i].title,
                                     colX + titleX,
                                     rowY + trackRowHeight_ * 0.5f - textSizes_.nav * 0.5f,
                                     titleMaxW, textSizes_.nav,
                                     isPlayingRow ? CLR_ACCENT : CLR_TEXT_PRIMARY, rowStyle);

                int durMs = album.tracks[i].durationMs;
                if (durMs > 0) {
                    char durBuf[16];
                    snprintf(durBuf, sizeof(durBuf), "%d:%02d", durMs / 60000, (durMs % 60000) / 1000);
                    float durW = canvas.textWidthStyled(durBuf, textSizes_.secondary, FontStyle::Math);
                    canvas.textStyled(durBuf, colX + colW - durW,
                                    rowY + trackRowHeight_ * 0.5f - textSizes_.secondary * 0.5f,
                                    textSizes_.secondary, toColor(CLR_TEXT_SECONDARY), FontStyle::Math);
                }
            }
            float tracksBottom = y + (float)album.tracks.size() * trackRowHeight_;

            // ── Sidecar text sections (album description, artist bio) ──
            float sectY = std::max(tracksBottom, artY + artSize) + 36.0f;
            float textW = tp.w - pad * 2.0f;
            if (albumTextWrapW_ != textW) {
                albumDescLines_.clear();
                artistBioLines_.clear();
                if (!albumDescText_.empty())
                    wrapText(canvas, albumDescText_, textW, textSizes_.secondary, FontStyle::Roman, albumDescLines_);
                if (!artistBioText_.empty())
                    wrapText(canvas, artistBioText_, textW, textSizes_.secondary, FontStyle::Roman, artistBioLines_);
                albumTextWrapW_ = textW;
            }
            float lineAdv = textSizes_.secondary * 1.5f;
            auto drawSection = [&](const std::string& caption,
                                   const std::vector<std::string>& lines, float& yy) {
                if (lines.empty()) return;
                canvas.textStyled(caption, tp.x + pad, yy, textSizes_.badge,
                                  toColor(CLR_TEXT_DIM), FontStyle::Bold);
                yy += textSizes_.badge * 2.2f;
                for (auto& ln : lines) {
                    if (ln.empty()) { yy += lineAdv * 0.6f; continue; }
                    // Height accounting always runs; drawing is culled to
                    // the visible band.
                    if (yy + lineAdv >= tp.y && yy <= tp.y + tp.h)
                        canvas.textStyled(ln, tp.x + pad, yy, textSizes_.secondary,
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
                             currentTitleW_.empty() ? "No track" : wideToUtf8(currentTitleW_),
                             infoR.x, infoR.y, infoR.w,
                             textSizes_.transportTitle, CLR_TEXT_PRIMARY, FontStyle::Bold);
        std::string artist = wideToUtf8(currentArtistW_);
        if (!artist.empty()) {
            std::string a = truncateToWidth(canvas, artist, infoR.w,
                                            textSizes_.secondary, FontStyle::Italic);
            canvas.textStyled(a, infoR.x, infoR.y + titleArtistAdvance(textSizes_.transportTitle),
                              textSizes_.secondary, toColor(CLR_TEXT_SECONDARY), FontStyle::Italic);
        }

        // Three buttons only — prev / play-stop / next, same combined
        // play-stop toggle as Essential mode. No pause: this user only ever
        // stops or starts from zero.
        struct BtnDef { RECT rc; int idx; UiIcon icon; COLORREF clr; };
        BtnDef buttons[] = {
            { rcBtnPrev_, 0, UiIcon::Prev, CLR_TEXT_PRIMARY },
            { rcBtnPlay_, 1, isPlaying_ ? UiIcon::Stop : UiIcon::Play,
                             isPlaying_ ? CLR_TEXT_PRIMARY : CLR_ACCENT },
            { rcBtnNext_, 2, UiIcon::Next, CLR_TEXT_PRIMARY },
        };
        for (auto& b : buttons) {
            if (hoverTransportBtn_ == b.idx) {
                Rect r = toRect(b.rc);
                canvas.rect(r.x, r.y, r.w, r.h, toColor(CLR_HOVER), 8.0f);
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
            COLORREF dspClr = bp ? CLR_ACCENT : CLR_TEXT_DIM;
            float rightEdge = t.x + t.w - 16;
            float cy = t.y + t.h * 0.5f;
            float tagW = canvas.textWidthStyled(dsp, textSizes_.badge, FontStyle::Math);

            // Hover hit rect always tracks the compact tag's home (with a
            // little slop), so the hover state stays stable while the
            // expanded readout is showing.
            rcDspBadge_ = { (LONG)(rightEdge - tagW - 8), (LONG)(cy - textSizes_.badge),
                            (LONG)(rightEdge + 8),        (LONG)(cy + textSizes_.badge) };

            if (hoverDspBadge_) {
                std::string src;
                if (displayAlbum_ >= 0 && displayAlbum_ < (int)albums_.size() &&
                    displayTrack_ >= 0 && displayTrack_ < (int)albums_[displayAlbum_].tracks.size()) {
                    const Track& dt = albums_[displayAlbum_].tracks[displayTrack_];
                    src = formatQualityText(dt.sampleRate, dt.bitDepth);
                }
                struct Seg { std::string text; COLORREF clr; };
                std::vector<Seg> segs;
                if (!src.empty()) {
                    segs.push_back({src, CLR_TEXT_DIM});
                    segs.push_back({" \xC2\xBB ", CLR_TEXT_DIM});
                }
                segs.push_back({dsp, dspClr});
                segs.push_back({" \xC2\xBB ", CLR_TEXT_DIM});
                segs.push_back({useWasapi_ ? "WASAPI" : "USB", CLR_TEXT_DIM});

                float total = 0;
                for (auto& s : segs) total += canvas.textWidthStyled(s.text, textSizes_.badge, FontStyle::Math);
                float sx = rightEdge - total;
                float sy = cy - textSizes_.badge * 0.5f;
                for (auto& s : segs) {
                    canvas.textStyled(s.text, sx, sy, textSizes_.badge, toColor(s.clr), FontStyle::Math);
                    sx += canvas.textWidthStyled(s.text, textSizes_.badge, FontStyle::Math);
                }
            } else {
                canvas.textStyled(dsp, rightEdge - tagW, cy - textSizes_.badge * 0.5f,
                                  textSizes_.badge, toColor(dspClr), FontStyle::Math);

                char timeBuf[64];
                snprintf(timeBuf, sizeof(timeBuf), "%d:%02d / %d:%02d",
                        seekPosMs_ / 60000, (seekPosMs_ % 60000) / 1000,
                        seekTotalMs_ / 60000, (seekTotalMs_ % 60000) / 1000);
                float timeW = canvas.textWidthStyled(timeBuf, textSizes_.secondary, FontStyle::Math);
                canvas.textStyled(timeBuf, rightEdge - tagW - 24 - timeW,
                                  cy - textSizes_.secondary * 0.5f,
                                  textSizes_.secondary, toColor(CLR_TEXT_SECONDARY), FontStyle::Math);
            }
        }
    }

    // (No on-screen mode toggle — Alt+L switches Essential/Complete.)

    renderer_->draw(frameCurves_, /*overlay_rotation_deg=*/0, frameImages_, frameImagesFg_, msdfQuads_, frameShapes_);
}

// ── Layout ───────────────────────────────────────────────────────────────────

void PlayerWindow::recalcLayout() {
    RECT rc; GetClientRect(hwnd_, &rc);
    int W = rc.right, H = rc.bottom;

    // Window-relative text sizes, floored at the geometric minimum (see
    // player_window.h) — a defensive backstop in case anything ever resizes
    // the window without going through toggleUiMode() (which is the only
    // place either mode's size is ever set; there's no interactive resize).
    ResponsiveTextScale scale{kMinReadableTextSizePx};
    textSizes_.badge           = scale.sizeFor(kTextSizeBadgePct, (float)H);
    textSizes_.secondary       = scale.sizeFor(kTextSizeSecondaryPct, (float)H);
    textSizes_.body            = scale.sizeFor(kTextSizeBodyPct, (float)H);
    textSizes_.nav             = scale.sizeFor(kTextSizeNavPct, (float)H);
    textSizes_.transportTitle  = scale.sizeFor(kTextSizeTransportTitlePct, (float)H);
    textSizes_.trackPanelTitle = scale.sizeFor(kTextSizeTrackPanelTitlePct, (float)H);
    textSizes_.header          = scale.sizeFor(kTextSizeHeaderPct, (float)H);

    // The one proportion factor for all remaining fixed-pixel layout values
    // (see uiScale_'s comment in player_window.h): 13.0f is the nav role's
    // calibration size at the reference window height.
    uiScale_ = textSizes_.nav / 13.0f;
    const float us = uiScale_;

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

    int transportH = (int)(80 * us);  // scales with the text it contains

    // Sidebar width scales the same way — fixed pixel widths with
    // height-scaled text is how "MATRIX PLAYER" ended up painted over the
    // first grid column's art at 1080p: the sidebar stayed 170px while its
    // text grew ~1.6x.
    int sidebarW = std::max(170, (int)(170.0f * us));

    rcTransport_ = { 0, H - transportH, W, H };
    rcSidebar_   = { 0, 0, sidebarW, H - transportH };

    // The album view is a full page, not a side panel: opening an album
    // replaces the grid entirely (the grid draw is skipped while it's open).
    // rcGrid_ stays the full content area in both states — the settings
    // page and the grid share it.
    rcGrid_ = { sidebarW, 0, W, H - transportH };
    rcTrackPanel_ = trackPanelOpen_ ? rcGrid_ : RECT{ 0, 0, 0, 0 };

    // Grid columns — derived from a target tile pitch (~220px art + margins)
    // rather than a fixed column count, so density stays consistent across
    // window widths and panel open/close: more of the library visible at a
    // glance, tiles still large enough to enjoy the art.
    static constexpr int kTargetTilePitch = 250;  // desired cell stride incl. margins
    static constexpr int kMinGridArtSize  = 80;   // legible floor; drop a column instead of going below it
    static constexpr int kGridArtMargin   = 30;   // gap reserved around the art within its cell

    int gridW = rcGrid_.right - rcGrid_.left - gridPadX_ * 2;
    int desiredCols = std::clamp(gridW / kTargetTilePitch, 2, 8);
    while (desiredCols > 1 && (gridW / desiredCols) - kGridArtMargin < kMinGridArtSize) desiredCols--;
    gridCols_ = std::max(1, desiredCols);

    int newGridArtSize = std::max(kMinGridArtSize, gridW / gridCols_ - kGridArtMargin);
    if (newGridArtSize != gridArtSize_) {
        // Tile size changed (resize, monitor change, panel open/close) — cached
        // art was decoded for the old size, so it must reload at the new one.
        gridArtSize_ = newGridArtSize;
        clearGridArtTexCache();
    }
    gridTileSize_ = gridArtSize_ + kGridArtMargin;

    // Tile text block height from the ACTUAL text sizes (two title lines +
    // artist + breathing room) — see gridRowGap_'s comment in the header.
    gridRowGap_ = (int)(titleArtistAdvance(textSizes_.body) * 2.0f
                        + textSizes_.secondary * 1.35f + 18.0f * us);

    // Track rows likewise scale with their text.
    trackRowHeight_ = (int)(40 * us);

    int albumRows = ((int)gridIndices_.size() + gridCols_ - 1) / gridCols_;
    gridTotalHeight_ = albumRows * (gridTileSize_ + gridRowGap_) + gridPadY_;

    // Sidebar items — search box sits between the brand and the nav. All
    // Y positions scale with the text (fixed values put "Albums" visibly
    // adrift of the search box across resolutions).
    rcBrand_       = { 0, 0, sidebarW, (int)(50 * us) };
    rcSearch_      = { 12, (int)(58 * us), sidebarW - 12, (int)(90 * us) };
    rcNavAlbums_   = { 0, (int)(102 * us), sidebarW, (int)(142 * us) };
    rcNavSettings_ = { 0, (int)(142 * us), sidebarW, (int)(182 * us) };

    // Transport sub-regions — proportional to the (scaled) bar height.
    int tTop = rcTransport_.top;
    int tPad = (int)(12 * us);
    int artSide = transportH - 2 * tPad;
    rcTransportArt_  = { tPad, tTop + tPad, tPad + artSide, tTop + tPad + artSide };

    // Center buttons: the app's primary interactive elements (44px at the
    // reference window; scaled like everything else). Three of them:
    // prev / play-stop / next (no pause, no separate stop).
    int btnSize = (int)(44 * us);
    int btnGap = (int)(12 * us);
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
    rcTransportInfo_ = { rcTransportArt_.right + tPad, tTop + (int)(16 * us),
                         rcBtnPrev_.left - 76, tTop + transportH - tPad };

    // (The album view has no on-screen close button — Escape closes it.)

    // Settings page items — geometry scales with the same text factor as
    // the panels (fixed 400x50 rows under height-scaled text looked cramped
    // and off-center at 1080p), centered on the content area with a uniform
    // vertical rhythm.
    int settCx   = (rcGrid_.left + rcGrid_.right) / 2;
    int settTop  = (int)(90.0f * us);
    int rowHalfW = (int)(220.0f * us);
    int rowH     = (int)(52.0f * us);
    int rowStep  = rowH + (int)(14.0f * us);
    auto settRow = [&](int i) -> RECT {
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

RECT PlayerWindow::computeCompleteWindowRect(HMONITOR mon) const {
    // True fullscreen: the window covers the monitor's entire resolution,
    // including over the taskbar (rcMonitor, not rcWork) — sized to whichever
    // monitor the caller asks about, so this stays correct after the window
    // (or its whole desktop) moves to a different display (see
    // adaptToCurrentMonitor()). kFixedWindowStyle is WS_POPUP (no title bar,
    // no borders), so the window rect and the monitor rect are the same
    // thing — no AdjustWindowRectEx needed.
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    return mi.rcMonitor;
}

RECT PlayerWindow::computeEssentialWindowRect(HMONITOR mon) const {
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    int monW = mi.rcWork.right - mi.rcWork.left;
    int monH = mi.rcWork.bottom - mi.rcWork.top;

    // Anchor to whichever dimension is the monitor's constraint, phone-shaped
    // (9:16) on the OPPOSITE axis from the monitor's own orientation — a
    // portrait "phone" on a landscape monitor, a landscape one on a portrait
    // monitor. Self-fitting by construction; see player_window.h's UiMode.
    int contentW, contentH;
    if (monW >= monH) {
        contentH = monH;
        contentW = (int)std::lround(contentH * 9.0 / 16.0);
    } else {
        contentW = monW;
        contentH = (int)std::lround(contentW * 9.0 / 16.0);
    }

    RECT r = { 0, 0, contentW, contentH };
    AdjustWindowRectEx(&r, kFixedWindowStyle, FALSE, kFixedWindowExStyle);
    int w = std::min((int)(r.right - r.left), monW);
    int h = std::min((int)(r.bottom - r.top), monH);
    int x = mi.rcWork.left + (monW - w) / 2;
    int y = mi.rcWork.top + (monH - h) / 2;
    return { x, y, x + w, y + h };
}

void PlayerWindow::toggleUiMode() {
    uiMode_ = (uiMode_ == UiMode::Complete) ? UiMode::Essential : UiMode::Complete;
    HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    RECT r = (uiMode_ == UiMode::Complete)
        ? computeCompleteWindowRect(mon)
        : computeEssentialWindowRect(mon);
    SetWindowPos(hwnd_, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    // The window just jumped straight to its new size in one step (Essential
    // <-> Complete, the latter now fullscreen) rather than resizing
    // gradually — draw() only discovers a resize lazily otherwise, so the
    // very next frame would render at the stale swapchain extent and come
    // out corrupted/torn. Same reasoning as ArtWindow::show()'s notifyResized().
    if (renderer_) renderer_->notifyResized();
    recalcLayout();
    invalidate();
}

void PlayerWindow::adaptToCurrentMonitor() {
    // A minimized window's rect is Windows' off-screen placeholder
    // (conventionally around (-32000,-32000)), not a real position — fitting
    // against "whichever monitor is nearest that" is meaningless, and
    // calling SetWindowPos on a minimized window here would fight the user's
    // own minimize (each reposition re-fires WM_WINDOWPOSCHANGED, which previously
    // could re-trigger this and thrash instead of settling, tanking the frame
    // rate). Skip entirely while minimized; the real position is re-checked
    // (and any real monitor change caught) as soon as it's restored.
    if (IsIconic(hwnd_)) return;

    HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    lastMonitor_ = mon;  // set first: avoids re-entering via the WM_WINDOWPOSCHANGED
                         // this function's own SetWindowPos below will trigger

    RECT r = (uiMode_ == UiMode::Complete)
        ? computeCompleteWindowRect(mon)
        : computeEssentialWindowRect(mon);

    RECT cur{};
    GetWindowRect(hwnd_, &cur);
    if (cur.left == r.left && cur.top == r.top &&
        cur.right - cur.left == r.right - r.left &&
        cur.bottom - cur.top == r.bottom - r.top)
        return;  // already correct for this monitor — nothing to do

    SetWindowPos(hwnd_, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    // Same one-step-jump reasoning as toggleUiMode() — moving to a
    // different-resolution monitor can be just as large a jump.
    if (renderer_) renderer_->notifyResized();
    // Re-layout and redraw IMMEDIATELY: without this, the frame rendered
    // for the previous monitor's extent stayed on screen (letterboxed with
    // black bars on a taller monitor) until some unrelated event happened
    // to invalidate — the "black bars for a moment after moving monitors".
    recalcLayout();
    invalidate();
}

int PlayerWindow::essentialHitTest(int x, int y) const {
    POINT pt{ x, y };
    if (PtInRect(&rcEssentialPrev_, pt))     return 0;
    if (PtInRect(&rcEssentialPlayStop_, pt)) return 1;
    if (PtInRect(&rcEssentialNext_, pt))     return 2;
    return -1;
}

// Alt+F/J/C/U/G/H — the window-move mechanism in place of title-bar dragging
// (there is no title bar; see kFixedWindowStyle). Keeps the window's current
// size, aligns it to the given edge (or center) of its current monitor.
void PlayerWindow::snapToEdge(int hotkeyId) {
    RECT wr{};
    GetWindowRect(hwnd_, &wr);
    int w = wr.right - wr.left, h = wr.bottom - wr.top;

    HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    int monW = mi.rcWork.right - mi.rcWork.left;
    int monH = mi.rcWork.bottom - mi.rcWork.top;

    int x, y;
    switch (hotkeyId) {
    case kHotkeySnapLeft:
        x = mi.rcWork.left;
        y = mi.rcWork.top + std::max(0, (monH - h) / 2);
        break;
    case kHotkeySnapRight:
        x = mi.rcWork.right - w;
        y = mi.rcWork.top + std::max(0, (monH - h) / 2);
        break;
    case kHotkeySnapBottom:
        x = mi.rcWork.left + std::max(0, (monW - w) / 2);
        y = mi.rcWork.bottom - h;
        break;
    case kHotkeySnapTop:
        x = mi.rcWork.left + std::max(0, (monW - w) / 2);
        y = mi.rcWork.top;
        break;
    case kHotkeySnapCenterG:
    case kHotkeySnapCenterH:
        x = mi.rcWork.left + std::max(0, (monW - w) / 2);
        y = mi.rcWork.top + std::max(0, (monH - h) / 2);
        break;
    default:
        return;
    }
    SetWindowPos(hwnd_, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
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
        PostMessageW(hwnd_, WM_APP_ART_DECODED, 0, 0);
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

    // Artist folder is one level up (two for Singles/<Title> layouts).
    fsys::path artistDir = albumDir.parent_path();
    if (artistDir.filename().u8string() == "Singles")
        artistDir = artistDir.parent_path();
    if (artistDir.empty()) return;

    artistBioText_ = loadSidecarText(artistDir, { "bio" });
    std::string artistImg = resolveArtPath(artistDir.u8string());
    if (!artistImg.empty()) {
        FileByteReader reader;
        artistImgTex_ = createTextureFromImageFile(*renderer_, reader,
                                                   artistImg.c_str(), 256, 256);
    }
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
        if (!searchQuery_.empty()) {
            const Album& a = albums_[i];
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

int PlayerWindow::trackPanelHitTest(int x, int y) const {
    if (!trackPanelOpen_ || activeNavItem_ != 0) return -1;
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
    POINT pt = { x, y };
    if (PtInRect(&rcNavAlbums_, pt)) return 0;
    if (PtInRect(&rcNavSettings_, pt)) return 1;
    return -1;
}

int PlayerWindow::transportBtnHitTest(int x, int y) const {
    POINT pt = { x, y };
    if (PtInRect(&rcBtnPrev_, pt)) return 0;
    if (PtInRect(&rcBtnPlay_, pt)) return 1;
    if (PtInRect(&rcBtnNext_, pt)) return 2;
    return -1;
}

int PlayerWindow::settingsHitTest(int x, int y) const {
    POINT pt = { x, y };
    if (PtInRect(&rcSettingsAddFolder_, pt)) return 0;
    if (PtInRect(&rcSettingsManage_, pt)) return 1;
    if (PtInRect(&rcSettingsAudio_, pt)) return 2;
    if (PtInRect(&rcSettingsEq_, pt)) return 3;
    if (PtInRect(&rcSettingsBitperfect_, pt)) return 4;
    return -1;
}

// ── Mouse handling ───────────────────────────────────────────────────────────

void PlayerWindow::onMouseMove(int x, int y) {
    if (!mouseTracking_) {
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd_, 0 };
        TrackMouseEvent(&tme);
        mouseTracking_ = true;
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

    POINT pt = { x, y };

    if (PtInRect(&rcSidebar_, pt)) {
        hoverSidebarItem_ = sidebarHitTest(x, y);
    } else if (PtInRect(&rcTransport_, pt)) {
        hoverTransportBtn_ = transportBtnHitTest(x, y);
        hoverDspBadge_ = PtInRect(&rcDspBadge_, pt) != 0;
    } else if (trackPanelOpen_ && activeNavItem_ == 0 && PtInRect(&rcTrackPanel_, pt)) {
        hoverTrackIdx_ = trackPanelHitTest(x, y);
    } else if (PtInRect(&rcGrid_, pt)) {
        if (activeNavItem_ == 0)
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
    POINT pt = { x, y };

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
        searchFocused_ = PtInRect(&rcSearch_, pt) != 0;
        if (searchFocused_ != wasFocused) invalidate();
        if (searchFocused_) return;
    }

    // Transport buttons — middle is the combined play-stop toggle, same
    // rule as the VK_SPACE handler.
    int btn = transportBtnHitTest(x, y);
    if (btn == 0) { onPrev(); return; }
    if (btn == 1) { if (isPlaying_) onStop(); else onPlay(); return; }
    if (btn == 2) { onNext(); return; }

    // Transport art -> fullscreen
    if (PtInRect(&rcTransportArt_, pt) && transportArtTex_ != kInvalidTexture) {
        onArtClick();
        return;
    }

    // Sidebar
    if (PtInRect(&rcSidebar_, pt)) {
        int nav = sidebarHitTest(x, y);
        if (nav >= 0 && nav != activeNavItem_) {
            activeNavItem_ = nav;
            invalidate();
        }
        return;
    }

    // (No album view back button — Escape closes it.)

    // Album view track click
    if (trackPanelOpen_ && activeNavItem_ == 0 && PtInRect(&rcTrackPanel_, pt)) {
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
    if (activeNavItem_ == 0 && !trackPanelOpen_ && PtInRect(&rcGrid_, pt)) {
        int idx = gridHitTest(x, y);
        if (idx >= 0) openAlbumView(idx);
        return;
    }

    // Settings page
    if (activeNavItem_ == 1 && PtInRect(&rcGrid_, pt)) {
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
    POINT pt = { x, y };

    // Double-click on grid tile: play first track
    if (activeNavItem_ == 0 && !trackPanelOpen_ && PtInRect(&rcGrid_, pt)) {
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
    if (trackPanelOpen_ && PtInRect(&rcTrackPanel_, pt)) {
        int track = trackPanelHitTest(x, y);
        if (track >= 0) {
            currentAlbum_ = selectedAlbumIdx_;
            currentTrack_ = track;
            onPlay();
        }
        return;
    }
}

void PlayerWindow::onMouseWheel(int x, int y, int delta) {
    POINT pt = { x, y };
    ScreenToClient(hwnd_, &pt);

    if (trackPanelOpen_ && activeNavItem_ == 0 && PtInRect(&rcTrackPanel_, pt)) {
        // The album view scrolls as one page; its content height is
        // measured by the draw block (albumViewContentH_).
        trackScrollY_ -= delta;
        int panelH = rcTrackPanel_.bottom - rcTrackPanel_.top;
        trackScrollY_ = std::clamp(trackScrollY_, 0,
                                   std::max(0, albumViewContentH_ - panelH));
        invalidate(&rcTrackPanel_);
        return;
    }

    if (!trackPanelOpen_ && PtInRect(&rcGrid_, pt)) {
        gridScrollY_ -= delta;
        int gridH = rcGrid_.bottom - rcGrid_.top;
        gridScrollY_ = std::clamp(gridScrollY_, 0, std::max(0, gridTotalHeight_ - gridH));
        invalidate(&rcGrid_);
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

// ── Manage Folders dialog ─────────────────────────────────────────────────────

#define ID_DLG_LIST   301
#define ID_DLG_REMOVE 302
#define ID_DLG_DONE   303

struct ManageDlgCtx {
    Db*   db;
    HWND  parent;
    HWND  list;
    bool  changed = false;
};

static LRESULT CALLBACK manageFoldersDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* ctx = (ManageDlgCtx*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_CREATE: {
        ctx = (ManageDlgCtx*)((CREATESTRUCTW*)lp)->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)ctx);
        HINSTANCE hi = ((CREATESTRUCTW*)lp)->hInstance;
        ctx->list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
            8, 8, 484, 200, hwnd, (HMENU)ID_DLG_LIST, hi, nullptr);
        for (auto& r : ctx->db->loadMusicRoots()) {
            SendMessageW(ctx->list, LB_ADDSTRING, 0, (LPARAM)utf8ToWide(r).c_str());
        }
        CreateWindowExW(0, L"BUTTON", L"Remove Selected",
            WS_CHILD | WS_VISIBLE, 8, 216, 150, 28, hwnd, (HMENU)ID_DLG_REMOVE, hi, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Done",
            WS_CHILD | WS_VISIBLE, 422, 216, 70, 28, hwnd, (HMENU)ID_DLG_DONE, hi, nullptr);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == ID_DLG_REMOVE) {
            int sel = (int)SendMessageW(ctx->list, LB_GETCURSEL, 0, 0);
            if (sel == LB_ERR) break;
            wchar_t buf[MAX_PATH] = {};
            SendMessageW(ctx->list, LB_GETTEXT, sel, (LPARAM)buf);
            int pLen = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
            std::string path(pLen, '\0');
            WideCharToMultiByte(CP_UTF8, 0, buf, -1, path.data(), pLen, nullptr, nullptr);
            if (!path.empty() && path.back() == '\0') path.pop_back();
            ctx->db->removeMusicRoot(path);
            SendMessageW(ctx->list, LB_DELETESTRING, sel, 0);
            ctx->changed = true;
        } else if (LOWORD(wp) == ID_DLG_DONE) {
            DestroyWindow(hwnd);
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        break;
    case WM_DESTROY:
        PostMessageW(ctx->parent, WM_NULL, 0, 0);
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void PlayerWindow::onManageFolders() {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc   = manageFoldersDlgProc;
        wc.hInstance     = hInst_;
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"MatrixManageFolders";
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        registered = true;
    }

    ManageDlgCtx ctx{ &db_, hwnd_, nullptr, false };
    HWND dlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME, L"MatrixManageFolders", L"Music Folders",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        0, 0, 510, 290, hwnd_, nullptr, hInst_, &ctx);

    RECT pr; GetWindowRect(hwnd_, &pr);
    RECT dr; GetWindowRect(dlg, &dr);
    int dw = dr.right - dr.left, dh = dr.bottom - dr.top;
    SetWindowPos(dlg, nullptr,
        pr.left + (pr.right - pr.left - dw) / 2,
        pr.top  + (pr.bottom - pr.top - dh) / 2,
        0, 0, SWP_NOSIZE | SWP_NOZORDER);

    EnableWindow(hwnd_, FALSE);
    ShowWindow(dlg, SW_SHOW);

    MSG msg;
    while (IsWindow(dlg) && GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    EnableWindow(hwnd_, TRUE);
    SetForegroundWindow(hwnd_);
    markDirty();  // repaint the main window, obscured/disabled while the dialog was up

    if (!ctx.changed) return;
    watcher_.unwatchAll();
    setupWatchers();
    startBackgroundScan();
}

// ── Audio Settings dialog ─────────────────────────────────────────────────────

#define ID_AUDIO_USB       401
#define ID_AUDIO_WASAPI    402
#define ID_AUDIO_DEVICE    403
#define ID_AUDIO_SHARED    404
#define ID_AUDIO_EXCLUSIVE 405
#define ID_AUDIO_APPLY     406
#define ID_AUDIO_USB_DEV   407

struct AudioSettingsDlgCtx {
    Db*    db;
    HWND   parent;
    HWND   rdoUsb, rdoWasapi;
    HWND   cmbDevice;
    HWND   cmbUsbDevice;
    HWND   rdoShared, rdoExclusive;
    HWND   lblDevice, lblMode;
    HWND   lblUsbDevice;
    std::vector<WasapiDeviceInfo> devices;
    std::vector<UsbAudioDeviceInfo> usbDevices;
    bool   applied = false;
};

static void audioSetBackendControls(AudioSettingsDlgCtx* ctx, bool wasapi) {
    EnableWindow(ctx->cmbDevice,    wasapi);
    EnableWindow(ctx->rdoShared,    wasapi);
    EnableWindow(ctx->rdoExclusive, wasapi);
    EnableWindow(ctx->lblDevice,    wasapi);
    EnableWindow(ctx->lblMode,      wasapi);
    EnableWindow(ctx->cmbUsbDevice, !wasapi);
    EnableWindow(ctx->lblUsbDevice, !wasapi);
}

static LRESULT CALLBACK audioSettingsDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* ctx = (AudioSettingsDlgCtx*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_CREATE: {
        ctx = (AudioSettingsDlgCtx*)((CREATESTRUCTW*)lp)->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)ctx);
        HINSTANCE hi = ((CREATESTRUCTW*)lp)->hInstance;
        int x = 12, y = 10;

        CreateWindowExW(0, L"STATIC", L"Output backend:",
            WS_CHILD | WS_VISIBLE, x, y, 200, 18, hwnd, nullptr, hi, nullptr);
        y += 22;
        ctx->rdoUsb = CreateWindowExW(0, L"BUTTON", L"USB Direct (libusbK)",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
            x + 8, y, 250, 20, hwnd, (HMENU)ID_AUDIO_USB, hi, nullptr);
        y += 24;
        ctx->rdoWasapi = CreateWindowExW(0, L"BUTTON", L"WASAPI",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
            x + 8, y, 250, 20, hwnd, (HMENU)ID_AUDIO_WASAPI, hi, nullptr);
        y += 32;

        ctx->lblUsbDevice = CreateWindowExW(0, L"STATIC", L"USB DAC:",
            WS_CHILD | WS_VISIBLE, x, y, 60, 18, hwnd, nullptr, hi, nullptr);
        ctx->cmbUsbDevice = CreateWindowExW(0, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            x + 64, y - 2, 370, 200, hwnd, (HMENU)ID_AUDIO_USB_DEV, hi, nullptr);
        y += 32;

        ctx->lblDevice = CreateWindowExW(0, L"STATIC", L"Device:",
            WS_CHILD | WS_VISIBLE, x, y, 60, 18, hwnd, nullptr, hi, nullptr);
        ctx->cmbDevice = CreateWindowExW(0, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            x + 64, y - 2, 370, 200, hwnd, (HMENU)ID_AUDIO_DEVICE, hi, nullptr);
        y += 32;

        ctx->lblMode = CreateWindowExW(0, L"STATIC", L"Mode:",
            WS_CHILD | WS_VISIBLE, x, y, 200, 18, hwnd, nullptr, hi, nullptr);
        y += 22;
        ctx->rdoShared = CreateWindowExW(0, L"BUTTON",
            L"Shared  \x2014  other apps can play simultaneously",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
            x + 8, y, 420, 20, hwnd, (HMENU)ID_AUDIO_SHARED, hi, nullptr);
        y += 24;
        ctx->rdoExclusive = CreateWindowExW(0, L"BUTTON",
            L"Exclusive  \x2014  lower latency, blocks other apps",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
            x + 8, y, 420, 20, hwnd, (HMENU)ID_AUDIO_EXCLUSIVE, hi, nullptr);
        y += 36;

        CreateWindowExW(0, L"BUTTON", L"Apply",
            WS_CHILD | WS_VISIBLE, 346, y, 80, 28, hwnd, (HMENU)ID_AUDIO_APPLY, hi, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Cancel",
            WS_CHILD | WS_VISIBLE, 434, y, 80, 28, hwnd, (HMENU)IDCANCEL, hi, nullptr);

        // Populate USB device list
        ctx->usbDevices = UsbAudioDriver::enumerateUsbAudioDevices();
        for (auto& ud : ctx->usbDevices) {
            std::wstring wname(utf8ToWide(ud.name));
            SendMessageW(ctx->cmbUsbDevice, CB_ADDSTRING, 0, (LPARAM)wname.c_str());
        }
        auto savedVid = ctx->db->loadSetting("usb_vid");
        auto savedPid = ctx->db->loadSetting("usb_pid");
        int usbSel = 0;
        if (!savedVid.empty() && !savedPid.empty()) {
            uint16_t sv = (uint16_t)strtoul(savedVid.c_str(), nullptr, 16);
            uint16_t sp = (uint16_t)strtoul(savedPid.c_str(), nullptr, 16);
            for (int i = 0; i < (int)ctx->usbDevices.size(); i++) {
                if (ctx->usbDevices[i].vid == sv && ctx->usbDevices[i].pid == sp) {
                    usbSel = i; break;
                }
            }
        }
        if (!ctx->usbDevices.empty())
            SendMessageW(ctx->cmbUsbDevice, CB_SETCURSEL, usbSel, 0);

        // Populate WASAPI device list
        SendMessageW(ctx->cmbDevice, CB_ADDSTRING, 0, (LPARAM)L"(Default device)");
        ctx->devices = WasapiOutput::enumerateDevices();
        for (auto& d : ctx->devices)
            SendMessageW(ctx->cmbDevice, CB_ADDSTRING, 0, (LPARAM)d.name.c_str());

        bool wasapi = (ctx->db->loadSetting("audio_backend") == "wasapi");
        SendMessageW(wasapi ? ctx->rdoWasapi : ctx->rdoUsb, BM_SETCHECK, BST_CHECKED, 0);
        audioSetBackendControls(ctx, wasapi);

        auto savedId = ctx->db->loadSetting("wasapi_device_id");
        int devSel = 0;
        for (int i = 0; i < (int)ctx->devices.size(); i++) {
            int idLen = WideCharToMultiByte(CP_UTF8, 0, ctx->devices[i].id.c_str(), -1, nullptr, 0, nullptr, nullptr);
            std::string id(idLen, '\0');
            WideCharToMultiByte(CP_UTF8, 0, ctx->devices[i].id.c_str(), -1, id.data(), idLen, nullptr, nullptr);
            if (!id.empty() && id.back() == '\0') id.pop_back();
            if (id == savedId) { devSel = i + 1; break; }
        }
        SendMessageW(ctx->cmbDevice, CB_SETCURSEL, devSel, 0);

        bool exclusive = (ctx->db->loadSetting("wasapi_mode") == "exclusive");
        SendMessageW(exclusive ? ctx->rdoExclusive : ctx->rdoShared, BM_SETCHECK, BST_CHECKED, 0);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_AUDIO_USB:
            audioSetBackendControls(ctx, false);
            break;
        case ID_AUDIO_WASAPI:
            audioSetBackendControls(ctx, true);
            break;
        case ID_AUDIO_APPLY: {
            bool wasapi = (SendMessageW(ctx->rdoWasapi, BM_GETCHECK, 0, 0) == BST_CHECKED);
            ctx->db->saveSetting("audio_backend", wasapi ? "wasapi" : "usb");
            if (wasapi) {
                int sel = (int)SendMessageW(ctx->cmbDevice, CB_GETCURSEL, 0, 0);
                std::string devId;
                if (sel > 0 && sel <= (int)ctx->devices.size()) {
                    auto& ws = ctx->devices[sel - 1].id;
                    int dLen = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
                    devId.assign(dLen, '\0');
                    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, devId.data(), dLen, nullptr, nullptr);
                    if (!devId.empty() && devId.back() == '\0') devId.pop_back();
                }
                ctx->db->saveSetting("wasapi_device_id", devId);
                bool excl = (SendMessageW(ctx->rdoExclusive, BM_GETCHECK, 0, 0) == BST_CHECKED);
                ctx->db->saveSetting("wasapi_mode", excl ? "exclusive" : "shared");
            } else {
                int usel = (int)SendMessageW(ctx->cmbUsbDevice, CB_GETCURSEL, 0, 0);
                if (usel >= 0 && usel < (int)ctx->usbDevices.size()) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "%04X", ctx->usbDevices[usel].vid);
                    ctx->db->saveSetting("usb_vid", buf);
                    snprintf(buf, sizeof(buf), "%04X", ctx->usbDevices[usel].pid);
                    ctx->db->saveSetting("usb_pid", buf);
                }
            }
            ctx->applied = true;
            DestroyWindow(hwnd);
            break;
        }
        case IDCANCEL:
            DestroyWindow(hwnd);
            break;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        break;
    case WM_DESTROY:
        PostMessageW(ctx->parent, WM_NULL, 0, 0);
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void PlayerWindow::onAudioSettings() {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc   = audioSettingsDlgProc;
        wc.hInstance     = hInst_;
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"MatrixAudioSettings";
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        registered = true;
    }

    AudioSettingsDlgCtx ctx{ &db_, hwnd_ };
    HWND dlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME, L"MatrixAudioSettings", L"Audio Settings",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        0, 0, 530, 297, hwnd_, nullptr, hInst_, &ctx);

    RECT pr; GetWindowRect(hwnd_, &pr);
    RECT dr; GetWindowRect(dlg, &dr);
    int dw = dr.right - dr.left, dh = dr.bottom - dr.top;
    SetWindowPos(dlg, nullptr,
        pr.left + (pr.right - pr.left - dw) / 2,
        pr.top  + (pr.bottom - pr.top  - dh) / 2,
        0, 0, SWP_NOSIZE | SWP_NOZORDER);

    EnableWindow(hwnd_, FALSE);
    ShowWindow(dlg, SW_SHOW);

    MSG msg;
    while (IsWindow(dlg) && GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    EnableWindow(hwnd_, TRUE);
    SetForegroundWindow(hwnd_);
    markDirty();  // repaint the main window, obscured/disabled while the dialog was up

    if (!ctx.applied) return;

    onStop();

    useWasapi_ = (db_.loadSetting("audio_backend") == "wasapi");
    wasapiMode_ = (db_.loadSetting("wasapi_mode") == "exclusive")
                  ? WasapiMode::Exclusive : WasapiMode::Shared;
    auto devIdUtf8 = db_.loadSetting("wasapi_device_id");
    wasapiDeviceId_ = utf8ToWide(devIdUtf8);

    if (useWasapi_) {
        output_ = std::make_unique<WasapiOutput>(wasapiDeviceId_, wasapiMode_);
    } else {
        auto vidStr = db_.loadSetting("usb_vid");
        auto pidStr = db_.loadSetting("usb_pid");
        uint16_t vid = vidStr.empty() ? (uint16_t)0x32BB : (uint16_t)strtoul(vidStr.c_str(), nullptr, 16);
        uint16_t pid = pidStr.empty() ? (uint16_t)0x0004 : (uint16_t)strtoul(pidStr.c_str(), nullptr, 16);

        usbDriver_.close();
        usbOpen_ = usbDriver_.open(vid, pid);
        if (usbOpen_) usbDriver_.parseDescriptors();
        output_ = std::make_unique<UsbAudioOutput>(usbDriver_);
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

// ── EQ Settings dialog ──────────────────────────────────────────────────────

#define ID_EQ_SEARCH  501
#define ID_EQ_LIST    502
#define ID_EQ_ASSIGN  503
#define ID_EQ_CLEAR   504
#define ID_EQ_CLOSE   505

struct EqSettingsDlgCtx {
    Db*                        db;
    HWND                       parent;
    HWND                       editSearch;
    HWND                       listProfiles;
    HWND                       lblDevice;
    HWND                       lblSelected;
    HWND                       lblDetails;
    const std::vector<EqProfile>* allProfiles;
    std::vector<int>           filteredIndices;
    std::string                deviceKey;
    EqManager*                 eqManager;
    const EqProfileStore*      profileStore;
    int                        currentSampleRate;
    int                        currentChannels;
    bool                       bitperfectActive = false;
    bool                       changed = false;
};

static void eqFilterList(EqSettingsDlgCtx* ctx) {
    wchar_t searchBuf[256] = {};
    GetWindowTextW(ctx->editSearch, searchBuf, 256);
    std::wstring search(searchBuf);
    for (auto& c : search) c = towlower(c);

    SendMessageW(ctx->listProfiles, WM_SETREDRAW, FALSE, 0);
    SendMessageW(ctx->listProfiles, LB_RESETCONTENT, 0, 0);
    ctx->filteredIndices.clear();

    for (int i = 0; i < (int)ctx->allProfiles->size(); i++) {
        auto& p = (*ctx->allProfiles)[i];
        std::wstring nameW(utf8ToWide(p.name));
        std::wstring nameLower = nameW;
        for (auto& c : nameLower) c = towlower(c);

        if (!search.empty() && nameLower.find(search) == std::wstring::npos)
            continue;

        std::wstring label = nameW;
        if (!p.form.empty()) {
            std::wstring formW(utf8ToWide(p.form));
            label += L"  (" + formW + L")";
        }
        SendMessageW(ctx->listProfiles, LB_ADDSTRING, 0, (LPARAM)label.c_str());
        ctx->filteredIndices.push_back(i);
    }
    SendMessageW(ctx->listProfiles, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(ctx->listProfiles, nullptr, TRUE);
}

static void eqUpdateSelection(EqSettingsDlgCtx* ctx) {
    int sel = (int)SendMessageW(ctx->listProfiles, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR || sel >= (int)ctx->filteredIndices.size()) {
        SetWindowTextW(ctx->lblSelected, L"No profile selected");
        SetWindowTextW(ctx->lblDetails, L"");
        return;
    }
    int idx = ctx->filteredIndices[sel];
    auto& p = (*ctx->allProfiles)[idx];
    std::wstring nameW(utf8ToWide(p.name));
    std::wstring formW(utf8ToWide(p.form));
    std::wstring selText = nameW;
    if (!formW.empty()) selText += L"  (" + formW + L")";
    SetWindowTextW(ctx->lblSelected, selText.c_str());

    wchar_t detailBuf[128];
    swprintf_s(detailBuf, 128, L"Preamp: %.1f dB  |  Filters: %d",
               p.preamp, (int)p.filters.size());
    SetWindowTextW(ctx->lblDetails, detailBuf);
}

static LRESULT CALLBACK eqSettingsDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* ctx = (EqSettingsDlgCtx*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_CREATE: {
        ctx = (EqSettingsDlgCtx*)((CREATESTRUCTW*)lp)->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)ctx);
        HINSTANCE hi = ((CREATESTRUCTW*)lp)->hInstance;
        int x = 12, y = 10;

        std::wstring devKeyW(utf8ToWide(ctx->deviceKey));
        std::wstring devLabel = L"Current device: " + devKeyW;
        ctx->lblDevice = CreateWindowExW(0, L"STATIC", devLabel.c_str(),
            WS_CHILD | WS_VISIBLE, x, y, 460, 18, hwnd, nullptr, hi, nullptr);
        y += 22;

        if (ctx->bitperfectActive) {
            CreateWindowExW(0, L"STATIC",
                L"Bitperfect mode active \x2014 EQ changes apply when DSP mode is enabled.",
                WS_CHILD | WS_VISIBLE, x, y, 460, 18, hwnd, nullptr, hi, nullptr);
            y += 22;
        }

        // Show current assignment
        EqAssignment assign;
        std::wstring currentAssign = L"No EQ assigned";
        if (ctx->db->loadEqAssignment(ctx->deviceKey, assign) ||
            ctx->db->loadEqAssignment("global", assign)) {
            std::wstring n(utf8ToWide(assign.name));
            currentAssign = L"Current EQ: " + n;
        }
        CreateWindowExW(0, L"STATIC", currentAssign.c_str(),
            WS_CHILD | WS_VISIBLE, x, y, 460, 18, hwnd, nullptr, hi, nullptr);
        y += 30;

        CreateWindowExW(0, L"STATIC", L"Search:",
            WS_CHILD | WS_VISIBLE, x, y + 2, 50, 18, hwnd, nullptr, hi, nullptr);
        ctx->editSearch = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr,
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            x + 54, y, 406, 22, hwnd, (HMENU)ID_EQ_SEARCH, hi, nullptr);
        y += 30;

        ctx->listProfiles = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
            x, y, 460, 240, hwnd, (HMENU)ID_EQ_LIST, hi, nullptr);
        y += 248;

        ctx->lblSelected = CreateWindowExW(0, L"STATIC", L"No profile selected",
            WS_CHILD | WS_VISIBLE, x, y, 460, 18, hwnd, nullptr, hi, nullptr);
        y += 20;
        ctx->lblDetails = CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE, x, y, 460, 18, hwnd, nullptr, hi, nullptr);
        y += 30;

        CreateWindowExW(0, L"BUTTON", L"Assign to Device",
            WS_CHILD | WS_VISIBLE, x, y, 140, 28, hwnd, (HMENU)ID_EQ_ASSIGN, hi, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Clear",
            WS_CHILD | WS_VISIBLE, x + 150, y, 80, 28, hwnd, (HMENU)ID_EQ_CLEAR, hi, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Close",
            WS_CHILD | WS_VISIBLE, 392, y, 80, 28, hwnd, (HMENU)ID_EQ_CLOSE, hi, nullptr);

        eqFilterList(ctx);
        return 0;
    }
    case WM_COMMAND:
        if (HIWORD(wp) == EN_CHANGE && LOWORD(wp) == ID_EQ_SEARCH) {
            eqFilterList(ctx);
        } else if (HIWORD(wp) == LBN_SELCHANGE && LOWORD(wp) == ID_EQ_LIST) {
            eqUpdateSelection(ctx);
        } else if (LOWORD(wp) == ID_EQ_ASSIGN) {
            int sel = (int)SendMessageW(ctx->listProfiles, LB_GETCURSEL, 0, 0);
            if (sel == LB_ERR || sel >= (int)ctx->filteredIndices.size()) {
                MessageBoxW(hwnd, L"Select a profile first.", L"EQ", MB_OK);
                break;
            }
            int idx = ctx->filteredIndices[sel];
            auto& p = (*ctx->allProfiles)[idx];
            ctx->db->saveEqAssignment(ctx->deviceKey, p.name, p.source, p.form);
            if (!ctx->bitperfectActive) {
                auto* profile = ctx->profileStore->findByKey(p.name, p.source, p.form);
                if (profile && ctx->eqManager)
                    ctx->eqManager->applyProfile(profile, ctx->currentSampleRate, ctx->currentChannels);
            }
            ctx->changed = true;
            std::wstring nameW(utf8ToWide(p.name));
            if (ctx->bitperfectActive) {
                std::wstring msg = L"Saved: " + nameW + L"\nWill be active when DSP mode is enabled.";
                MessageBoxW(hwnd, msg.c_str(), L"EQ Profile Saved (Bitperfect)", MB_OK | MB_ICONINFORMATION);
            } else {
                std::wstring msg = L"Assigned: " + nameW;
                MessageBoxW(hwnd, msg.c_str(), L"EQ Profile Assigned", MB_OK | MB_ICONINFORMATION);
            }
        } else if (LOWORD(wp) == ID_EQ_CLEAR) {
            ctx->db->clearEqAssignment(ctx->deviceKey);
            if (ctx->eqManager) ctx->eqManager->clear();
            ctx->changed = true;
            MessageBoxW(hwnd, L"EQ assignment cleared.", L"EQ", MB_OK | MB_ICONINFORMATION);
        } else if (LOWORD(wp) == ID_EQ_CLOSE) {
            DestroyWindow(hwnd);
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        break;
    case WM_DESTROY:
        PostMessageW(ctx->parent, WM_NULL, 0, 0);
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void PlayerWindow::onEqSettings() {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc   = eqSettingsDlgProc;
        wc.hInstance     = hInst_;
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"MatrixEqSettings";
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        registered = true;
    }

    int sr = 44100, ch = 2;
    if (output_) {
        int r = output_->getConfiguredRate();
        int c = output_->getConfiguredChannels();
        if (r > 0) sr = r;
        if (c > 0) ch = c;
    }

    EqSettingsDlgCtx ctx{};
    ctx.db             = &db_;
    ctx.parent         = hwnd_;
    ctx.allProfiles    = &eqProfiles_.getAll();
    ctx.deviceKey      = getActiveDeviceKey();
    ctx.eqManager      = &eqManager_;
    ctx.profileStore   = &eqProfiles_;
    ctx.currentSampleRate = sr;
    ctx.currentChannels   = ch;
    ctx.bitperfectActive  = bitperfectMode_.load();

    HWND dlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME, L"MatrixEqSettings", L"EQ / AutoEQ Profiles",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        0, 0, 500, 520, hwnd_, nullptr, hInst_, &ctx);

    RECT pr; GetWindowRect(hwnd_, &pr);
    RECT dr; GetWindowRect(dlg, &dr);
    int dw = dr.right - dr.left, dh = dr.bottom - dr.top;
    SetWindowPos(dlg, nullptr,
        pr.left + (pr.right - pr.left - dw) / 2,
        pr.top  + (pr.bottom - pr.top  - dh) / 2,
        0, 0, SWP_NOSIZE | SWP_NOZORDER);

    EnableWindow(hwnd_, FALSE);
    ShowWindow(dlg, SW_SHOW);

    MSG msg;
    while (IsWindow(dlg) && GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    EnableWindow(hwnd_, TRUE);
    SetForegroundWindow(hwnd_);
    markDirty();  // repaint the main window, obscured/disabled while the dialog was up
}

void PlayerWindow::onAddFolder() {
    wchar_t path[MAX_PATH] = {};
    BROWSEINFOW bi = {};
    bi.hwndOwner = hwnd_;
    bi.lpszTitle = L"Select music folder to add";
    bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_USENEWUI;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    SHGetPathFromIDListW(pidl, path);
    CoTaskMemFree(pidl);

    int rLen = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
    std::string root(rLen, '\0');
    WideCharToMultiByte(CP_UTF8, 0, path, -1, root.data(), rLen, nullptr, nullptr);
    if (!root.empty() && root.back() == '\0') root.pop_back();
    db_.addMusicRoot(root);

    HWND h = hwnd_;
    watcher_.watchRoot(root, [h](const std::string&) {
        PostMessageW(h, WM_APP_SCAN_DONE, 1, 0);
    });

    startBackgroundScan();
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
    if (useWasapi_) {
        auto devId = db_.loadSetting("wasapi_device_id");
        return devId.empty() ? "wasapi" : "wasapi:" + devId;
    }
    auto vid = db_.loadSetting("usb_vid");
    auto pid = db_.loadSetting("usb_pid");
    if (vid.empty()) vid = "32BB";
    if (pid.empty()) pid = "0004";
    return vid + ":" + pid;
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
    currentTitleW_ = utf8ToWide(t.title);
    currentArtistW_ = utf8ToWide(t.artist);

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
        // Negotiate the best rate the device supports (WASAPI: probe; USB: descriptor).
        std::vector<int> supported = useWasapi_
            ? output_->probeRates(active_->channels())
            : usbDriver_.getOutputRates();
        outSr = pickOutputRate(fileSr, supported);
        cfgOk = output_->configure(outSr, active_->channels(), 32, false);
        printf("[Audio][WARN] %d Hz unsupported -> negotiated %d Hz\n", fileSr, outSr);
        fflush(stdout);
    }
    if (!cfgOk) {
        if (isBitperfect) {
            printf("[Bitperfect][ERROR] DAC does not support native sample rate %d Hz, aborting\n", fileSr);
            fflush(stdout);
            MessageBoxW(hwnd_, L"DAC does not support native sample rate.\nStrict Bitperfect mode active: playback aborted to preserve audio purity.",
                L"Bitperfect Failure", MB_OK | MB_ICONERROR);
        } else {
            printf("[Audio][ERROR] Output failed to configure at %d Hz\n", fileSr);
            fflush(stdout);
            MessageBoxW(hwnd_, L"Audio output failed to configure.\nCheck Audio Settings.",
                L"Audio configure failed", MB_OK | MB_ICONERROR);
        }
        active_->stop();
        isPlaying_ = false;
        return;
    }
    outSr = output_->getConfiguredRate();

    // Query device's maximum supported bit depth for the final quantize step.
    int deviceMaxBits = 32;
    if (useWasapi_ && !isBitperfect) {
        auto* wasapi = static_cast<WasapiOutput*>(output_.get());
        deviceMaxBits = wasapi->getMaxBitDepth(outSr, active_->channels());
        if (deviceMaxBits <= 0) deviceMaxBits = 32;
    } else if (!useWasapi_) {
        deviceMaxBits = usbDriver_.getConfiguredBitDepth();
        if (deviceMaxBits <= 0) deviceMaxBits = 32;
    }
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
            static DWORD lastShortLog = 0;
            DWORD nowMs = GetTickCount();
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
        MessageBoxW(hwnd_, L"Audio output failed to start.\nCheck Audio Settings.",
            L"Audio start failed", MB_OK | MB_ICONERROR);
        active_->stop();
        isPlaying_ = false;
        return;
    }
    printf("[onPlay] USB streaming started, ring=%zu\n", output_->ringAvailable());
    fflush(stdout);

    startGaplessCoordinator(callbackI32, capturedOutSr, capturedDacCh);
    SetTimer(hwnd_, TIMER_SEEK_UPDATE, 250, nullptr);
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
    KillTimer(hwnd_, TIMER_SEEK_UPDATE);
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
                    Sleep(5);
                }
                PostMessageW(hwnd_, WM_COMMAND, MAKEWPARAM(ID_BTN_PLAY, BN_CLICKED), 0);
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
        printf("[Audio][ERROR] WASAPI device fault detected, stopping playback\n");
        fflush(stdout);
        onStop();
        MessageBoxW(hwnd_,
            L"Audio device error (driver fault inside Windows' audio stack).\n"
            L"Playback stopped. Try unplugging/replugging the DAC.",
            L"Audio device error", MB_OK | MB_ICONERROR);
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
    invalidate(&rcTransport_);
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
    currentTitleW_  = utf8ToWide(nt.title);
    currentArtistW_ = utf8ToWide(nt.artist);
    seekTotalMs_    = nt.durationMs > 0 ? nt.durationMs : 0;
    seekPosMs_      = 0;
    loadTransportArtTexture(albums_[album].artPath);
    selectedAlbumIdx_ = album;
    loadTrackPanelArtTexture(album);
    invalidate();
}

// ── Background scan ──────────────────────────────────────────────────────────

void PlayerWindow::setupWatchers() {
    HWND h = hwnd_;
    for (auto& root : db_.loadMusicRoots()) {
        watcher_.watchRoot(root, [h](const std::string&) {
            PostMessageW(h, WM_APP_SCAN_DONE, 1, 0);
        });
    }
}

void PlayerWindow::startBackgroundScan() {
    if (scanning_.load()) return;
    if (scanThread_.joinable()) scanThread_.join();

    scanning_.store(true);
    HWND h = hwnd_;

    scanThread_ = std::thread([this, h]() {
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
        PostMessageW(h, WM_APP_SCAN_DONE, 0, 0);
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

LRESULT CALLBACK PlayerWindow::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CREATE) {
        auto* cs = (CREATESTRUCTW*)lp;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
    }
    PlayerWindow* self = (PlayerWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (self) return self->handleMsg(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT PlayerWindow::handleMsg(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE:
        recalcLayout();
        invalidate();
        return 0;

    case WM_HOTKEY:
        if ((int)wp == kHotkeyToggleMode) toggleUiMode();
        else                              snapToEdge((int)wp);
        return 0;

    case WM_DISPLAYCHANGE:
        // Some monitor's resolution/topology changed. The window's own
        // HMONITOR handle may not change even if its work area did (a
        // resolution change on the same physical monitor), so re-fit
        // unconditionally rather than gating on a monitor-handle diff.
        adaptToCurrentMonitor();
        return 0;

    case WM_WINDOWPOSCHANGED: {
        // Covers the window ending up on a different monitor by any means
        // (Win+Shift+Arrow, dragging a secondary monitor's taskbar icon,
        // etc.) — not just our own snapToEdge()/toggleUiMode(), which stay
        // on the current monitor by construction anyway.
        //
        // Skip while minimized: a minimized window's rect is Windows' off-
        // screen placeholder, not a real position, so "nearest monitor" for
        // it is meaningless — treating it as a real monitor change caused
        // adaptToCurrentMonitor() to fight the minimize (repeated reposition
        // -> repeated WM_WINDOWPOSCHANGED), cratering the frame rate.
        if (!IsIconic(hwnd_)) {
            HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
            if (mon != lastMonitor_) adaptToCurrentMonitor();
        }
        break;  // let DefWindowProc still generate WM_SIZE/WM_MOVE as usual
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        // Vulkan (drawFrame(), driven from run() while pendingFrames_ > 0)
        // owns presentation — just validate the update region so Windows
        // doesn't keep resending WM_PAINT. Windows sends this whenever the
        // window is exposed (uncovered by another window, alt-tab, etc.),
        // independent of our own invalidate() call sites, so it must still
        // mark a frame dirty or an exposed-but-otherwise-unchanged window
        // would stay stale until some unrelated interaction.
        PAINTSTRUCT ps;
        BeginPaint(hwnd_, &ps);
        EndPaint(hwnd_, &ps);
        markDirty();
        return 0;
    }

    case WM_TIMER:
        if (wp == TIMER_SEEK_UPDATE) onTimer();
        return 0;

    case WM_APP_SCAN_DONE:
        if (wp == 1) startBackgroundScan();
        else         onScanDone();
        return 0;

    case WM_APP_TRACK_CHANGE:
        applyTrackMetadata((int)wp, (int)lp);
        return 0;

    case WM_APP_ART_DECODED:
        onArtDecoded();
        return 0;

    case WM_COMMAND:
        if (LOWORD(wp) == ID_BTN_PLAY && HIWORD(wp) == BN_CLICKED)
            onPlay();
        return 0;

    case WM_MOUSEMOVE:
        onMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;

    case WM_MOUSELEAVE:
        onMouseLeave();
        return 0;

    case WM_LBUTTONDOWN:
        onLButtonDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;

    case WM_LBUTTONDBLCLK:
        onLButtonDblClk(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;

    case WM_MOUSEWHEEL:
        onMouseWheel(GET_X_LPARAM(lp), GET_Y_LPARAM(lp),
                     GET_WHEEL_DELTA_WPARAM(wp));
        return 0;

    case WM_CHAR:
        if (searchFocused_) {
            wchar_t ch = (wchar_t)wp;
            if (ch == 0x08) {  // backspace: pop one UTF-8 codepoint
                while (!searchQuery_.empty() && (searchQuery_.back() & 0xC0) == 0x80)
                    searchQuery_.pop_back();
                if (!searchQuery_.empty()) searchQuery_.pop_back();
            } else if (ch >= 0x20 && ch != 0x7F) {
                char u8[8];
                int n = WideCharToMultiByte(CP_UTF8, 0, &ch, 1, u8, sizeof(u8), nullptr, nullptr);
                if (n > 0) searchQuery_.append(u8, n);
            } else {
                return 0;  // control chars: consumed, no query change
            }
            rebuildGridIndices();
            gridScrollY_ = 0;
            recalcLayout();
            invalidate();
            return 0;
        }
        break;

    case WM_KEYDOWN:
        switch (wp) {
        case VK_SPACE:
            if (searchFocused_) break;  // typing a space, not play/stop
            if (isPlaying_) onStop(); else if (currentAlbum_ >= 0) onPlay();
            return 0;
        case VK_ESCAPE:
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
            return 0;
        }
        break;

    case WM_DESTROY:
        UnregisterHotKey(hwnd_, kHotkeySnapLeft);
        UnregisterHotKey(hwnd_, kHotkeySnapRight);
        UnregisterHotKey(hwnd_, kHotkeySnapBottom);
        UnregisterHotKey(hwnd_, kHotkeySnapTop);
        UnregisterHotKey(hwnd_, kHotkeySnapCenterG);
        UnregisterHotKey(hwnd_, kHotkeySnapCenterH);
        UnregisterHotKey(hwnd_, kHotkeyToggleMode);

        onStop();
        watcher_.unwatchAll();
        if (scanThread_.joinable()) scanThread_.join();
        // Join the art-decode worker before tearing down textures/renderer —
        // it never touches the Renderer, but a decode completing after this
        // point would PostMessage to a dying HWND.
        stopArtDecodeThread();
        usbDriver_.close();

        // Texture caches must be torn down before the Renderer that owns
        // their VkImages/descriptor sets.
        clearGridArtTexCache();
        if (trackPanelArtTex_ != kInvalidTexture) renderer_->destroy_texture(trackPanelArtTex_);
        if (transportArtTex_ != kInvalidTexture) renderer_->destroy_texture(transportArtTex_);
        if (artistImgTex_ != kInvalidTexture) renderer_->destroy_texture(artistImgTex_);

        // Destroy the Vulkan swapchain/surface while hwnd_ is still valid,
        // rather than waiting for PlayerWindow's own destructor (which runs
        // after DestroyWindow has fully torn down the HWND).
        renderer_.reset();
        vkSurface_.reset();

        if (thumbBitmap_) { DeleteObject(thumbBitmap_); thumbBitmap_ = nullptr; }

        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

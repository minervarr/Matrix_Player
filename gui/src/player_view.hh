#pragma once
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <deque>
#include <cstdint>
#include <memory>
#include "core/library.h"
#include "core/decoder.h"
#include "core/db.h"
#include "art_view.hh"
#include "audio_output.h"
#ifdef _WIN32
#include "wasapi_output.hh"
#endif
#include "core/eq_profiles.h"
#include "core/eq_manager.h"
#include "color.hh"
#include "layout_rect.hh"
#include "host.hh"
#include "hotkey_ids.hh"
#include "keys.hh"
#include "renderer.hh"
#include "canvas.hh"
#include "texture.hh"
#include "font.hh"
#include "msdf.hh"
#include "responsive_text.hh"
#include "ui_min_text_size.gen.h"

// Theme colors
static constexpr ColorRef CLR_BG_MAIN        = RGB(10, 10, 10);
static constexpr ColorRef CLR_BG_SIDEBAR     = RGB(18, 18, 18);
static constexpr ColorRef CLR_BG_TRANSPORT   = RGB(22, 22, 22);
static constexpr ColorRef CLR_BG_TRACKPANEL  = RGB(14, 14, 14);
static constexpr ColorRef CLR_TEXT_PRIMARY    = RGB(242, 242, 242);
static constexpr ColorRef CLR_TEXT_SECONDARY  = RGB(128, 128, 128);
// 140 (was 80): rgb(80) on the rgb(10) background was ~2.4:1 contrast —
// below WCAG AA's 4.5:1 for the real information it carries (quality badge,
// REF EQ, hints). 140 ≈ 5.6:1, still clearly de-emphasized vs SECONDARY.
static constexpr ColorRef CLR_TEXT_DIM        = RGB(140, 140, 140);
static constexpr ColorRef CLR_ACCENT          = RGB(0, 200, 83);
static constexpr ColorRef CLR_HOVER           = RGB(38, 38, 38);
static constexpr ColorRef CLR_SEPARATOR       = RGB(36, 36, 36);
static constexpr ColorRef CLR_SEEKBAR_TRACK   = RGB(55, 55, 55);
static constexpr ColorRef CLR_SEEKBAR_FILL    = RGB(0, 200, 83);
static constexpr ColorRef CLR_TILE_PLACEHOLDER = RGB(28, 28, 28);
static constexpr ColorRef CLR_TEXT_ALBUM_TITLE = RGB(255, 255, 255);

// ── UI text sizing: window-relative, floored at a geometric minimum ────────
//
// Text sizes are percentages of the window's content height (not fixed
// pixel literals) so they scale with the actual window on any monitor —
// 1080p, 4K, or a tiny TV — instead of silently shrinking below legibility
// on a small window/display. kMinReadableTextSizePx (ui_min_text_size.gen.h,
// generated at build time by the min_text_size tool from the shipped fonts'
// own outline geometry — see its header comment) is the hard floor: below
// it, the font's strokes are objectively too thin for this antialiased
// renderer to represent well, not a matter of taste.
//
// Percentages are calibrated against kReferenceWindowHeight, the app's
// original default client height (CreateWindowExW's 1200x700 outer size,
// standard WS_OVERLAPPEDWINDOW chrome), so appearance at that size is
// unchanged from before this system existed.
static constexpr float kReferenceWindowHeight = 661.0f;

// Compressed upward from the original 9-16px scale: every size in that scale
// sat below kMinReadableTextSizePx (18.29px) at kReferenceWindowHeight, which
// meant enforcing the floor via minimum window size alone required a
// ~1343px-tall window — the app refused to even launch on a 1920x1080
// monitor. The 9px "badge" role (REF EQ/BITPERFECT) was a standalone outlier
// smaller than everything else; merged into the same size as "secondary"
// here so there's one smallest role instead of two, and the whole scale
// shifted up so that role clears the floor at a window size that actually
// fits common displays (see kMinWindowContentH below).
// Retuned (was 15–20/661): at 1080p the old scale produced 24–33px type —
// nearly double the title-to-tile ratio of reference players (Roon,
// Audirvana sit near 14px titles on ~200px tiles). These values land at
// ~19–28px @1080p, floored at kMinReadableTextSizePx for MSDF quality.
static constexpr float kTextSizeBadgePct           = 11.5f / kReferenceWindowHeight;  // REF EQ / BITPERFECT badge
static constexpr float kTextSizeSecondaryPct       = 11.5f / kReferenceWindowHeight;  // artist/secondary/time/duration
static constexpr float kTextSizeBodyPct            = 12.5f / kReferenceWindowHeight;  // grid album title, track number
static constexpr float kTextSizeNavPct             = 13.0f / kReferenceWindowHeight;  // nav items, settings rows, track title
static constexpr float kTextSizeTransportTitlePct  = 14.0f / kReferenceWindowHeight;  // now-playing title
static constexpr float kTextSizeTrackPanelTitlePct = 16.0f / kReferenceWindowHeight;  // track panel album name
static constexpr float kTextSizeHeaderPct          = 17.0f / kReferenceWindowHeight;  // "Settings" page header

// The smallest role (badge/secondary, tied) hits the floor first as the
// window shrinks, so it determines the minimum content height Complete
// mode's fixed window size must be at least as tall as (see UiMode below).
static constexpr float kMinWindowContentH = kMinReadableTextSizePx / kTextSizeBadgePct;

// Sanity check on the calibration itself (font choice / EM / percentile in
// min_text_size), independent of any specific window size: if a future font
// swap or recalibration blows this up (as the very first, uncalibrated
// attempt did — it computed a ~3400px minimum), fail the build immediately
// instead of silently shipping a window-size requirement no real monitor
// can satisfy.
static_assert(kMinWindowContentH <= 2000.0f,
    "Computed minimum window content height is implausibly large - check "
    "the font/EM/percentile calibration in min_text_size. See CMakeLists.txt's "
    "generate_ui_min_text_size step and ui_min_text_size.gen.h.");

struct UiTextSizes {
    float badge, secondary, body, nav, transportTitle, trackPanelTitle, header;
};

// UiMode itself is declared in host.hh (Host's window-sizing methods need it
// too): Complete = today's full browsing UI, true fullscreen; Essential = a
// minimal "now playing" widget for monitors too short for Complete.

class PlayerWindow {
public:
    bool create();
    void run();

    // Host callbacks — public because Host (a separate object, not a
    // PlayerWindow subclass) dispatches into these directly, the same way
    // handleMsg used to before it moved into os/windows_host.cc /
    // os/linux_host.cc. Not part of the app's own conceptual API.
    void onHostResized();          // an explicit UI-mode/monitor change: notifyResized() + relayout
    void onHostLayoutInvalidated(); // routine resize notification (no notifyResized(), see .cc)
    void onHostExposed();          // window newly visible/uncovered — just mark a frame dirty
    void onKeyDownPortable(int keyCode);      // key::* space (keys.hh) — shared key handling
    void onCharPortable(uint32_t codepoint);  // search-box text entry
    void onHotkey(int hotkeyId);              // Alt+F/J/C/U/G/H/L — see hotkey_ids.hh
    void adaptToCurrentMonitor();             // WM_DISPLAYCHANGE/WM_WINDOWPOSCHANGED re-fit
    void shutdown();               // teardown before the window/renderer die (was WM_DESTROY)

    // Mouse — dispatched from Host's input translation.
    void onMouseMove(int x, int y);
    void onMouseLeave();
    void onLButtonDown(int x, int y);
    void onLButtonDblClk(int x, int y);
    void onMouseWheel(int x, int y, int delta);

    void onTimer();  // periodic playback-position tick, see host_->startTimer()

    // Cross-thread completions, delivered via host_->postAppEvent() from a
    // background thread and dispatched back here on the UI thread by Host.
    void applyTrackMetadata(int album, int track);
    void onScanDone();
    void onArtDecoded();
    void startBackgroundScan();
    void onPlay();

private:
    // Actions
    void onAddFolder();
    void onManageFolders();
    void onAudioSettings();
    void prepareNextTrack();
    void startGaplessCoordinator(PcmS32Callback cbI32, int outSr, int dacCh);
    void onAlbumSelected(int idx);
    void onTrackSelected(int idx);
    void onStop();
    void onNext();
    void onPrev();
    void onSeek(int posMs);
    void onArtClick();
    void onEqSettings();
    void toggleBitperfectMode();
    void setupWatchers();
    std::string getActiveDeviceKey();
    void applyDeviceEq(int sampleRate, int channels);

    // Layout
    void recalcLayout();

    // UI mode (Essential/Complete) — see UiMode's comment in host.hh.
    void toggleUiMode();
    int  essentialHitTest(int x, int y) const;  // -1 none, else EssentialBtn index
    void snapToEdge(int hotkeyId);  // Alt+F/J/C/U/G/H — thin wrapper over host_->snapToEdge()

    // Vulkan rendering (vk_canvas). Constructed in create() once the host
    // window exists; drawFrame() is called from run() only while a frame is
    // pending (see markDirty()/pendingFrames_), instead of relying on a
    // paint/expose event.
    void drawFrame();

    // Dirty-flag render-on-demand: markDirty() arms enough pending frames to
    // reach every swapchain image (so nothing skipped shows stale content);
    // run() only calls drawFrame() while pendingFrames_ > 0, and otherwise
    // blocks instead of busy-spinning. invalidate() additionally pokes the
    // host in case the platform needs an explicit repaint request.
    void markDirty();
    void invalidate();

    // Art cache (Vulkan texture path, via stb_image — used by drawFrame())
    TextureHandle getGridArtTexture(int albumIdx);
    void          clearGridArtTexCache();
    void          loadTrackPanelArtTexture(int albumIdx);
    void          loadTransportArtTexture(const std::string& artPath);

    // Full-page album view (replaces the old right-side track panel):
    // openAlbumView() flips into it and loads everything it shows;
    // loadAlbumViewContent() resolves the sidecar files next to the music —
    // album description in the album folder, artist bio + artist image in
    // the artist folder above it — per the downloader's fixed layout.
    void openAlbumView(int albumIdx);
    void loadAlbumViewContent(int albumIdx);

    // Helpers
    // Multi-script text support: Latin Modern (the only font baked into
    // msdfFont_ otherwise) has no Cyrillic/Greek/CJK/Hangul/Kana glyphs, so
    // those scripts would render as blank/missing text (see the IC3PEAK
    // Cyrillic titles bug). Bakes Cyrillic+Greek plus any other non-Latin-1
    // codepoints in albums_' scanned metadata from a bundled fallback chain
    // that's all serif, matching Latin Modern's look instead of jumping to
    // a system UI font: New Computer Modern (Cyrillic/Greek/Latin Extended)
    // -> Fandol Song (Chinese) -> Haranoaji Mincho (Japanese) -> Un Batang
    // (Korean). No Windows system fonts involved. Safe/cheap to call
    // repeatedly — already-covered codepoints are skipped (see
    // MsdfFont::bakeCodepoints).
    // Returns true if anything new was baked (caller must then re-save the
    // MSDF cache and re-run renderer_->initMsdf() to push the grown atlas).
    bool bakeFallbackGlyphs();
    int  gridHitTest(int x, int y) const;
    int  trackPanelHitTest(int x, int y) const;
    int  sidebarHitTest(int x, int y) const;
    int  transportBtnHitTest(int x, int y) const;
    int  settingsHitTest(int x, int y) const;

    // Real window/monitor/message-pump handle — see host.hh.
    std::unique_ptr<Host> host_;

    // Transport/UI icons are drawn as native vector shapes (Canvas
    // triangle/rect/segment) directly in drawFrame() — no SVG rasterization,
    // no textures, crisp at any size. See drawUiIcon() in player_view.cc.

    // UI mode state
    UiMode  uiMode_ = UiMode::Complete;
    // (Essential/Complete toggle is keyboard-only: Alt+L. No on-screen button.)

    // Essential-mode layout zones (see toggleUiMode())
    LayoutRect rcEssentialArt_      = {};
    LayoutRect rcEssentialTitle_    = {};
    LayoutRect rcEssentialPrev_     = {};
    LayoutRect rcEssentialPlayStop_ = {};
    LayoutRect rcEssentialNext_     = {};
    int  hoverEssentialBtn_   = -1;  // 0=prev,1=playStop,2=next

    // Layout zones
    LayoutRect rcSidebar_    = {};
    LayoutRect rcGrid_       = {};
    LayoutRect rcTrackPanel_ = {};
    LayoutRect rcTransport_  = {};

    // Transport sub-regions. rcBtnPlay_ is the combined play/stop toggle —
    // there is deliberately no pause and no seek anywhere (this user only
    // ever stops or starts from zero). rcDspBadge_ is written by drawFrame()
    // (its width is the measured badge text) and read by onMouseMove() to
    // reveal the full signal-path readout on hover.
    LayoutRect rcTransportArt_  = {};
    LayoutRect rcTransportInfo_ = {};
    LayoutRect rcBtnPrev_       = {};
    LayoutRect rcBtnPlay_       = {};
    LayoutRect rcBtnNext_       = {};
    LayoutRect rcDspBadge_      = {};

    // Sidebar items
    LayoutRect rcBrand_       = {};
    LayoutRect rcNavAlbums_   = {};
    LayoutRect rcNavSettings_ = {};

    // Settings page items
    LayoutRect rcSettingsAddFolder_    = {};
    LayoutRect rcSettingsManage_       = {};
    LayoutRect rcSettingsAudio_        = {};
    LayoutRect rcSettingsEq_           = {};
    LayoutRect rcSettingsBitperfect_   = {};

    // Grid state. gridTileSize_/gridArtSize_ are recomputed every recalcLayout()
    // from a fixed target column count and the available width (see
    // recalcLayout()) — these defaults only seed the very first layout pass.
    //
    // gridRowGap_: vertical gap below a tile's art reserved for its text
    // block — two title lines plus the artist line. Computed per layout
    // pass from the ACTUAL text sizes: the old fixed 64px was calibrated
    // for the reference window and overlapped the next row's art on taller
    // monitors where text scales up (artist descenders clipped by the art
    // below — the 1920x1200 "Anaima y" bug).
    //
    // uiScale_: the one proportion factor for every remaining fixed-pixel
    // layout value (sidebar rows, transport bar, paddings). Same ratio the
    // text already scales by, so text and the chrome around it keep the
    // reference layout's proportions on any monitor/resolution.
    int   gridRowGap_ = 64;
    float uiScale_    = 1.0f;   // = textSizes_.nav / 13.0f, see recalcLayout()
    int gridScrollY_     = 0;
    int gridTileSize_    = 180;
    int gridArtSize_     = 150;
    int gridCols_        = 1;
    int gridTotalHeight_ = 0;
    int gridPadX_        = 24;
    int gridPadY_        = 16;

    // Sidebar search box — live-filters the album grid. gridIndices_ is the
    // single indirection: tile position → albums_ index. Draw loop and
    // gridHitTest() both go through it, so every click/hover consumer keeps
    // receiving real album indices whether or not a filter is active.
    LayoutRect       rcSearch_ = {};
    std::string      searchQuery_;       // UTF-8
    bool             searchFocused_ = false;
    std::vector<int> gridIndices_;
    void rebuildGridIndices();

    // Album view scroll + hit-test anchors. trackListTop_/Left_/Right_ are
    // written by drawFrame() (the album page lays itself out while drawing,
    // same pattern as rcDspBadge_) and read by trackPanelHitTest();
    // trackListTop_ is the window-space Y row 0 would have at scroll 0.
    // albumViewContentH_ bounds onMouseWheel()'s scroll. The view closes
    // via Escape only — no on-screen close button.
    int trackScrollY_   = 0;
    int trackRowHeight_  = 40;
    int  trackListTop_   = 0;
    int  trackListLeft_  = 0;
    int  trackListRight_ = 0;
    int  albumViewContentH_ = 0;

    // Album view sidecar content (loaded by loadAlbumViewContent()).
    // *Lines_ are the word-wrapped render caches, rebuilt by drawFrame()
    // whenever the wrap width changes (albumTextWrapW_ tracks it).
    std::string              albumDescText_;
    std::string              artistBioText_;
    std::vector<std::string> albumDescLines_;
    std::vector<std::string> artistBioLines_;
    float                    albumTextWrapW_ = -1.0f;
    TextureHandle            artistImgTex_ = kInvalidTexture;

    // Hover state
    int hoverAlbumIdx_      = -1;
    int hoverTrackIdx_      = -1;
    int hoverSidebarItem_   = -1;
    int hoverTransportBtn_  = -1;
    int hoverSettingsItem_  = -1;
    bool hoverDspBadge_     = false;

    // Selection / navigation
    int  selectedAlbumIdx_  = -1;
    bool trackPanelOpen_    = false;
    int  activeNavItem_     = 0;  // 0=Albums, 1=Settings

    // Last-played album (persisted via Db::saveSetting/loadSetting, matched
    // by name+artist rather than index since the list reorders across
    // rescans) — draws a dim accent indicator on its grid tile even before
    // anything has played this session. See onPlay() and create().
    std::string lastPlayedAlbumName_;
    std::string lastPlayedArtistName_;

    // Now-playing display state (UTF-8; Canvas::text() wants UTF-8 directly,
    // so there's no wide-string round-trip here as there was pre-reorg).
    std::string currentTitle_;
    std::string currentArtist_;
    int          seekPosMs_   = 0;
    int          seekTotalMs_ = 0;

    // Art caches (Vulkan textures, used by drawFrame())
    std::unordered_map<int, TextureHandle> gridArtTexCache_;
    TextureHandle trackPanelArtTex_      = kInvalidTexture;
    int           trackPanelArtTexAlbum_ = -1;
    TextureHandle transportArtTex_       = kInvalidTexture;
    std::string   transportArtTexPath_;

    // Async grid-art decode: JPEG/PNG decode used to run synchronously inside
    // drawFrame() (getGridArtTexture()), so revealing a new grid row while
    // scrolling stalled the UI thread for a whole row of decodes. Decodes now
    // run on a worker thread; the main thread only uploads finished RGBA to a
    // Vulkan texture when notified (see onHostArtDecoded()) (create_texture
    // must stay on the render thread). `gen` guards against stale results
    // landing after a rescan reshuffled album indices; artDecodePending_ is
    // main-thread-only.
    struct ArtDecodeResult { int albumIdx; std::vector<uint8_t> rgba; int w = 0, h = 0; uint64_t gen = 0; };
    struct ArtDecodeJob    { int albumIdx; std::string path; int targetSize; uint64_t gen; };
    std::thread                 artDecodeThread_;
    std::mutex                  artDecodeMu_;
    std::condition_variable     artDecodeCv_;
    std::deque<ArtDecodeJob>    artDecodeQueue_;
    std::vector<ArtDecodeResult> artDecodeDone_;
    bool                        artDecodeQuit_ = false;   // guarded by artDecodeMu_
    std::atomic<uint64_t>       artCacheGen_{0};
    std::unordered_map<int, char> artDecodePending_;
    void artDecodeWorker();
    void stopArtDecodeThread();

    // Bound the grid-art VRAM footprint on large libraries: scrolling through
    // thousands of albums used to pin every tile's texture forever. Evict
    // least-recently-drawn tiles once past the cap (~250 KB per 250px tile);
    // evicted tiles just re-decode on the worker if scrolled back to.
    // 96 ≈ 2-3 screens of tiles (a 1080p fullscreen grid shows ~30-40) —
    // was 256, which let the cache balloon to ~50 MB of dedicated VRAM.
    static constexpr size_t kMaxGridArtTextures = 96;
    std::unordered_map<int, uint64_t> gridArtLastUse_;
    uint64_t artUseTick_ = 0;

    // Library + playback (unchanged from original)
    //
    // albumsMu_ guards albums_ itself: onScanDone() reassigns it wholesale
    // on the UI thread when a rescan completes, while prepareNextTrack()
    // reads it both from the UI thread (via onPlay()) and from the
    // background gaplessThread_ (see startGaplessCoordinator()) — without
    // this lock a rescan landing mid-gapless-preload is a use-after-free on
    // the vector's backing storage.
    std::mutex albumsMu_;
    std::vector<Album> albums_;
    int  currentAlbum_ = -1;
    int  currentTrack_ = -1;

    Decoder          decoder_;
    Decoder          nextDecoder_;
    Decoder*         active_    = &decoder_;

    int              nextAlbum_ = -1;
    int              nextTrack_ = -1;

    std::thread              gaplessThread_;
    std::mutex               gaplessMu_;
    std::condition_variable  gaplessCv_;
    bool                     gaplessSignal_  = false;
    std::atomic<bool>        stopGapless_{false};
    Db               db_;
    ArtWindow        artWin_;
    UsbAudioDriver   usbDriver_;
    bool             usbOpen_  = false;

    std::unique_ptr<AudioOutput> output_;
    bool             useWasapi_     = false;
    std::atomic<bool> bitperfectMode_{false};
#ifdef _WIN32
    std::wstring     wasapiDeviceId_;
    WasapiMode       wasapiMode_    = WasapiMode::Shared;
#endif

    // True playback position is "frames written to the output minus the output's
    // still-pending buffer". playedFrames_ is a session-monotonic count of
    // OUTPUT-rate frames written to the current output stream (reset only on a
    // fresh stream / seek, NOT on a seamless gapless swap). displayTrackStartFrame_
    // marks where the currently-DISPLAYED track began on that timeline.
    std::atomic<int64_t> playedFrames_{0};
    int64_t displayTrackStartFrame_ = 0;

    // currentAlbum_/currentTrack_ are the DECODE/navigation cursor (advance as
    // soon as the next decoder starts). displayAlbum_/displayTrack_ are the
    // now-playing track shown to the user; they flip when the DAC actually
    // crosses a boundary. boundaries_ queues pending flips (front = soonest);
    // a track shorter than the output buffer can stack more than one.
    int displayAlbum_ = -1;
    int displayTrack_ = -1;
    struct TrackBoundary { int64_t frame; int album; int track; };
    std::deque<TrackBoundary> boundaries_;
    std::mutex boundariesMu_;

    std::vector<float> resampleBuf_;
    std::vector<float> upmixBuf_;

    EqProfileStore   eqProfiles_;
    EqManager        eqManager_;

    FolderWatcher        watcher_;
    std::thread          scanThread_;
    std::mutex           scanMu_;
    std::vector<Album>   scanResult_;
    std::atomic<bool>    scanning_{false};

    bool isPlaying_ = false;
    bool mouseTracking_ = false;

    // Vulkan rendering (vk_canvas). The concrete SurfaceProvider/AssetReader
    // (Win32SurfaceProvider+FileAssetReader vs WaylandSurfaceProvider+
    // FileAssetReader) live inside host_ — Renderer only ever sees the
    // portable base-class references vk_canvas itself defines
    // (core/platform.hh), so no per-platform type appears here.
    std::unique_ptr<Renderer>             renderer_;
    std::vector<float>                    frameCurves_;
    std::vector<float>                    frameShapes_;  // SDF shape quads (Canvas::useShapes)
    std::vector<ImageDraw>                frameImages_;
    std::vector<ImageDraw>                frameImagesFg_;
    bool                                  running_ = true;
    uint32_t                              pendingFrames_ = 0;

    // UI text font (Latin Modern Sans, FreeType-decomposed curves through the
    // same vector pipeline as everything else) — replaces the built-in
    // stroke-fallback glyphs (font=nullptr), which mis-rendered some digit
    // pairs (e.g. "00").
    Font uiFont_;

    // MSDF font: pre-baked distance-field atlas for crisp text at any size
    // without per-frame Bézier decomposition. Generated once at startup from
    // the OTF font and cached to disk for subsequent runs.
    MsdfFont             msdfFont_;
    std::vector<float>   msdfQuads_;
    // Set once in create(); reused by onScanDone() to re-save the cache
    // (now possibly including fresh fallback-script glyphs) after a rescan.
    std::string          msdfCachePath_;
    // Set once in create(); exe-relative "fonts/" dir, reused by
    // bakeFallbackGlyphs() to find the bundled fallback-script font files.
    std::string          fontsDir_;

    // Computed once per recalcLayout() call from the window's current
    // content height and the *Pct constants above.
    UiTextSizes textSizes_{};
};

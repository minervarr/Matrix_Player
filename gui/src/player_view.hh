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
#include "core/variants.h"
#include "core/decoder.h"
#include "core/db.h"
#include "core/streamer_db.h"
#include "art_view.hh"
#include "audio_output.h"
#ifdef _WIN32
#include "wasapi_output.hh"
#else
#ifdef MATRIX_HAVE_ALSA
#include "os/alsa_output.hh"
#endif
#ifdef MATRIX_HAVE_JACK
#include "os/jack_output.hh"
#endif
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
#include "widgets.hh"
#include "texture.hh"
#include "font.hh"
#include "msdf.hh"
#include "theme.hh"
#include "ui_metrics.hh"
#include "panels/settings_panels.hh"

// ── UI text sizing and geometry scale ──────────────────────────────────────
//
// Both now live in ui_metrics.hh: one factor anchored at the app's real render
// height (1080), with the smallest type role pinned to the font's own
// legibility floor and every other role derived from it by a fixed ratio. That
// replaces the seven independently hand-tuned percentages that used to live
// here — every one of which sat below the floor at their own 661px reference,
// so the hierarchy they described only partly existed on screen.
//
// See ui_metrics.hh's header comment and
// docs/superpowers/specs/2026-07-28-ui-design-system-rigor-pass-design.md.

// The smallest role IS the floor, so the minimum content height at which the
// scale is not clamped is exactly the reference height. Complete mode's window
// sizing must be at least this tall (see UiMode in host.hh); below it the whole
// scale clamps uniformly rather than distorting.
static constexpr float kMinWindowContentH = kUiReferenceHeight;

// UiMode itself is declared in host.hh (Host's window-sizing methods need it
// too): Complete = today's full browsing UI, true fullscreen; Essential = a
// minimal "now playing" widget for monitors too short for Complete.

// Output backend selection (Audio Settings panel / db "audio_backend" key).
// Usb is primary/bit-perfect on both platforms; Wasapi is Windows' secondary
// backend, Alsa/Jack are Linux's — mirroring WASAPI's role there (see
// CLAUDE.md's design-decisions table).
enum class AudioBackend { Usb, Wasapi, Alsa, Jack };

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
    // `cause` is what gets recorded in the listening log — see StartCause in
    // core/stats.h. Defaulted so the many ordinary call sites (a click on a
    // track, the transport button) need say nothing; only the paths that are
    // NOT the listener choosing pass something else.
    void onPlay(StartCause cause = StartCause::Manual);

private:
    // Actions
    void onAddFolder();
    void onManageFolders();
    void onAudioSettings();
    // Decodes an artist photo at its on-screen size, mip-free — see the
    // comment on the definition for why both halves matter.
    TextureHandle loadArtistImageTexture(const std::string& path);
    void prepareNextTrack();
    // The album that plays after / before `album`: the next one of the SAME
    // release type, which is the next tile in the section on screen —
    // rebuildGridIndices() filters albums_ by that same field in that same
    // order. Anchoring on the PLAYING album's own type instead of on
    // albumTypeFilter_ means browsing to another section mid-track cannot
    // re-route the queue. Albums with no tracks are skipped. Returns -1 at the
    // section's edge, where playback stops. **Caller must hold albumsMu_.**
    int  nextAlbumInSection(int album) const;
    int  prevAlbumInSection(int album) const;
    void startGaplessCoordinator(PcmS32Callback cbI32, int outSr, int dacCh);
    void onAlbumSelected(int idx);
    void onTrackSelected(int idx);
    void onStop();
    void onNext();
    void onPrev();
    void onSeek(int posMs);

    // Listening stats follow the DISPLAY cursor, not the decode cursor: they
    // describe what the DAC actually rendered. beginTrackStats() logs the play
    // and starts counting; flushTrackStats() banks the listen time and decides
    // whether it counted as a skip. statsPath_ empty = nothing being counted.
    // Pointer image for a point, from the same rects the hover hit-tests use.
    // Hand only where a click actually does something — a hand over inert
    // chrome is a promise the UI doesn't keep.
    CursorShape cursorForPoint(int x, int y) const;
    void        applyCursorFor(int x, int y);
    CursorShape lastCursor_ = CursorShape::Arrow;
    bool        keepAwake_  = false;   // mirrors the art window, see run()

    void beginTrackStats(const Track& t, StartCause cause);
    void flushTrackStats(EndCause cause);
    void accrueListenTime();
    void savePlaybackStateNow();
    std::string statsPath_;
    std::string statsKey_;
    int         statsDurationMs_ = 0;
    // The open play_events row, 0 when nothing is being counted.
    int64_t     statsEventId_ = 0;
    // Honest listen time: onTimer() adds each plausible forward step of the
    // play position and ignores anything that looks like a seek. Summing the
    // steps rather than reading the final position is what makes a skip
    // forward stop counting as time heard, and a rewind-and-replay count the
    // replayed stretch twice. statsLastPosMs_ < 0 = no previous sample yet.
    int64_t     statsMsHeard_   = 0;
    int         statsLastPosMs_ = -1;
    // Cause to stamp on the play the gapless coordinator is about to start.
    // Gapless by default — the ordinary case is one track ending and the next
    // following it — but onNext()'s seamless path routes the listener's own
    // choice through the very same code, and that is a different fact about
    // the music. Consumed and reset by applyTrackMetadata(). UI thread only.
    StartCause  gaplessStartCause_ = StartCause::Gapless;
    // Set by the gapless coordinator THREAD before it asks the UI thread to
    // restart playback because the next track needs a different device format
    // (see startGaplessCoordinator). That restart reaches onPlay() and would
    // otherwise be indistinguishable from the listener pressing play.
    std::atomic<bool> playFromGapless_{false};
    // Position from the previous session, applied once by the first onPlay()
    // that opens the very file it was saved for. 0 = nothing to resume.
    int         pendingResumeMs_ = 0;
    std::string pendingResumePath_;
    void onArtClick();
    void onEqSettings();
    void toggleBitperfectMode();
    void setupWatchers();
    std::string getActiveDeviceKey();
    std::string audioBackendLabel() const;
    void applyDeviceEq(int sampleRate, int channels);

    // ── Settings panels (Phase 7) — vk_canvas-native replacements for the
    // four native dialogs, identical on both platforms. See
    // panels/settings_panels.hh for the shared row-list/button/header
    // widgets these draw with.
    void closeActivePanel();
    void onPanelMouseMove(int x, int y);
    void onPanelClick(int x, int y);
    void onPanelWheel(int x, int y, int delta);
    bool onPanelKeyDown(int keyCode);   // true = consumed (a panel is open)
    void onPanelChar(uint32_t codepoint);
    void drawActivePanel(Canvas& canvas, const LayoutRect& area);

    void drawManageFolders(Canvas& canvas, const LayoutRect& area);

    void drawAudioSettings(Canvas& canvas, const LayoutRect& area);
    void applyAudioSettingsPanel();

    void drawEqSettings(Canvas& canvas, const LayoutRect& area);
    void eqRefilter();

    void drawFolderPicker(Canvas& canvas, const LayoutRect& area);
    void fpLoadDir(const std::string& dir);
    void commitAddFolder(const std::string& root);

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
    // sizeClass picks which decode of the cover to hand back: ArtFull for a
    // whole tile, ArtHalf for a mosaic quadrant. Both are cached separately.
    TextureHandle getGridArtTexture(int albumIdx, int sizeClass = ArtFull);
    void          clearGridArtTexCache();
    void          loadTrackPanelArtTexture(int albumIdx);
    void          loadTransportArtTexture(const std::string& artPath);

    // Full-page album view (replaces the old right-side track panel):
    // openAlbumView() flips into it and loads everything it shows;
    // loadAlbumViewContent() resolves artist bio + artist image, preferring
    // a sibling .streamer/library.db (see streamerDbs_/rootForPath()) keyed
    // by the album's folder name, and falling back to the legacy sidecar-
    // file convention (bio.* + an image loose in the artist's own folder,
    // one level above the album folder) when no such database is found.
    void openAlbumView(int albumIdx);
    void loadAlbumViewContent(int albumIdx);
    std::string rootForPath(const std::string& path) const;

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
    // MTSDF cache and re-run renderer_->initMsdf() to push the grown atlas).
    bool bakeFallbackGlyphs();
    // Bakes the UI icon glyphs (assets/fonts/icons/matrix-icons.otf) into that
    // same atlas. Deliberately runs BEFORE bakeFallbackGlyphs(): the atlas has
    // a hard 4096px height ceiling, and the icon set is small and fixed while
    // the CJK fallback set grows with the user's library — so icons claim their
    // rows first. Same "true if anything new was baked" contract as above.
    bool bakeIconGlyphs();
    int  gridHitTest(int x, int y) const;
    int  trackPanelHitTest(int x, int y) const;
    // Album index of the "OTHER VERSIONS" thumbnail under (x,y), or -1.
    int  variantTileHitTest(int x, int y) const;
    int  sidebarHitTest(int x, int y) const;
    int  transportBtnHitTest(int x, int y) const;
    int  settingsHitTest(int x, int y) const;

    // Real window/monitor/message-pump handle — see host.hh.
    std::unique_ptr<Host> host_;

    // Transport/UI icons are glyphs in the shared MTSDF atlas, baked from
    // assets/fonts/icons/matrix-icons.otf — real curves, tinted at draw time,
    // no extra GPU pass and no texture uploads. See ui_icons.hh, and
    // drawUiIcon() in player_view.cc for the primitive fallback used when the
    // icon font is missing.

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
    // Non-modal bitperfect-mismatch warning strip, drawn above the transport
    // bar when bitperfectWarning_ is non-empty. See draw()/onLButtonDown().
    LayoutRect rcBitperfectWarning_ = {};

    // Sidebar items
    LayoutRect rcBrand_       = {};
    LayoutRect rcNavAlbum_    = {};
    LayoutRect rcNavEp_       = {};
    LayoutRect rcNavSingle_   = {};
    LayoutRect rcNavRemix_    = {};
    LayoutRect rcNavSettings_ = {};

    // Settings page items
    LayoutRect rcSettingsAddFolder_    = {};
    LayoutRect rcSettingsManage_       = {};
    LayoutRect rcSettingsAudio_        = {};
    LayoutRect rcSettingsEq_           = {};
    LayoutRect rcSettingsBitperfect_   = {};

    // ── Settings panels state (Phase 7) ──────────────────────────────────
    SettingsPanel activePanel_ = SettingsPanel::None;
    static constexpr int kPanelRowH = 44;

    // Manage Folders
    std::vector<std::string> mfRoots_;
    int  mfHoverRow_    = -1;
    int  mfSelectedRow_ = -1;
    int  mfScrollY_     = 0;
    bool mfChanged_     = false;
    LayoutRect mfListArea_ = {}, mfCloseRc_ = {}, mfBtnRemove_ = {}, mfBtnDone_ = {};
    bool mfHoverClose_ = false, mfHoverRemove_ = false, mfHoverDone_ = false;
    std::vector<widgets::ListRow> mfListRows_;  // cached during draw, read by hit-test

    // Audio Settings. asBackendOptions_ lists whichever backends this build
    // actually has (USB always; WASAPI on Windows; ALSA/JACK on Linux, each
    // only if audio_engine found the library — see MATRIX_HAVE_ALSA/_JACK),
    // so row indices never need per-platform special-casing at the call site.
    std::vector<AudioBackend> asBackendOptions_;
    int  asBackendSelIdx_   = 0;
    std::vector<LayoutRect> asBackendRowRects_;  // parallel to asBackendOptions_
    int  asHoverBackendRow_ = -1;
    std::vector<UsbAudioDeviceInfo> asUsbDevices_;
    int  asUsbSel_      = -1;
    int  asHoverDeviceRow_ = -1;
    int  asDeviceScrollY_ = 0;
    LayoutRect asDeviceListArea_ = {};
    std::vector<widgets::ListRow> asDeviceListRows_;  // cached during draw, read by hit-test
#ifdef _WIN32
    std::vector<WasapiDeviceInfo> asWasapiDevices_;  // index 0 shown as "(Default device)"
    int  asWasapiSel_   = 0;
    bool asExclusive_   = false;
    LayoutRect asModeRows_[2] = {};
    int  asHoverModeRow_ = -1;
#else
#ifdef MATRIX_HAVE_ALSA
    std::vector<AlsaDeviceInfo> asAlsaDevices_;  // index 0 shown as "(System default)"
    int  asAlsaSel_     = 0;
#endif
#ifdef MATRIX_HAVE_JACK
    std::vector<JackPlaybackPortInfo> asJackPorts_;  // index 0 shown as "(Auto-connect)"
    int  asJackSel_     = 0;
#endif
#endif
    LayoutRect asCloseRc_ = {}, asBtnApply_ = {};
    bool asHoverClose_ = false, asHoverApply_ = false;

    // EQ Settings
    std::string eqSearch_;
    bool eqSearchFocused_ = false;
    std::vector<int> eqFilteredIndices_;
    int  eqHoverRow_    = -1;
    int  eqSelectedRow_ = -1;
    int  eqScrollY_     = 0;
    std::string eqDeviceKey_;
    bool eqBitperfectActive_ = false;
    LayoutRect eqSearchRc_ = {}, eqListArea_ = {}, eqCloseRc_ = {}, eqBtnAssign_ = {}, eqBtnClear_ = {};
    bool eqHoverClose_ = false, eqHoverAssign_ = false, eqHoverClear_ = false;
    std::vector<widgets::ListRow> eqListRows_;  // cached during draw, read by hit-test

    // Folder picker (also reached via "Add Music Folder")
    std::string fpCurrentDir_;
    std::vector<std::string> fpEntries_;   // subfolder names only, sorted
    bool fpHasParent_  = false;
    int  fpHoverRow_   = -1;
    int  fpScrollY_    = 0;
    LayoutRect fpListArea_ = {}, fpCloseRc_ = {}, fpBtnSelect_ = {}, fpBtnCancel_ = {};
    bool fpHoverClose_ = false, fpHoverSelect_ = false, fpHoverCancel_ = false;
    std::vector<widgets::ListRow> fpListRows_;  // cached during draw, read by hit-test

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
    int   gridRowGap_ = 64;
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

    // Album variants (see core/include/core/variants.h). The grid shows one
    // tile per GROUP — its best member — instead of one per folder, so the
    // same release held twice (Deluxe beside standard, 24/96 beside 16/44.1)
    // stops appearing as near-duplicate tiles. Derived from albums_ by a pure
    // function, never persisted: rebuildAlbumGroups() runs immediately before
    // rebuildGridIndices() everywhere albums_ is reassigned.
    std::vector<AlbumGroup> albumGroups_;
    std::vector<int>        albumGroupOf_;   // albums_ index → albumGroups_ index
    void rebuildAlbumGroups();
    // The other members of an album's group, best first, EXCLUDING the album
    // itself — what the album view lists below the artist bio. Empty when the
    // album has no siblings.
    std::vector<int> otherVariantsOf(int albumIdx) const;
    // Draws a remix group's 2x2 cover mosaic in place of the single tile
    // cover. False when this album is not a multi-member remix group's
    // primary, meaning the caller should draw the ordinary cover.
    bool drawVariantMosaic(Canvas& canvas, int albumIdx,
                           float x, float y, float a);

    // Album view scroll + hit-test anchors. trackListLeft_/Right_ and
    // trackRowTop_ are written by drawFrame() (the album page lays itself out
    // while drawing, same pattern as rcDspBadge_) and read by
    // trackPanelHitTest(). albumViewContentH_ bounds onMouseWheel()'s scroll.
    // The view closes via Escape only — no on-screen close button.
    int trackScrollY_   = 0;
    int trackRowHeight_  = 40;
    int  trackListLeft_  = 0;
    int  trackListRight_ = 0;
    int  albumViewContentH_ = 0;
    // Scroll-0 window Y of each track row, one entry per album.tracks index,
    // rewritten by the album view's draw block. Rows are no longer on a fixed
    // i*trackRowHeight_ grid (a "DISC n" separator shifts everything below
    // it), so trackPanelHitTest() reads this instead of dividing.
    std::vector<int> trackRowTop_;

    // Album view sidecar content (loaded by loadAlbumViewContent()).
    // *Lines_ are the word-wrapped render caches, rebuilt by drawFrame()
    // whenever the wrap width changes (albumTextWrapW_ tracks it).
    std::string              albumDescText_;
    std::string              artistBioText_;
    std::vector<std::string> albumDescLines_;
    std::vector<std::string> artistBioLines_;
    float                    albumTextWrapW_ = -1.0f;
    TextureHandle            artistImgTex_ = kInvalidTexture;
    // Where the artist photo landed this frame, written by drawFrame() (same
    // pattern as rcDspBadge_) and read by onLButtonDown to open the viewer.
    // It is the CROPPED rect — clicking the sliver that is actually on screen
    // is what a user can aim at. Empty when the photo is off-screen.
    LayoutRect               rcArtistImg_ = {};
    // The album view's prose column (description + bio), written by
    // drawFrame() like rcArtistImg_. Not clickable and deliberately without a
    // hover background — it exists so the cursor can say "text" there.
    LayoutRect               rcAlbumText_ = {};
    // The "OTHER VERSIONS" strip below the artist bio: where each variant
    // thumbnail landed this frame and which album it stands for. Written by
    // drawFrame() and read by onLButtonDown/cursorForPoint, the same
    // draw-then-hit-test pattern as rcArtistImg_. Cleared on every album
    // switch by loadAlbumViewContent().
    std::vector<std::pair<LayoutRect, int>> rcVariantTiles_;
    std::string              artistImgPath_;   // source file, for ArtWindow
    // What the fullscreen ArtWindow is currently showing. It is one window
    // serving two jobs — album art from the transport thumbnail, artist photo
    // from the album view — and loadTransportArtTexture() has to know which,
    // so a track change refreshes the right picture.
    bool                     artWinShowsArtist_ = false;
    // Artist photo path for an album, resolved the same two ways
    // loadAlbumViewContent() resolves it (streamer db, then legacy sidecar).
    // Empty when that album has none.
    std::string artistImagePathFor(int albumIdx) const;

    // Hover state
    int hoverAlbumIdx_      = -1;
    int hoverTrackIdx_      = -1;
    // Album index (not strip position) of the "OTHER VERSIONS" thumbnail
    // under the pointer, or -1. Holding the album index means it survives the
    // strip relaying out mid-hover.
    int hoverVariantIdx_    = -1;
    int hoverSidebarItem_   = -1;
    int hoverTransportBtn_  = -1;
    int hoverSettingsItem_  = -1;
    bool hoverDspBadge_     = false;

    // Selection / navigation
    int  selectedAlbumIdx_  = -1;
    bool trackPanelOpen_    = false;
    // Sidebar is two independent things: which album TYPE is being browsed
    // (Albums/EPs/Singles/Remixes — filters the grid), and whether the
    // Settings gear is open (replaces the whole content area). They're
    // deliberately separate state: leaving Settings returns to whichever
    // type filter was active, never hardcoded back to Albums.
    enum class AlbumTypeFilter { Album, Ep, Single, Remix };
    // rebuildGridIndices() compares this against Album::ReleaseType via a
    // plain (int) cast — these static_asserts make a future reorder of
    // either enum a build error instead of a silently wrong filter.
    static_assert((int)AlbumTypeFilter::Album  == (int)Album::ReleaseType::Album,  "AlbumTypeFilter/Album::ReleaseType value mismatch");
    static_assert((int)AlbumTypeFilter::Ep     == (int)Album::ReleaseType::Ep,     "AlbumTypeFilter/Album::ReleaseType value mismatch");
    static_assert((int)AlbumTypeFilter::Single == (int)Album::ReleaseType::Single, "AlbumTypeFilter/Album::ReleaseType value mismatch");
    static_assert((int)AlbumTypeFilter::Remix  == (int)Album::ReleaseType::Remix,  "AlbumTypeFilter/Album::ReleaseType value mismatch");
    AlbumTypeFilter albumTypeFilter_ = AlbumTypeFilter::Album;
    bool            settingsOpen_    = false;
    static constexpr int kSidebarSettingsHit = 4;  // sidebarHitTest() sentinel for the Settings row

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
    //
    // Keyed by artKey(albumIdx, sizeClass), not by album index alone: the
    // mosaic tile a remix group draws (see §8.2) needs the SAME album's cover
    // at half the tile's edge, and serving that from the full-size texture
    // would minify 2:1 with no mip chain (mips are off on purpose — see
    // onArtDecoded) and alias visibly. Two size classes exist: Full (the tile)
    // and Half (a mosaic quadrant).
    enum ArtSizeClass { ArtFull = 0, ArtHalf = 1, ArtSizeClassCount = 2 };
    static constexpr int artKey(int albumIdx, int sizeClass) {
        return albumIdx * ArtSizeClassCount + sizeClass;
    }
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
    // `key` is artKey(albumIdx, sizeClass) — the worker never needs the album
    // index itself, only the cache slot the result belongs in.
    struct ArtDecodeResult { int key; std::vector<uint8_t> rgba; int w = 0, h = 0; uint64_t gen = 0; };
    struct ArtDecodeJob    { int key; std::string path; int targetSize; uint64_t gen; };
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
    // Sibling ".streamer" databases (external Qobuz-style downloader
    // metadata — see core/streamer_db.h), keyed by music-root path. Kept
    // per-root since a user may have several roots, only some of which sit
    // next to a .streamer folder; entries for roots without one just stay
    // closed (isOpen() == false).
    std::unordered_map<std::string, StreamerDb> streamerDbs_;
    ArtWindow        artWin_;
    UsbAudioDriver   usbDriver_;
    bool             usbOpen_  = false;

    std::unique_ptr<AudioOutput> output_;
    AudioBackend     audioBackend_  = AudioBackend::Usb;
    std::atomic<bool> bitperfectMode_{false};
    // Empty = hidden. Set on a bitperfect-mismatch playback failure (see
    // onPlay()); cleared at the top of the next onPlay() attempt or on
    // click. Plain string, not atomic — only touched from the UI thread.
    std::string bitperfectWarning_;

    // What the signal path ACTUALLY achieved for the playing track, as opposed
    // to what the mode toggle asked for. The DSP badge used to read
    // bitperfectMode_ directly, so it said BITPERFECT whenever the toggle was
    // on — even when the depth had been truncated or a server owned the final
    // conversion. Bit-perfect that cannot be trusted is worse than none.
    enum class BpState {
        Off,        // Reference EQ — EQ/resampling by design
        Exact,      // sample-for-sample to a device we own
        ViaServer,  // exact through our chain, but a sound server converts after us
        Degraded    // something was genuinely lost (depth truncated)
    };
    BpState     bpState_ = BpState::Off;
    std::string bpDetail_;      // one line, shown in the hover readout
#ifdef _WIN32
    std::wstring     wasapiDeviceId_;
    WasapiMode       wasapiMode_    = WasapiMode::Shared;
#else
#ifdef MATRIX_HAVE_ALSA
    std::string      alsaDeviceId_  = "default";
#endif
#ifdef MATRIX_HAVE_JACK
    std::string      jackStartPort_;
#endif
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

    // MTSDF font: pre-baked distance-field atlas for crisp text at any size
    // without per-frame Bézier decomposition. Generated once at startup from
    // the OTF font and cached to disk for subsequent runs. Also carries the UI
    // icon glyphs (see ui_icons.hh) — same atlas, same pass.
    //
    // "Mtsdf" vs "Msdf": what generate() bakes is always MTSDF (RGB multi-
    // channel field + a true single-channel SDF in alpha). The type keeps the
    // MsdfFont name because it genuinely handles both — a legacy v2 load()
    // atlas really is plain MSDF — and isMtsdf() reports which is live.
    MsdfFont             msdfFont_;
    std::vector<float>   msdfQuads_;
    // Set once in create(); reused by onScanDone() to re-save the cache
    // (now possibly including fresh fallback-script glyphs) after a rescan.
    std::string          msdfCachePath_;
    // Set once in create(); exe-relative "fonts/" dir, reused by
    // bakeFallbackGlyphs() to find the bundled fallback-script font files.
    std::string          fontsDir_;

    // Recomputed once per recalcLayout() from the window's content height.
    // metrics_.text.* are the type roles; metrics_.space()/stroke() are the
    // geometry helpers. See ui_metrics.hh.
    UiMetrics metrics_{};
};

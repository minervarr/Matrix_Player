#pragma once
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <set>
#include <deque>
#include <cstdint>
#include <memory>
#include "core/library.h"
#include "core/variants.h"
#include "core/facets.h"
#include "core/decoder.h"
#include "core/db.h"
#include "core/streamer_db.h"
#include "art_view.hh"
#include "audio_output.h"
#include "ui_orientation.hh"
#include "rail_layout.hh"
#include "bar_a.hh"
#include "img_decode.hh"
#ifdef _WIN32
#include "wasapi_output.hh"
#else
#ifdef MATRIX_HAVE_ALSA
#include "os/alsa_output.hh"
#endif
#ifdef MATRIX_HAVE_JACK
#include "os/jack_output.hh"
#endif
#ifdef MATRIX_HAVE_AAUDIO
#include "os/aaudio_output.hh"
#endif
#ifdef MATRIX_HAVE_AOAS
#include "os/aoas_output.hh"
#endif
#ifdef MATRIX_HAVE_BLUETOOTH
#include "os/bt_output.hh"
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
#include "raster_font.hh"
#include "glyph_baker.hh"
#include "theme.hh"
// The OS's own transport (Android's foreground service + MediaSession; nothing
// on the desktops). Declared here because nowPlayingForOs() returns one of its
// types; the seam itself is platform-free.
#include "media_session.hh"
// The Bluetooth A2DP codec. Not an AudioOutput and not in the audio path — a
// property of the OUTPUT ROUTE, which is why it sits beside the per-device EQ
// assignment rather than beside a backend. See bt_codec.hh.
#include "bt_codec.hh"
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
// scale is not clamped is exactly the reference height; below it the whole
// scale clamps uniformly rather than distorting.
static constexpr float kMinWindowContentH = kUiReferenceHeight;

// Output backend selection (Audio Settings panel / db "audio_backend" key).
// Usb is primary/bit-perfect on both platforms; Wasapi is Windows' secondary
// backend, Alsa/Jack are Linux's — mirroring WASAPI's role there (see
// CLAUDE.md's design-decisions table). Aoas is Android's bit-perfect relay
// backend: the phone as a CLIENT of the AOAS service, which owns the USB
// permission and the live isochronous stream and never closes either — see
// gui/src/os/aoas_output.hh and docs/superpowers/specs/2026-08-28-aoas-client-backend.md.
// Append-only: this value is persisted indirectly (db "audio_backend" stores a
// name, not an ordinal), but every switch below is written against the set.
// Appended, never reordered: getBackendKey() writes one of these to the
// database by NAME, but the settings panel's option list is indexed by
// position and a reorder would silently move a listener's saved choice.
enum class AudioBackend { Usb, Wasapi, Alsa, Jack, AAudio, Aoas, Bluetooth };

// ── The app's own host vocabularies ──────────────────────────────────────────
//
// Both were declared in host.hh until app_view.hh split the seam. They are
// this player's words, not a window system's, so they live with the player and
// travel through Host as plain integers — see the note above postAppEvent().

// Cross-thread completions our background threads (art-decode worker,
// background scan thread, gapless coordinator) need serviced on the UI thread.
// The three Request* values below RequestPlay are the OS's transport asking for
// something — a notification button, a headset, a Bluetooth remote, or a lost
// audio focus. They arrive on the platform's thread and are posted here so the
// APP thread answers them, through the same onPlay/onStop/onNext/onPrev the
// on-screen buttons call. Appended, never reordered: these travel through
// Host::postAppEvent as plain integers.
enum class AppEvent { TrackChange, ScanDone, ArtDecoded, RequestPlay,
                      RequestStop, RequestNext, RequestPrev,
                      // A Bluetooth sink connected, went away, or finished
                      // negotiating a codec. Carries no payload: the details
                      // are read from bt_codec on the app thread, because a
                      // MAC does not fit in an intptr_t and the answer may have
                      // moved on by the time this is drained anyway.
                      BtRouteChanged };

// The single repeating timer this app asks for: playback position updates.
enum class TimerId { SeekUpdate };

class PlayerWindow : public AppView {
public:
    // `injectedHost` is the dev-tooling seam: pass nothing and create() builds
    // the platform's real Host via make_host(), exactly as before. The headless
    // UI-capture tool (tools/ui_capture) passes its own Host so that everything
    // below this line — Vulkan init, the DB restore, the font/atlas bake, the
    // layout — runs the SAME code the shipping app runs, against a surface that
    // presents to nothing. A capture that re-implemented any of it would stop
    // describing the app the moment the app moved.
    bool create(std::unique_ptr<Host> injectedHost = nullptr);
    void run();

#ifdef MATRIX_UI_CAPTURE
    // Compiled ONLY into tools/ui_capture — the shipping binary never defines
    // MATRIX_UI_CAPTURE, so neither method exists in it.
    //
    // captureGoTo() reaches a named UI state by SYNTHESIZING THE CLICKS a user
    // would make, on the rects recalcLayout() itself computed. Being a member
    // is the whole point: the tool never restates where the sidebar or the
    // settings rows are, so the captures follow the layout instead of drifting
    // behind it. Returns false for an unknown state name.
    bool captureGoTo(const std::string& state);
    // Draws exactly one frame and reads the swapchain image back as
    // tightly-packed RGBA8 (Renderer::readbackLastFrame).
    bool captureFrame(std::vector<uint8_t>& rgba, uint32_t& w, uint32_t& h);

    // Replaces the library with a deterministic synthetic one, so a capture
    // run says something on a machine with no music on it.
    //
    // This exists because a capture of an EMPTY grid proves nothing: five of
    // the states already report themselves unreachable, and — worse for a
    // before/after diff — the per-tile text layout (truncateToWidth /
    // splitTwoLines) never runs at all, so a diff of those PNGs would happily
    // call a broken text change identical.
    //
    // Generated in code rather than read from a file: a fixture that can drift
    // from the tool that consumes it is a fixture that will. Every string here
    // is chosen to exercise something — titles long enough to wrap and then
    // truncate, "(24-96)"-style quality suffixes, every ReleaseType so the six
    // grid states are populated, and Han/Hangul/Cyrillic records that sort to
    // the END of a Latin-first list, which is exactly where 17-grid-multiscript
    // scrolls to and where the fallback faces are the only thing that can draw.
    //
    // MUST be called AFTER the background scan has reported in: onScanDone()
    // replaces albums_ wholesale, and a scan that found nothing (no music
    // roots on this machine) would wipe the fixture right back out.
    void captureLoadFixture(int albumCount = 240);
#endif

    // Host callbacks — the AppView implementation. Public because Host (a
    // separate object, not a PlayerWindow subclass) dispatches into these
    // directly, the same way handleMsg used to before it moved into
    // os/windows_host.cc / os/linux_host.cc. Not part of the app's own
    // conceptual API. See app_view.hh for what each one promises.
    void onHostResized() override;          // an explicit UI-mode/monitor change: notifyResized() + relayout
    void onHostLayoutInvalidated() override; // routine resize notification (no notifyResized(), see .cc)
    void onHostExposed() override;          // window newly visible/uncovered — just mark a frame dirty
    void onKeyDownPortable(int keyCode) override;      // key::* space (keys.hh) — shared key handling
    void onCharPortable(uint32_t codepoint) override;  // search-box text entry
    void onHotkey(int hotkeyId) override;              // Alt+F/J/C/U/G/H/L — see hotkey_ids.hh
    void adaptToCurrentMonitor() override;             // WM_DISPLAYCHANGE/WM_WINDOWPOSCHANGED re-fit
    void shutdown() override;      // teardown before the window/renderer die (was WM_DESTROY)

    // ── The drawing surface can come and go ─────────────────────────────────
    //
    // On a desktop the window is born once and dies once, so create() builds
    // the Renderer once and shutdown() tears it down. On Android the surface
    // dies EVERY time the listener leaves the app and is born again on return
    // (APP_CMD_TERM_WINDOW / APP_CMD_INIT_WINDOW) — and with it go the
    // swapchain, every album-art texture, and the glyph atlas that lives in
    // GPU memory. Neither of these is called by either desktop host.
    //
    // The split they enforce is the one that matters: what lives in CPU
    // memory (the loaded faces, the RasterFont's outlines and its placed
    // cells) SURVIVES; what lives on the GPU does not. Getting that wrong is
    // invisible until it isn't — a stale "already uploaded" flag means every
    // string on screen vanishes on the second visit, with no error anywhere.
    void onSurfaceLost() override;
    // Rebuilds everything the above released, against the host's NEW surface.
    // Returns false if the Renderer could not be created, in which case the
    // caller must not draw.
    //
    // Two paths, and the fast one is the normal one: if the device survived
    // (surfaceOnly_), only the VkSurfaceKHR and its swapchain are rebuilt and
    // every pipeline, the glyph atlas and every album-art texture stay put.
    // The full Vulkan bring-up is the fallback, taken only when the new
    // surface's format is incompatible with pipelines already built.
    bool onSurfaceRecreated() override;

private:
    // Set by onSurfaceLost(), cleared by onSurfaceRecreated(). "The window went
    // away but the device did not" — the ordinary Android background/foreground
    // cycle. Not a general Renderer state: nothing else may read it, because
    // between those two calls there is no surface to draw to and run() is
    // already gated on the host not pumping a frame.
    bool surfaceOnly_ = false;
    // The pre-existing full teardown, kept whole as the fallback rather than
    // re-derived. See its definition.
    void destroyRendererForSurfaceLoss();
public:

    // Mouse — dispatched from Host's input translation.
    void onMouseMove(int x, int y) override;
    void onMouseLeave() override;
    // A press ARMS a click; it never performs one. Every action in this window
    // fires from onLButtonUp instead, and the reason is the phone: the host
    // reports a press the moment the finger touches the glass, so acting there
    // meant that starting a scroll on an album tile opened that album under
    // the finger — and the drag that followed then scrolled the album view
    // that had just appeared. Nothing in this app is a press-and-drag control
    // (there is no scrubber and no slider), so nothing needs the down edge,
    // and firing on release is what every desktop toolkit does anyway.
    void onLButtonDown(int x, int y) override;
    void onLButtonUp(int x, int y) override;
    void onLButtonDblClk(int x, int y) override;
    void onMouseWheel(int x, int y, int delta) override;
    // A DRAG that has ended: the pointer (or the finger) travelled dx,dy with
    // the button held and has just been released. Reported by the host rather
    // than reconstructed here, because each platform already tracks it — Win32
    // from WM_LBUTTONDOWN/UP, Wayland from PointerAction::Down/Up, Android
    // from its own 24 px slop, which is also why the SLOP stays a host
    // property and the MEANING stays an app one. A tap never reaches this.
    //
    // It exists for the fullscreen artwork: a swipe ejects the picture onto a
    // second screen where a second screen can exist, and does nothing where it
    // cannot — a gesture costs no layout, which is the whole point (see
    // "Optional capabilities" in CLAUDE.md).
    void onDragEnd(int dx, int dy) override;
    // The two extra buttons on the side of a mouse (Win32 XBUTTON1/XBUTTON2,
    // evdev BTN_SIDE/BTN_EXTRA). Back is Escape by another name — the same
    // one-step-out that a browser's back button performs; forward re-enters
    // exactly what the last back left. Public for the same reason the rest of
    // this block is: Host dispatches straight into them.
    void onNavBack() override;
    void onNavForward() override;

private:
    // The armed click: where the press landed, and whether it is still a click
    // at all. Disarmed by any scroll (on a touch screen the wheel IS the drag)
    // and by onDragEnd, so a stroke that turned into a gesture never activates
    // whatever it happened to start on. handleClick() is the body that used to
    // be onLButtonDown's.
    void handleClick(int x, int y);
    int  pressX_ = 0, pressY_ = 0;
    bool pressArmed_ = false;

public:

    // Periodic playback-position tick, see host_->startTimer(). The id is
    // TimerId::SeekUpdate and is not read: this app asks for one timer.
    void onTimer(int timerId) override;

    // The host is up and we are fully built. Seeds the music library from
    // Host::launchArgument() when the platform arrived already knowing it and
    // nothing is indexed yet — which today means Android's "scan_root" intent
    // extra, and on a desktop means nothing at all.
    //
    // Guarded here rather than by the host: it fires again after a suspend on
    // platforms that have one.
    void onHostReady() override;

    // Dispatches an AppEvent posted from a background thread. The host carried
    // the three integers and read none of them — see host.hh.
    void onAppEvent(int id, intptr_t p1, intptr_t p2) override;

    // Adds a music root and starts a scan of it — what the folder picker does
    // when a folder is chosen. Public because onHostReady() is not the only
    // caller; the picker's own commit path lands here too. Idempotent at the DB
    // level (addMusicRoot ignores a duplicate), but a caller should still check
    // hasMusicRoots() rather than re-adding on every launch.
    void commitAddFolder(const std::string& root);
    bool hasMusicRoots();

    // Cross-thread completions, reached from onAppEvent() above.
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
    // comment on the definition for why both halves matter. The async path
    // (requestArtistImage) is the normal one; the sync decode is only the
    // fallback when no worker is available.
    int artistImageTargetSize() const;
    void requestArtistImage(const std::string& path, int targetSize);
    void ensureArtDecodeThreads();
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
    void        applyCursor();             // re-derive from lastMouseX_/Y_
    CursorShape lastCursor_ = CursorShape::Arrow;
    int         lastMouseX_ = 0, lastMouseY_ = 0;
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
    // (There is no resume-on-launch state here any more. The player does not
    // return you to the middle of a record — see savePlaybackStateNow().)
    void onArtClick();
    void onEqSettings();
    void toggleBitperfectMode();
    void setupWatchers();
    std::string getActiveDeviceKey();
    std::string audioBackendLabel() const;
    void applyDeviceEq(int sampleRate, int channels);

    // The ONE path that changes which headphone profile is in force — the
    // sidebar block and the EQ panel's Assign button both go through it, so
    // "write the assignment, apply the coefficients, restart the trial clock"
    // can't drift apart between the two call sites.
    void selectEqProfile(const EqAssignment& a);
    // ...and its opposite, for the same reason: the sidebar's "No AutoEQ" row
    // and the EQ panel's Clear button are one implementation. It clears the
    // legacy "global" assignment too — see the comment on the definition.
    void clearEqProfile();
    void reloadEqHeadphones();
    bool isKnownHeadphone(const EqAssignment& a) const;
    // Bottom-anchored inside the sidebar. Draws nothing in bitperfect mode:
    // there is no EQ to pick a profile for.
    // Search is a STATE of bar A, not just a focused text box: opening it is
    // what collapses the filter letters, and closing it takes the query back.
    void openSearch();
    void closeSearch();
    // Bar A draws and hit-tests itself from plain values (bar_a.hh), shared
    // verbatim with Android. All that is left here is building those values
    // out of this app's state, and translating the pick back into the integer
    // hit vocabulary the rest of this class speaks.
    BarAModel       barAModel() const;
    static BarAPick sidebarHitToPick(int nav);
    static int      pickToSidebarHit(const BarAPick& p);

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
    const EqHeadphone* eqSelectedHeadphone() const;

    void drawFolderPicker(Canvas& canvas, const LayoutRect& area);
    void fpLoadDir(const std::string& dir);

    // Layout
    void recalcLayout();

    // Layout orientation — see ui_orientation.hh.
    void toggleOrientation();

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
    // a .streamer/library.db found by walking UP from the album's own folder
    // (see streamerDbForAlbumDir()) and keyed by the album's folder name, and
    // falling back to the legacy sidecar-file convention (bio.* + an image
    // loose in the artist's own folder, one level above the album folder)
    // when no such database is found.
    void openAlbumView(int albumIdx);
    void loadAlbumViewContent(int albumIdx);

    // The downloader library that owns `albumDir`, or nullptr when that album
    // is not inside one. Resolves on first use and caches; see streamerDbs_.
    // std::string rather than a path, to keep <filesystem> out of this header.
    StreamerDb* streamerDbForAlbumDir(const std::string& albumDir) const;

    // Bake every codepoint the app can draw, at every size it draws them at,
    // and push the grown atlas to the GPU. Idempotent — cells already present
    // are skipped, so a rescan costs only what is genuinely new.
    //
    // The fallback chain is all serif, matching New Computer Modern's look
    // rather than jumping to a system UI font: NewCM itself (Latin, Greek,
    // Cyrillic) -> Fandol Song (Chinese) -> Harano Aji Mincho (Japanese) ->
    // Un Batang (Korean). No system fonts involved on either platform.
    void refreshGlyphs();

    // The role sizes the glyph cache was last baked for. When these change —
    // which is every resize — the cache is reset rather than grown, so a long
    // session cannot accumulate the cells of every size it has ever been.
    std::vector<int> glyphSizes_;

    // Bake whatever the last frame asked for and did not have. A per-size
    // cache cannot know every size up front (icon boxes come from layout
    // geometry, the art window has its own scale), so it learns them from what
    // is actually drawn — see RasterFont::hasMisses().
    void bakeGlyphMisses();
    void runGlyphBaker();
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

    // Orientation state. `orientation_` is the POLICY (automatic, or a manual
    // choice that sticks); `curOrientation_` is the ANSWER for the window as it
    // is right now, recomputed once per recalcLayout() so every drawing and
    // hit-testing site reads one consistent value for the whole frame. Calling
    // resolve() ad hoc from draw code would let two halves of one frame
    // disagree during a resize.
    // (The toggle is keyboard-only for now: Alt+L. No on-screen button.)
    UiOrientationState orientation_    = {};
    UiOrientation      curOrientation_ = UiOrientation::Horizontal;

    // ── The frame ───────────────────────────────────────────────────────────
    // Two bars of ONE thickness facing each other, content centred between.
    // Bar A is navigation (top in Vertical, left in Horizontal), bar B is the
    // transport (bottom / right). rcSidebar_ and rcTransport_ are the same two
    // rectangles under the names the rest of this file already reads.
    LayoutRect rcBarA_ = {};
    LayoutRect rcBarB_ = {};
    // Where everything inside bar A sits — computed by computeRailLayout(),
    // never by hand. See rail_layout.hh.
    RailLayout rail_ = {};
    // Whether the search field has taken over bar A's middle. Distinct from
    // searchFocused_, which is about the caret: the field only EXISTS while
    // this is set, because opening it is what collapses the filter letters.
    bool searchOpen_ = false;

    // Layout zones
    LayoutRect rcSidebar_    = {};
    LayoutRect rcGrid_       = {};
    LayoutRect rcTrackPanel_ = {};
    LayoutRect rcTransport_  = {};

    // Transport sub-regions. rcBtnPlay_ is the combined play/stop toggle —
    // there is deliberately no pause and no seek anywhere (this user only
    // ever stops or starts from zero).
    //
    // The bar is a BALANCE about the play button (see recalcLayout): artwork
    // and clock are the two masses at the two ends, type and DSP tag the two
    // labels facing the centre. All five are plain geometry now — rcDspBadge_
    // used to be written by drawFrame() from the measured text, and grew to
    // cover the clock as well, which is why touching the clock opened the
    // signal chain. The tag is the button; the clock is not.
    LayoutRect rcTransportArt_   = {};
    LayoutRect rcTransportInfo_  = {};
    LayoutRect rcBtnPrev_        = {};
    LayoutRect rcBtnPlay_        = {};
    LayoutRect rcBtnNext_        = {};
    LayoutRect rcDspBadge_       = {};
    LayoutRect rcTransportClock_ = {};
    // Non-modal bitperfect-mismatch warning strip, drawn above the transport
    // bar when audioNotice_ is non-empty. See draw()/handleClick().
    LayoutRect rcAudioNotice_ = {};

    // Sidebar items
    LayoutRect rcBrand_       = {};
    LayoutRect rcNavAlbum_    = {};
    LayoutRect rcNavEp_       = {};
    LayoutRect rcNavSingle_   = {};
    LayoutRect rcNavRemix_    = {};
    LayoutRect rcNavCompilation_ = {};
    LayoutRect rcNavLive_     = {};
    LayoutRect rcNavPlaylists_ = {};
    LayoutRect rcNavSettings_ = {};

    // Settings page items
    LayoutRect rcSettingsAddFolder_    = {};
    LayoutRect rcSettingsManage_       = {};
    LayoutRect rcSettingsAudio_        = {};
    LayoutRect rcSettingsEq_           = {};
    LayoutRect rcSettingsBitperfect_   = {};

    // ── Settings panels state (Phase 7) ──────────────────────────────────
    SettingsPanel activePanel_ = SettingsPanel::None;
    // Authored at the 1080 reference height, same convention as every other
    // panel dimension (see ui_metrics.hh) — MUST go through panelRowH(), never
    // used bare, or list rows stop scaling with the rest of the panel chrome
    // above ~1080p (a fixed-size row next to metrics_.space()-scaled headers/
    // buttons/padding reads as squashed proportions at e.g. 8K).
    static constexpr int kPanelRowH = 44;
    float panelRowH() const { return metrics_.space((float)kPanelRowH); }

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
    // ── Bluetooth codec, inside the Audio Settings panel ────────────────────
    // It lives HERE, and not on a screen of its own, because it is a property
    // of the output — the same panel that already chooses the output. The old
    // player gave it a whole Activity and that is what made it feel like a
    // separate feature rather than part of the chain.
    //
    // asBtEdit_ is what the panel is SHOWING, which is not what the stack is
    // running until Apply is pressed — the same relationship every other
    // control on this panel has with the thing it configures.
    bt_codec::Config asBtEdit_;
    // Cached, NOT asked per frame. capability() reaches the Bluetooth service
    // over Binder (getConnectedDevices), and the draw path re-runs on every
    // hover — so asking there turned a mouse moving across the panel into a
    // stream of IPC. Refreshed when the panel opens and whenever the route
    // changes, which is exactly when the answer can differ.
    bt_codec::Capability asBtCap_ = bt_codec::Capability::Unavailable;
    bool             asBtEditLoaded_ = false;   // seeded from the saved/active config once
    std::vector<LayoutRect> asBtCodecRows_;
    // What the connected headphones can actually take, as the stack names
    // them. Refreshed with asBtCap_ — at panel-open and on a route change —
    // never per frame: reading it is a Binder call. EMPTY means the question
    // went unanswered, not that the device supports nothing.
    std::vector<bt_codec::CodecOption> asBtSelectable_;
    int  asHoverBtCodecRow_ = -1;
    LayoutRect asBtRateRc_{}, asBtBitsRc_{}, asBtQualityRc_{}, asBtEnableRc_{}, asBtForgetRc_{};
    bool asHoverBtRate_ = false, asHoverBtBits_ = false, asHoverBtQuality_ = false,
         asHoverBtEnable_ = false, asHoverBtForget_ = false;

    void drawBluetoothCodecSection(Canvas& canvas, const Rect& c, float& y, float pad);
    bool handleBluetoothCodecClick(int x, int y);
    void seedBtEditFromDevice();
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
#ifdef MATRIX_HAVE_BLUETOOTH
    // No leading "(default)" row, unlike ALSA and JACK: there is no default
    // pair of headphones, so -1 means nothing is chosen and the list indexes
    // asBtDevices_ directly.
    std::vector<BtDeviceInfo> asBtDevices_;
    int  asBtSel_       = -1;
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
    // The panel's two header lines, cached. drawEqSettings() used to run TWO
    // sqlite queries and rebuild both strings on EVERY frame the panel was up —
    // sixty times a second to redraw text that only changes when the assignment
    // does. Refreshed via markEqAssignmentDirty() from the three places that can
    // change it: onEqSettings() (which also re-reads the device), the Clear
    // button, and selectEqProfile() — the single funnel every save goes through.
    std::string eqDeviceLine_;
    std::string eqAssignLine_;
    bool eqAssignLineDirty_ = true;
    void markEqAssignmentDirty() { eqAssignLineDirty_ = true; }
    LayoutRect eqSearchRc_ = {}, eqListArea_ = {}, eqCloseRc_ = {}, eqBtnAssign_ = {}, eqBtnClear_ = {};
    bool eqHoverClose_ = false, eqHoverAssign_ = false, eqHoverClear_ = false;
    std::vector<widgets::ListRow> eqListRows_;  // cached during draw, read by hit-test
    // Two views over ONE list and ONE selection: the full catalogue, or just
    // the saved headphones. Pinning and removing live only here — the sidebar
    // stays a pure switcher, with no room for a per-row × at 277px wide.
    bool eqShowMine_ = false;
    LayoutRect eqTabAll_ = {}, eqTabMine_ = {}, eqBtnPin_ = {}, eqBtnRemove_ = {};
    bool eqHoverTabAll_ = false, eqHoverTabMine_ = false;
    bool eqHoverPin_ = false, eqHoverRemove_ = false;

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

    // ── Resolved grid pads, in DEVICE pixels ────────────────────────────────
    // gridPadX_ above is the ONLY authored pad (at the 1080 reference) — it is
    // the horizontal margin, and the sole knob for "more air around the grid."
    // There is no authored vertical pad: gridPadYpx_ below is DERIVED from
    // gridPadXpx_ and the cell's own centering slack via gridTopPad(), never
    // hand-tuned. This is by design, not an omission — the top margin and the
    // left margin are meant to stay optically equal (a tile is centered in
    // its cell, so the left margin already includes half the cell's slack;
    // the top pad must absorb that same slack or the first row reads tighter
    // than the sidebar beside it). Widening the top margin independently of
    // the left one would break that property, so there is nothing to author.
    // To put more air above the grid, change gridPadX_ — it moves both.
    //
    // gridPadXpx_/gridPadYpx_/gridStepX_ are what recalcLayout() resolves
    // gridPadX_ to every layout pass, and they are the ONLY thing the draw
    // block and gridHitTest() are allowed to read.
    //
    // They exist because those two used to read gridPadX_ (and a second,
    // now-deleted authored vertical pad) raw while recalcLayout() passed the
    // same numbers through space() — so at any height above 1080 the layout
    // reserved one column width and the draw painted another, and the top
    // margin never scaled at all.
    int gridPadXpx_ = 24;
    int gridPadYpx_ = 16;
    int gridStepX_  = 0;    // cell stride incl. margins; was recomputed twice

    // Sidebar search box — live-filters the album grid. gridIndices_ is the
    // single indirection: tile position → albums_ index. Draw loop and
    // gridHitTest() both go through it, so every click/hover consumer keeps
    // receiving real album indices whether or not a filter is active.
    LayoutRect       rcSearch_ = {};
    std::string      searchQuery_;       // UTF-8 — the text still being typed
    bool             searchFocused_ = false;
    std::vector<int> gridIndices_;
    void rebuildGridIndices();

    // ── Guided search (core/facets.h) ───────────────────────────────────────
    // The box does not parse a sentence. What the listener types is matched
    // against values that EXIST in this library, offered as suggestions, and
    // accepted into chips:
    //
    //     [ Björk ]  AND  [ 1990–1999 ]  AND  [ 24-bit ]
    //
    // searchQuery_ keeps its old job (free text, matched against names) and
    // the chips carry everything structural. Both feed rebuildGridIndices().
    std::vector<facets::Chip>       searchChips_;
    std::vector<facets::Suggestion> searchSuggest_;
    int  searchSuggestSel_ = -1;         // keyboard highlight; -1 = none
    // Rects for hit-testing, rebuilt by the draw pass that owns them.
    LayoutRect              rcChips_ = {};     // strip above the grid
    std::vector<LayoutRect> chipRects_;        // parallel to searchChips_
    std::vector<LayoutRect> suggestRects_;     // parallel to searchSuggest_
    int  hoverChipIdx_    = -1;
    int  hoverSuggestIdx_ = -1;
    void refreshSuggestions();
    // Arrow-key movement through the suggestion strip. False = nothing to move
    // through, so the key keeps whatever meaning it had. See the definition for
    // why it is bounded by suggestRects_ rather than by searchSuggest_.
    bool moveSuggestSel(int step);
    void acceptSuggestion(int i);
    void removeChip(int i);
    // Why the current chips found nothing, and which chip to blame — the
    // difference between "you own no 24-bit" and "none in the nineties".
    //
    // EXPENSIVE: not one library scan but up to nine (see the definition), so
    // the sentence it produces is cached and the draw pass reads the cache.
    // Everything that can change the answer — a chip added or removed, the
    // typed text, a finished rescan — must call markSearchEmptyDirty(); empty
    // string means "no reason to give", not "not computed yet".
    facets::EmptyReason searchEmptyReason() const;
    std::string         searchEmptyMsg_;
    bool                searchEmptyDirty_ = true;
    void markSearchEmptyDirty() { searchEmptyDirty_ = true; }

    // Album variants (see core/include/core/variants.h). The grid shows one
    // tile per GROUP — its best member — instead of one per folder, so the
    // same release held twice (Deluxe beside standard, 24/96 beside 16/44.1)
    // stops appearing as near-duplicate tiles. Derived from albums_ by a pure
    // function, never persisted: rebuildAlbumGroups() runs immediately before
    // rebuildGridIndices() everywhere albums_ is reassigned.
    std::vector<AlbumGroup> albumGroups_;
    std::vector<int>        albumGroupOf_;   // albums_ index → albumGroups_ index
    void rebuildAlbumGroups();

    // trackKey() → the (album, track) that should PLAY for that identity.
    //
    // A generated playlist is a list of keys, and a key is deliberately shared
    // by every copy of the same music on disk (27 of 518 rows in the test
    // library), so turning one back into something playable means choosing a
    // copy. The choice is variantOutranks() (core/variants.h) — DSD over PCM,
    // then sample rate, then bit depth — reused rather than restated, because
    // a second answer to "which copy is best" is one answer too many.
    //
    // Rebuilt inside rebuildAlbumGroups(), so it can never be stale while
    // albums_ is fresh. Read under albumsMu_ like albums_ itself.
    std::unordered_map<std::string, std::pair<int, int>> trackKeyIndex_;
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
    // pattern as rcDspBadge_) and read by handleClick to open the viewer.
    // It is the CROPPED rect — clicking the sliver that is actually on screen
    // is what a user can aim at. Empty when the photo is off-screen.
    LayoutRect               rcArtistImg_ = {};
    // The album view's prose column (description + bio), written by
    // drawFrame() like rcArtistImg_. Not clickable and deliberately without a
    // hover background — it exists so the cursor can say "text" there.
    LayoutRect               rcAlbumText_ = {};
    // The "OTHER VERSIONS" strip below the artist bio: where each variant
    // thumbnail landed this frame and which album it stands for. Written by
    // drawFrame() and read by handleClick/cursorForPoint, the same
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

    // ── Full-page SCENES (a third mechanism, and deliberately not a fourth) ──
    // The content area already had three occupants and the difference between
    // them is load-bearing (settings_panels.hh documents the regression from
    // confusing two of them): a PANEL intercepts every event before the
    // sidebar or the transport is hit-tested; a SECTION and the album VIEW do
    // not, and are reached by ordinary rect-gated branches.
    //
    // These two are for LOOKING AT MUSIC, not for configuring, so they follow
    // the album view: the transport stays live underneath, Escape and goBack()
    // close them. That is not a stylistic match — the signal-chain page exists
    // to be read WHILE something is playing, and a panel would kill the very
    // Space bar that stops it.
    //
    // One enum for both, because they share everything except what they draw:
    // the same dismissal, the same rect, the same place in drawFrame().
    enum class ContentOverlay { None, AlbumArt, SignalChain };
    ContentOverlay overlay_ = ContentOverlay::None;
    // What the art scene is showing. Kept separately from displayAlbum_
    // because the two answer different questions: the scene follows the
    // MUSIC (transportArtTexPath_), and the album view follows the BROWSER.
    std::string overlayArtPath_;
    // Its own texture, at its own size. transportArtTex_ is baked to the
    // thumbnail's pixels (see loadTransportArtTexture) and is unusable here.
    TextureHandle overlayArtTex_ = kInvalidTexture;
    int overlayArtTexW_ = 0, overlayArtTexH_ = 0;
    std::string overlayArtTexPath_;   // cache key; cleared with the handle
    LayoutRect rcOverlayImage_{};
    void openArtOverlay(const std::string& path);
    // The swipe's destination: ArtWindow, where one can exist. Returns false
    // where it cannot, and the gesture then does nothing at all — no message,
    // no empty state, no reserved pixel. See CLAUDE.md, "Optional
    // capabilities".
    bool ejectArtToSecondScreen();
    void drawSignalChain(Canvas& canvas, const LayoutRect& area);
    int  scScrollY_ = 0;
    int  scContentH_ = 0;      // measured by the draw, like albumViewContentH_
    LayoutRect rcScClose_{};
    bool hoverScClose_ = false;
    void closeOverlay();
    void drawArtOverlay(Canvas& canvas, const LayoutRect& area);
    void releaseOverlayArtTexture();
    // Sidebar is two independent things: which album TYPE is being browsed
    // (Albums/EPs/Singles/Remixes — filters the grid), and whether the
    // Settings gear is open (replaces the whole content area). They're
    // deliberately separate state: leaving Settings returns to whichever
    // type filter was active, never hardcoded back to Albums.
    enum class AlbumTypeFilter { Album, Ep, Single, Remix, Compilation, Live };
    // rebuildGridIndices() compares this against Album::ReleaseType via a
    // plain (int) cast — these static_asserts make a future reorder of
    // either enum a build error instead of a silently wrong filter.
    static_assert((int)AlbumTypeFilter::Album  == (int)Album::ReleaseType::Album,  "AlbumTypeFilter/Album::ReleaseType value mismatch");
    static_assert((int)AlbumTypeFilter::Ep     == (int)Album::ReleaseType::Ep,     "AlbumTypeFilter/Album::ReleaseType value mismatch");
    static_assert((int)AlbumTypeFilter::Single == (int)Album::ReleaseType::Single, "AlbumTypeFilter/Album::ReleaseType value mismatch");
    static_assert((int)AlbumTypeFilter::Remix  == (int)Album::ReleaseType::Remix,  "AlbumTypeFilter/Album::ReleaseType value mismatch");
    static_assert((int)AlbumTypeFilter::Compilation == (int)Album::ReleaseType::Compilation, "AlbumTypeFilter/Album::ReleaseType value mismatch");
    static_assert((int)AlbumTypeFilter::Live   == (int)Album::ReleaseType::Live,   "AlbumTypeFilter/Album::ReleaseType value mismatch");
    AlbumTypeFilter albumTypeFilter_ = AlbumTypeFilter::Album;

    // Which of the sidebar's SEVEN content rows is showing. Albums/EPs/Singles/
    // Remixes/Compilations/Live are one section browsing six release types
    // (albumTypeFilter_ says which); Playlists is the seventh, and it is a
    // section in exactly the same sense — a grid of tiles in the content area,
    // with the sidebar, the
    // transport bar and Settings all still live behind it. It used to borrow
    // the settings OVERLAY instead, which made it the one row you could not
    // click your way out of: every mouse and key event was diverted to the
    // panel dispatchers before the sidebar was ever hit-tested.
    //
    // Deliberately NOT another AlbumTypeFilter value: that enum is cast
    // straight to Album::ReleaseType (see the static_asserts above), and a
    // value with no release type behind it would empty gridIndices_ — which
    // onNext()/nextAlbumInSection() and the grid both read.
    enum class NavSection { Albums, Playlists };
    NavSection      navSection_      = NavSection::Albums;
    bool            settingsOpen_    = false;
    // True while a panel is only borrowing the settings overlay to draw in —
    // opened from the sidebar's headphone switcher, not by walking into
    // Settings. closeActivePanel() restores the previous view when it is set.
    bool            panelFromSidebar_ = false;
    // sidebarHitTest() sentinels. The first SIX values are AlbumTypeFilter
    // casts, so everything else starts above them — and handleClick() must
    // test these BEFORE its `nav >= 0` branch, which would otherwise cast a
    // sentinel straight into an out-of-range AlbumTypeFilter.
    //
    // These numbers move every time a release type is added: they started at
    // 4, and Compilation taking 4 would have made a click on it read as a
    // click on Settings. The static_assert below is what makes that a build
    // error rather than a mystery in the UI.
    static constexpr int kSidebarSettingsHit  = 6;  // the Settings row
    static constexpr int kSidebarHpMoreHit    = 7;  // the AutoEQ block's "Search more…"
    static constexpr int kSidebarPlaylistsHit = 8;  // the Playlists row
    static constexpr int kSidebarHpNoneHit    = 9;  // the AutoEQ block's "No AutoEQ"
    static constexpr int kSidebarSearchHit    = 10; // the search letter
    static constexpr int kSidebarSearchCloseHit = 11; // its close cell, while open
    static constexpr int kSidebarEqBoxHit     = 12; // the AutoEQ box's name (unfurls)
    static constexpr int kSidebarEqNoneHit    = 13; // the AutoEQ box's X (= no profile)
    static_assert(kSidebarSettingsHit > (int)AlbumTypeFilter::Live,
                  "sidebarHitTest sentinels must start above the last AlbumTypeFilter");
    static constexpr int kSidebarHpRowBase   = 100;  // + row index within hpRows_

    // ── Playlists ───────────────────────────────────────────────────────────
    // Three generated lists, each of which IS its query (core/db.h) — nothing
    // is stored, so nothing can drift from the listening log. plKind_ is the
    // second level of state INSIDE NavSection::Playlists, and it mirrors the
    // album section exactly: None draws the TILE GRID (one tile per list, the
    // grid's own geometry), anything else draws that list full-page — the same
    // two levels the album grid and the album view already are.
    enum class PlaylistKind { None, HeavyRotation, ForgottenFavourites, NeverHeard };
    PlaylistKind plKind_ = PlaylistKind::None;
    // Loaded once per kind-chosen / range-changed, never per frame: the query
    // is the playlist, and re-running it while scrolling would let the list
    // shift under the cursor.
    std::vector<TopEntry> plEntries_;
    // Parallel to plEntries_. TopEntry carries no duration, so each row's key
    // is resolved through trackKeyIndex_ ONCE per load, under a single
    // albumsMu_ lock. -1 where the key no longer resolves to anything on disk.
    std::vector<int>      plDurationMs_;
    // Only heavyRotation() takes a range; the other two queries have no such
    // parameter, so they show no tabs.
    RangePreset plRangePreset_ = RangePreset::Last30Days;
    // Row height, derived from the two text roles a row actually stacks
    // (title + artist) rather than borrowed from kPanelRowH, which is sized
    // for the ONE line the settings panels draw. Written by drawPlaylists(),
    // read by the wheel clamp and the scrollbar so all three agree.
    int  plRowH_      = 0;
    int  plScrollY_   = 0;
    int  plHoverRow_  = -1;
    int  plHoverTile_       = -1;   // 0..2, the tile grid's hovered list
    int  plHoverRangeTab_   = -1;
    LayoutRect plListArea_ = {};
    LayoutRect plRangeTabRc_[5] = {};
    std::vector<widgets::ListRow> plListRows_;  // cached during draw, read by hit-test

    // What a playlist tile WEARS, since a generated list has no artwork of its
    // own. For an ORDERED list the tile is a 2x2 mosaic of the covers behind
    // its top entries, in rank order — see drawPlaylistTileArt() for the
    // quadrant numbering and the fourth-quadrant rule. `albums` holds album
    // indices (distinct: two rows off the same record must not paint the same
    // cover twice), `count` how many of the four are filled, and `more`
    // whether the list runs past them.
    //
    // An UNORDERED list has no "first place" to put in a quadrant, so it wears
    // a flat treatment instead — a solid colour or a gradient, or a custom
    // image the listener picks. Never Heard is the only unordered list today
    // and it is generated, so there is nobody to pick an image FOR it: it gets
    // the gradient. The custom-image and solid-colour arms of that choice
    // belong to hand-made playlists, which do not exist yet (see TODO.md).
    struct PlaylistCover {
        int  albums[4] = { -1, -1, -1, -1 };
        int  count     = 0;
        bool more      = false;
        bool ranked    = false;
    };
    PlaylistCover plCovers_[3];

    static const char* playlistTitle(PlaylistKind k);
    void openPlaylistSection();               // sidebar row -> the tile grid
    void loadPlaylistCovers();                // the three tiles' mosaic sources
    void loadPlaylist(PlaylistKind kind);     // runs the query + resolves durations
    void drawPlaylistSection(Canvas& canvas, const LayoutRect& area);
    void drawPlaylistGrid(Canvas& canvas, const LayoutRect& area);
    void drawPlaylistTileArt(Canvas& canvas, int kindIdx, float x, float y, float a);
    int  playlistTileHitTest(int x, int y) const;   // 0..2, or -1
    void drawPlaylists(Canvas& canvas, const LayoutRect& area);
    void onPlaylistsClick(int x, int y);
    void onPlaylistsMouseMove(int x, int y);  // writes this section's hover state
    void playPlaylistFrom(int row);

    // ── Going back ──────────────────────────────────────────────────────────
    // ONE definition of "one step out", shared by Escape and the mouse's back
    // button so the two can never drift apart. Returns false when there is
    // nothing left to leave. navForward_ remembers exactly the state the last
    // goBack() left, and nothing else writes it — any other navigation clears
    // it, the same way following a link in a browser drops the forward stack.
    struct ViewState {
        NavSection      section        = NavSection::Albums;
        AlbumTypeFilter filter         = AlbumTypeFilter::Album;
        bool            settingsOpen   = false;
        bool            trackPanelOpen = false;
        int             selectedAlbum  = -1;
        PlaylistKind    plKind         = PlaylistKind::None;
    };
    bool      goBack();
    ViewState captureViewState() const;
    void      applyViewState(const ViewState& s);
    ViewState navForward_;
    bool      navForwardValid_ = false;

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
    // The same split overlayArtPath_ documents, for the transport thumbnail:
    // transportArtTexPath_ is what the TEXTURE holds and dies with the surface,
    // transportArtPath_ is what the transport is SHOWING and must not. They are
    // two facts, and on Android they come apart every time the listener leaves
    // the app — the Renderer is destroyed while the music keeps playing, so a
    // gapless boundary lands with nothing to make a texture with. Keeping the
    // path is what lets onSurfaceRecreated() put the picture back.
    std::string   transportArtPath_;

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
    struct ArtDecodeResult { int key; std::string path; std::vector<uint8_t> rgba; int w = 0, h = 0; uint64_t gen = 0; };
    struct ArtDecodeJob    { int key; std::string path; int targetW, targetH;
                                ImageFit fit; uint64_t gen; };
    // A small POOL, not one thread. Covers are decoded one per tile and a
    // screenful is six of them; serialized, that is the whole visible black-
    // square period stacked end to end (measured on a moto g06: 39-60 ms each
    // even after turbojpeg, so ~0.3 s for a screenful on one thread).
    //
    // Deliberately AFTER the turbojpeg change and not before it: without
    // scaled decode each in-flight job held a full-resolution RGBA buffer —
    // ~64 MB for a 4000x4000 cover — and four of those at once is a phone
    // running out of memory. With scaled decode a job holds about a megabyte.
    std::vector<std::thread>    artDecodeThreads_;
    std::mutex                  artDecodeMu_;
    std::condition_variable     artDecodeCv_;
    std::deque<ArtDecodeJob>    artDecodeQueue_;
    std::vector<ArtDecodeResult> artDecodeDone_;
    bool                        artDecodeQuit_ = false;   // guarded by artDecodeMu_
    std::atomic<uint64_t>       artCacheGen_{0};
    std::unordered_map<int, char> artDecodePending_;
    void artDecodeWorker();
    void stopArtDecodeThread();

    // The album-view ARTIST PHOTO is decoded on the same worker pool as the
    // grid covers — one slow artist.jpg (a 7 MB 9000x11054 progressive JPEG is
    // a real case) must not stall the UI thread when the album view opens, and
    // the pool already exists and is pressure-tested. It is keyed by PATH, not
    // by album index, because the photo belongs to the .streamer ARTIST and
    // must survive a rescan (album indices die on every rescan). A negative
    // key keeps artist jobs out of gridArtTexCache_, whose int keys are always
    // artKey(albumIdx, sizeClass) >= 0.
    static constexpr int kArtistArtKey = -1;
    // Main-thread only, single-slot: the photo is one-on-screen, so a map buys
    // nothing. artistImgRequestPath_ is what we currently WANT (set on enqueue
    // by requestArtistImage); artistImgCachePath_ is what artistImgTex_ holds.
    // A late result is applied only while it still matches the request — that
    // is what keeps a quick A→B album switch from painting A's artist under B.
    std::string artistImgRequestPath_;
    std::string artistImgCachePath_;

    // The fullscreen art SCENE decodes asynchronously for the same reason the
    // album-view artist does: the source is often a multi-megabyte JPEG, and
    // the scene resamples to the DISPLAY box (kContain, full-res — never the
    // thumbnail), so a synchronous decode would freeze the UI right as the
    // listener asks to see the art at its best. kOverlayArtKey routes the
    // result to overlayArtTex_ (the scene's texture) from onArtDecoded.
    // overlayArtRequestPath_ is what the newest openArtOverlay asked for; a
    // late result is applied only while it still matches. Single slot, like the
    // artist photo.
    static constexpr int kOverlayArtKey = -2;
    std::string overlayArtRequestPath_;
    // The box the resolved texture was requested for (its decode box, as
    // overlayArtTexW_/H_ is the resulting contain-fit DRAWN size). Comparing
    // the freshly-laid-out box against this is what stops the per-frame draw
    // re-requesting the same unchanged scene — the request is cheap, the
    // decode is not.
    int overlayArtReqW_ = 0, overlayArtReqH_ = 0;
    void requestOverlayArtTexture(int boxW, int boxH);

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
    // mutable so queueActive() can be const: it only reads, but reading these
    // safely still means taking the lock.
    mutable std::mutex albumsMu_;
    std::vector<Album> albums_;
    int  currentAlbum_ = -1;
    int  currentTrack_ = -1;

    // ── The playlist queue ──────────────────────────────────────────────────
    // Empty (queuePos_ < 0) is the ordinary case and the ordinary code path:
    // browsing albums advances by (currentAlbum_, currentTrack_ + 1) and then
    // nextAlbumInSection(), exactly as it always has. A queue only exists
    // while a generated playlist is playing, because a playlist crosses albums
    // and that model cannot express it.
    //
    // GUARDED BY albumsMu_, not by a lock of its own. prepareNextTrack() runs
    // on the UI thread (via onPlay) AND on the background gapless coordinator,
    // so queuePos_ advances from two threads — and albumsMu_ already protects
    // albums_, which is precisely what these indices point into. One lock, one
    // thing protected.
    struct QueueEntry {
        // The identity, kept alongside the indices so a rescan can re-resolve
        // rather than force the queue to be thrown away mid-listen.
        std::string trackKey;
        int album = -1;
        int track = -1;
    };
    std::vector<QueueEntry> queue_;
    int queuePos_ = -1;

    // True while a playlist is driving playback. Takes albumsMu_, so never
    // call it from code already holding it — queueActiveLocked() is the
    // in-lock form.
    bool queueActive() const;
    bool queueActiveLocked() const { return queuePos_ >= 0 && !queue_.empty(); }

    // Replace the queue with `keys` (resolved through trackKeyIndex_, dropping
    // what the library no longer has) and start at `startIndex` of what
    // survived. Returns false when nothing resolved, leaving playback alone.
    bool startQueue(const std::vector<std::string>& keys, int startIndex);

    // Abandon the queue and go back to ordinary album navigation. Called ONLY
    // from click handlers — never from a state change. applyTrackMetadata()
    // reassigns selectedAlbumIdx_ at every gapless boundary, so hanging this
    // off "the selected album changed" would make a playlist cancel itself on
    // its own second track.
    void clearQueue();

    // Re-point the queue's indices at the new albums_ after a rescan, by key.
    // Entries whose music is gone are dropped. Caller holds albumsMu_.
    void reresolveQueueLocked();

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
    // The music roots, as the scanner knows them. Kept because they BOUND the
    // upward walk in streamerDbForAlbumDir(): without a bound that walk would
    // climb to "/" and could bind a database belonging to nothing on screen.
    // Rebuilt by setupWatchers(), appended by commitAddFolder().
    std::vector<std::string> musicRoots_;
    // ".streamer" databases (external Qobuz-style downloader metadata — see
    // core/streamer_db.h), keyed by the DOWNLOADER'S OWN root — the directory
    // that contains the .streamer folder — and NOT by our music root. Those
    // are different directories whenever a library sits below a root, which
    // on Android is always: the downloader writes <external>/Music/streamer
    // while this app is seeded with <external>/Music.
    //
    // Populated lazily, on the first album view opened inside a given library,
    // so a root with no downloader library costs one failed walk and nothing
    // more. Cleared wholesale by setupWatchers(), since a rescan can move or
    // remove what these describe.
    //
    // `mutable` because artistImagePathFor() is const and resolves through
    // here. That is safe ONLY because every path into this map is on the UI
    // thread: loadAlbumViewContent() from openAlbumView(), and
    // artistImagePathFor() from loadTransportArtTexture(), which a gapless
    // boundary reaches through onTimer() — a Host callback, dispatched by
    // pump(). Nothing here is reached from the decode or gapless threads. If
    // that ever changes this needs a lock; it does not have one.
    mutable std::unordered_map<std::string, StreamerDb> streamerDbs_;
    // Music roots already reported as having no downloader library, so the
    // log says it once instead of on every album opened under them.
    mutable std::set<std::string> streamerMissLogged_;
    ArtWindow        artWin_;
    UsbAudioDriver   usbDriver_;
    bool             usbOpen_  = false;

    std::unique_ptr<AudioOutput> output_;
    AudioBackend     audioBackend_  = AudioBackend::Usb;
    std::atomic<bool> bitperfectMode_{false};
    // The app's ONE on-screen channel for "the audio path could not do what you
    // asked". Empty = hidden; cleared at the top of the next onPlay() attempt
    // or on click. Plain string, not atomic — only touched from the UI thread.
    //
    // It was `bitperfectWarning_` and wired to exactly one cause, which left
    // the visibility inverted: a bit-perfect rate mismatch drew a banner, while
    // an ALSA device that would not open at all (PipeWire holding the card, say)
    // failed through Host::showErrorMessage — stderr only on Linux, so nothing
    // on screen and, until the log fix in linux_host.cc, nothing in the log
    // either. Every audio failure goes through here now, carrying the backend's
    // own reason rather than "check Audio Settings".
    std::string audioNotice_;

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
    std::string bpDetail_;      // one line, shown on the badge's page

    // The badge's three forms, in ONE place. Bar B draws all three; the
    // Android notification's sub-text takes `shortForm`, because the badge
    // should not stop telling the truth about the chain just because the
    // screen went off. Extracted from drawFrame() when the second reader
    // appeared — two copies of this switch would drift the first time a state
    // was added, and the whole point of the badge is that it cannot lie.
    struct DspBadge {
        const char* full;       // "BITPERFECT" / "NOT BITPERFECT" / "REF EQ"
        const char* shortForm;  // "EXACT" / "EXACT*" / "ALTERED" / "REF EQ"
        ColorRef    color;
    };
    DspBadge dspBadge() const;

    // What the OS's own transport should show. Built from the DISPLAY cursor,
    // not the decode cursor — see the note in the implementation.
    media_session::NowPlaying nowPlayingForOs() const;

    // Bar B's left label as data — the release type plus, when the record has
    // more than one track, which one. Extracted from drawFrame() so the OS
    // notification and the bar cannot drift apart; the bar draws the two parts
    // at different sizes, which is why they come back separately.
    struct TransportOrdinal {
        const char* type      = nullptr;   // "ALBUM", "EP", "COMPILATION", ...
        const char* typeShort = nullptr;   // "COMP." for the one that needs it
        std::string ord;                   // "", "2", or "D1 \xC2\xB7 2"
        // The same ordinal with its separator closed up — "D1\xC2\xB7 2" becomes
        // "D1\xC2\xB72" — and the bare track number on its own. Rungs on the
        // ladder the bar walks down when the cell is too narrow for the full
        // label; see the draw site. Built here rather than by cutting `ord`
        // apart at the draw, because the pieces are known here and a string
        // taken back apart is a second place for the format to live.
        std::string ordTight;              // "", "2", or "D1\xC2\xB72"
        std::string num;                   // "", or "2"
    };
    TransportOrdinal transportOrdinal(int album, int track) const;
    std::string      transportOrdinalLine(int album, int track) const;

    // ── Bluetooth route ─────────────────────────────────────────────────────
    // The A2DP sink in use, and the codec it negotiated. Both are read on the
    // app thread only; the platform's own callbacks post BtRouteChanged rather
    // than writing here, so there is one writer and no lock.
    bt_codec::Device btDevice_;
    bt_codec::Config btActive_;
    // Set when an apply() came back saying the stack negotiated something else.
    // Shown rather than swallowed: asking for LDAC and silently getting SBC is
    // exactly the kind of quiet downgrade the signal chain exists to expose.
    std::string btNotice_;
    // The device the AUTOMATIC re-apply has already acted on, this run. Setting
    // an A2DP codec bounces the link, so the device disappears and returns —
    // which reads as a new device to onBtRouteChanged() and would start the
    // apply over, forever, with no audio the whole time. See the two guards in
    // applySavedBtCodec(). The panel's own Apply does not consult this.
    std::string btAutoAppliedMac_;

    // Consecutive 250 ms ticks with the decoder stopped AND the output drained.
    // A track change shows that shape for an instant; the end of the music
    // holds it. Only the second one may end the OS media session — see the
    // long comment at the test in onTimer().
    int drainIdleTicks_ = 0;
    static constexpr int kDrainIdleTicksToEnd = 8;   // 2 s

    void onBtRouteChanged();
    // Re-apply whatever is remembered for this MAC. Called when a sink
    // connects and after an association is granted.
    void applySavedBtCodec();

    // ── The signal chain, as DATA ────────────────────────────────────────
    // Everything below was already computed, and then thrown away: the codec
    // label was a local printed only on failure, the resampler's rates and
    // quality lived in a lambda that dies with the callback, the target depth
    // was a local called capturedBits, and the whole chain was summarised into
    // one 192-char sentence. A page cannot show what nobody kept.
    //
    // Nothing here is DERIVED at draw time and nothing is guessed. A field
    // left at its zero means "not known", and the page omits that row rather
    // than inventing a plausible value — the same rule the badge follows, for
    // the same reason: this readout exists to be trusted about loss.
    // Truncated is not a fourth flavour of dither — it is the ABSENCE of one
    // at a stage that still drops bits. The Reference-EQ fast path (rates
    // already match) quantizes once to int32 and hands that to the output
    // adapter, which narrows it with a plain shift: AAudio's `>> 16`, ALSA's
    // S16_LE branch, a USB endpoint configured below 32. That is a real loss
    // and the page has to name it; saying "one rounded snap, no dither" there
    // reads as "nothing was lost", which is the exact overclaim this readout
    // exists to prevent.
    enum class Quant { None, Tpdf, NoiseShaped, Truncated };
    struct SignalChain {
        bool valid = false;          // false until a track actually starts
        // SOURCE — what the DECODER opened, plus what the SCAN had recorded.
        // Both, because they can disagree (nothing has ever compared them),
        // and a library that says 44.1 about a 48 kHz file is worth saying.
        std::string codec;
        int srcRate = 0, srcBits = 0, srcChannels = 0, srcSubslotBytes = 0;
        bool srcIsFloat = false, srcIsDsd = false;
        int declRate = 0, declBits = 0, declChannels = 0;
        int64_t fileSize = 0;  int durationMs = 0;
        // DSP
        bool bitperfect = false;
        bool eqActive   = false;
        std::string eqProfile;
        double eqPreamp = 0.0;
        int    eqFilterCount = 0;
        std::vector<EqFilter> eqFilters;
        bool resampled = false;  int rsFrom = 0, rsTo = 0;
        const char* rsQuality = nullptr;
        Quant quant = Quant::None;  int quantFromBits = 0, quantToBits = 0;
        // OUTPUT
        std::string backend, deviceName, wire;
        int outRate = 0, outBits = 0, outChannels = 0, deviceMaxBits = 0;
        // BLUETOOTH — the last link, and on a phone usually the largest thing
        // that happens to the audio. Everything above describes the PCM handed
        // to the OS; over A2DP the stack then encodes it to SBC/AAC/aptX/LDAC
        // before it reaches the headphones. A readout that stops at "16-bit
        // PCM_I16" and says nothing about a lossy encode is not telling the
        // truth about the chain, which is this struct's whole reason to exist.
        //
        // Empty when the route is not Bluetooth. Reading this needs no
        // permission of any kind, so it is filled in even on a phone that
        // refuses to let the codec be CHANGED.
        std::string btCodec;    // "LDAC", or empty
        std::string btDetail;   // "96 kHz / 24-bit / 990 kbps"
        std::string btDevice;   // the headphones' own name
    };
    SignalChain chain_;
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
#ifdef MATRIX_HAVE_BLUETOOTH
    // BlueZ's object path for the chosen sink. The MAC lives in btDevice_,
    // which the bt_codec seam fills for the signal chain and the AutoEQ key —
    // this is the handle the OUTPUT opens by, and the two are kept apart
    // because one of them exists on Android too.
    std::string      btDevicePath_;
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

    // ── The AutoEQ database, parsed off the startup path ────────────────────
    //
    // eqProfiles_ is filled by this thread, started in the constructor. NOTHING
    // MAY READ eqProfiles_ WITHOUT CALLING ensureEqProfiles() FIRST.
    //
    // That is the whole safety argument, and it is not a style preference:
    // EqProfileStore has no synchronization of any kind, and load() CLEARS
    // profiles_ before refilling it — so a reader arriving mid-parse would walk
    // a vector that is being reallocated underneath it. ensureEqProfiles()
    // joins, and after a join the thread is gone and the store is whole, which
    // is why no mutex is needed anywhere.
    // Mutable because one reader — eqSelectedHeadphone() — is const, the same
    // reason RasterFont::misses_ is mutable: the join is bookkeeping, not a
    // change to what the object means.
    mutable std::thread eqProfilesThread_;
    void ensureEqProfiles() const;

    // The art window is built on first show(), not at launch — see
    // ensureArtWindow(). A refused create() is remembered so it is not retried
    // on every open (the headless capture tool refuses it by design).
    bool artWinReady_  = false;
    bool artWinFailed_ = false;
    void ensureArtWindow();

    // ── DRIVER'S AUTOEQ quick-switcher ──────────────────────────────────────
    // A DAC has no frequency response; the DRIVERS do — headphones, IEMs and
    // speakers alike — and several take turns on one output, so "one profile
    // per device" was the wrong shape. (The sidebar says DRIVER'S AUTOEQ for
    // that reason: "headphones" excluded half of what the list is for, and
    // "drivers" alone would read as an output driver in an app whose primary
    // path is a USB DAC.) The block is the fast way to swap, eq_headphones
    // (core/db.h) is the inventory it lists, and its last row is the OFF
    // position — see clearEqProfile(). EqManager double-buffers its
    // coefficients, so a swap takes effect on the next audio chunk rather
    // than the next track.
    //
    // A profile is applied the instant it is picked but only EARNS a row after
    // kEqCreditMs of real listening. Until then it shows as "on trial" and
    // leaves nothing behind — that is what stops a mis-click from permanently
    // occupying the list. The credit is free: statsMsHeard_ already ticks for
    // the listening log, so there is no second timer.
    static constexpr int64_t kEqCreditMs   = 60000;  // 60 s of audio actually heard
    std::vector<EqHeadphone> eqHeadphones_;
    EqAssignment eqCurrent_;                 // name empty = nothing applied
    bool    eqCurrentTentative_  = false;    // applied, but not yet in eqHeadphones_
    bool    eqCreditedThisTrack_ = false;
    // statsMsHeard_ counts the TRACK, not the profile. Without this baseline a
    // swap 3 minutes into a track would credit the new profile immediately.
    int64_t eqCreditBaselineMs_  = 0;
    // Computed during draw, read by hit-test — same contract as eqListRows_.
    struct HpRow { LayoutRect rc; int headphoneIdx; };  // -1 = the on-trial row
    std::vector<HpRow> hpRows_;
    LayoutRect hpNoneRc_ = {};   // "No AutoEQ" — the off position of the switch
    LayoutRect hpMoreRc_ = {};
    LayoutRect eqNameRc_ = {};   // the AutoEQ box's name — click unfurls the list
    // The AutoEQ box's list is UNFURLED, not clamped: it runs from the box to
    // the far end of bar A and shows the saved list WHOLE. The old four-row cap
    // and the sidebar block's height budget are gone with it — they existed
    // only because three rows had to fit under Settings. With a typical inventory of
    // about seven pairs, showing them all and letting the eye travel beats
    // scrolling. Ordering (pinned, then most-used, then most-recent) and the
    // 60-second credit gate are untouched: those decide WHICH profiles exist,
    // not how many fit.
    bool eqListOpen_ = false;

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

    // Per-size rasterized glyph cache: true 8-bit coverage of the outline at
    // the size it is drawn at, unhinted. Also carries the UI icon glyphs (see
    // ui_icons.hh) — same atlas, same pass.
    //
    // This replaced an MTSDF atlas, which had one bake serve every size and so
    // needed a wide distance-field margin per glyph: a CJK cell cost ~115x104
    // px, a 4096-square sheet ran out at ~1,300 glyphs, and Japanese and Korean
    // silently baked NOTHING. The member name is unchanged only because it is
    // spelled into a lot of call sites; the type is what matters.
    RasterFont           msdfFont_;

    // Glyph rasterization in compute. When it comes up, RasterFont keeps
    // flattened outlines instead of coverage and this writes the atlas image
    // directly — see GlyphBaker. If it fails to initialise, nothing is enabled
    // and the cache stays on its FreeType path, which is also the reference
    // the GPU one was measured against.
    GlyphBaker           glyphBaker_;
    uint32_t             bakedAtlasGen_ = 0;   // which atlas image gpuBakedCount() refers to
    std::vector<float>   msdfQuads_;
    // Set once in create(); exe-relative "fonts/" dir, reused by
    // refreshGlyphs() to find the bundled fallback-script font files.
    std::string          fontsDir_;

    // Recomputed once per recalcLayout() from the window's content height.
    // metrics_.text.* are the type roles; metrics_.space()/stroke() are the
    // geometry helpers. See ui_metrics.hh.
    UiMetrics metrics_{};
};

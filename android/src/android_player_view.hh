#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "aaudio_sink.h"     // ae::AAudioSink
#include "core/decoder.h"    // matrix_core's Decoder (magic-byte format selection)
#include "core/dsp/audio_convert.h"  // DitherLCG + floatToInt16Dither
#include "core/library.h"    // Track, Album, scanLibrary

class Canvas;

// The vertical slice's actual app logic: scan a folder, show a flat
// touch-scrollable track list, tap to play through AAudio. This is NOT a
// port of gui/src/player_view.hh's PlayerWindow (a 1000+ line desktop
// god-object — sidebar, grid, settings panels, EQ, playlists) — it is a new,
// much smaller class for a genuinely different (touch, phone-sized) UI, per
// docs/superpowers/specs/2026-08-08-android-native-port-design.md. Reuses
// core/ (scanLibrary, Decoder) and gui/src/theme.hh + ui_metrics.hh
// unmodified; draws through the same Canvas primitives player_view.cc uses.
//
// Deliberately out of scope for this slice (see the design doc): no
// sidebar/grid/album-art view/EQ/settings panels/gapless coordinator/
// Db-backed persistence/listening-log — just scanLibrary()'s in-memory
// result and one AAudioSink.
class AndroidPlayerView {
public:
    AndroidPlayerView();
    ~AndroidPlayerView();

    // Kicks off scanLibrary(rootPath) on a background thread. Call once,
    // from AndroidHost's window-init path.
    void startScan(const std::string& rootPath);

    // Per-frame update: applies a finished background scan, if any. Call
    // once per frame, before draw().
    void onFrame(float dtSeconds);

    // insetTop/Left/Right: the display-cutout safe area (safe_area.hh).
    // insetBottom: the cutout's bottom inset PLUS the nav-bar height
    // (fullscreen.hh's query_nav_bar_height) — both eat into the same edge,
    // so the caller folds them together before calling this.
    void draw(Canvas& canvas, float screenW, float screenH,
              float insetTop, float insetBottom, float insetLeft, float insetRight);

    // Touch: down starts a potential drag; move scrolls 1:1 once the finger
    // has moved past a small slop (else it stays a tap candidate); up commits
    // a tap (play the row under the finger) if no drag happened. No fling/
    // momentum — out of scope for this slice.
    void onTouchDown(float x, float y);
    void onTouchMove(float x, float y);
    void onTouchUp(float x, float y);

private:
    void applyScanResult();
    void playTrack(size_t index);
    void stopPlayback();
    float rowHeight() const;

    // --- scan state --- scanThread_ writes pendingAlbums_ once and sets
    // scanDone_; only onFrame() (main thread) ever reads pendingAlbums_ or
    // clears scanDone_, so the mutex only ever guards that single handoff.
    std::thread        scanThread_;
    std::atomic<bool>  scanDone_{false};
    std::atomic<bool>  scanning_{false};
    std::mutex         scanResultMu_;
    std::vector<Album> pendingAlbums_;

    std::vector<Track> tracks_;  // flattened; main-thread-only after apply

    // --- scroll/touch state ---
    float scrollY_            = 0.0f;
    float touchStartY_        = 0.0f;
    float touchStartScrollY_  = 0.0f;
    bool  touchIsDrag_        = false;

    // Layout as of the last draw() call — touch hit-testing (next frame's
    // input, arriving between draws) reads these rather than recomputing a
    // possibly-stale layout from scratch.
    float lastScreenH_ = 0.0f;
    float lastTop_     = 0.0f;
    float lastLeft_    = 0.0f;

    // --- playback state ---
    Decoder        decoder_;
    ae::AAudioSink sink_;
    DitherLCG      dither_;               // persists across write() calls — see audio_convert.h
    std::vector<int16_t> i16Scratch_;     // reused decode-thread-only scratch buffer
    int            playingIndex_ = -1;
    bool           playing_      = false;
};

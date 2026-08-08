#include "android_player_view.hh"

#include <android/log.h>
#include <cmath>

#include "canvas.hh"    // vk_canvas: Canvas, Color
#include "layout.hh"    // vk_canvas: clampScroll
#include "theme.hh"     // gui/src: CLR_* (ColorRef)
#include "ui_metrics.hh"  // gui/src: computeUiMetrics

#define LOG_TAG "AndroidPlayerView"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

// theme.hh's CLR_* constants are ColorRef (a packed 0x00BBGGRR uint32_t, the
// portable COLORREF replacement — see gui/src/color.hh) — a different
// representation from vk_canvas's own Color{r,g,b,a} floats Canvas actually
// draws with. No existing bridge between the two (desktop's player_view.cc
// draws through Canvas too, so this conversion presumably lives there or
// inline at each call site — this is the Android equivalent, kept local
// since it's the only file here that needs it).
Color toColor(ColorRef c, float alpha = 1.0f) {
    return Color{GetRValue(c) / 255.0f, GetGValue(c) / 255.0f, GetBValue(c) / 255.0f, alpha};
}

constexpr float kTouchSlopPx = 12.0f;

}  // namespace

AndroidPlayerView::AndroidPlayerView() = default;

AndroidPlayerView::~AndroidPlayerView() {
    stopPlayback();
    if (scanThread_.joinable()) scanThread_.join();
}

void AndroidPlayerView::startScan(const std::string& rootPath) {
    if (scanThread_.joinable() || scanning_.load()) return;  // one scan at a time
    scanning_.store(true);
    scanThread_ = std::thread([this, rootPath]() {
        LOGI("scanning %s", rootPath.c_str());
        std::vector<Album> albums = scanLibrary(rootPath);
        {
            std::lock_guard<std::mutex> lock(scanResultMu_);
            pendingAlbums_ = std::move(albums);
        }
        scanDone_.store(true);
    });
}

void AndroidPlayerView::onFrame(float /*dtSeconds*/) {
    if (scanDone_.load()) {
        applyScanResult();
        scanDone_.store(false);
        scanning_.store(false);
    }
}

void AndroidPlayerView::applyScanResult() {
    std::vector<Album> albums;
    {
        std::lock_guard<std::mutex> lock(scanResultMu_);
        albums = std::move(pendingAlbums_);
    }
    if (scanThread_.joinable()) scanThread_.join();

    tracks_.clear();
    for (auto& album : albums) {
        for (auto& t : album.tracks) tracks_.push_back(t);
    }
    LOGI("scan done: %zu tracks across %zu albums", tracks_.size(), albums.size());
}

float AndroidPlayerView::rowHeight() const {
    UiMetrics m = computeUiMetrics(lastScreenH_ > 0.0f ? lastScreenH_ : kUiReferenceHeight);
    return m.space(72.0f);
}

void AndroidPlayerView::draw(Canvas& canvas, float screenW, float screenH,
                             float insetTop, float insetBottom,
                             float insetLeft, float insetRight) {
    lastScreenH_ = screenH;

    canvas.clear(toColor(CLR_BG_MAIN));

    UiMetrics m = computeUiMetrics(screenH);
    const float rowH = m.space(72.0f);
    const float pad  = m.space(SP_MD);

    const float top    = insetTop + pad;
    const float bottom = screenH - insetBottom - pad;
    const float left   = insetLeft + pad;
    const float right  = screenW - insetRight - pad;

    lastTop_  = top;
    lastLeft_ = left;

    canvas.setClip(left, top, right - left, bottom - top);

    if (scanning_.load()) {
        canvas.text("Scanning...", left, top, m.text.body, toColor(CLR_TEXT_SECONDARY));
    } else if (tracks_.empty()) {
        canvas.text("No tracks found.", left, top, m.text.body, toColor(CLR_TEXT_SECONDARY));
    }

    float y = top - scrollY_;
    for (size_t i = 0; i < tracks_.size(); i++) {
        if (y + rowH >= top && y <= bottom) {
            const bool isPlaying = playing_ && playingIndex_ == static_cast<int>(i);
            if (isPlaying) {
                canvas.rect(left, y, right - left, rowH, toColor(CLR_ACCENT, UI_SELECT_TINT_ALPHA));
            }
            const Color textColor = isPlaying ? toColor(CLR_ACCENT) : toColor(CLR_TEXT_PRIMARY);
            const std::string& label =
                tracks_[i].title.empty() ? tracks_[i].filePath : tracks_[i].title;
            canvas.text(label, left + pad, y + rowH * 0.5f - m.text.body * 0.5f,
                       m.text.body, textColor);
        }
        y += rowH;
    }

    canvas.clearClip();
}

void AndroidPlayerView::onTouchDown(float x, float y) {
    (void)x;
    touchStartY_       = y;
    touchStartScrollY_ = scrollY_;
    touchIsDrag_       = false;
}

void AndroidPlayerView::onTouchMove(float x, float y) {
    (void)x;
    const float dy = y - touchStartY_;
    if (!touchIsDrag_ && std::fabs(dy) > kTouchSlopPx) touchIsDrag_ = true;
    if (touchIsDrag_) {
        const float contentH = static_cast<float>(tracks_.size()) * rowHeight();
        const float viewH    = lastScreenH_;  // clamp against the full screen height —
                                              // a generous bound; exact viewport height
                                              // isn't tracked between frames here.
        scrollY_ = clampScroll(touchStartScrollY_ - dy, contentH, viewH);
    }
}

void AndroidPlayerView::onTouchUp(float x, float y) {
    (void)x;
    if (!touchIsDrag_) {
        const float rowH = rowHeight();
        if (rowH > 0.0f) {
            const float rel = (touchStartY_ - lastTop_ + scrollY_);
            if (rel >= 0.0f) {
                const size_t index = static_cast<size_t>(rel / rowH);
                if (index < tracks_.size()) playTrack(index);
            }
        }
    }
    (void)y;
    touchIsDrag_ = false;
}

void AndroidPlayerView::playTrack(size_t index) {
    stopPlayback();
    if (index >= tracks_.size()) return;

    const Track& t = tracks_[index];
    if (!decoder_.open(t.filePath)) {
        LOGE("decoder_.open failed: %s", t.filePath.c_str());
        return;
    }

    ae::AudioFormat fmt;
    fmt.sampleRate   = decoder_.sampleRate();
    fmt.channels     = decoder_.channels();
    fmt.bitDepth     = 16;
    fmt.subslotBytes = 2;
    fmt.isFloat      = false;
    if (!sink_.configure(fmt) || !sink_.start()) {
        LOGE("sink_ configure/start failed");
        decoder_.close();
        return;
    }

    playingIndex_ = static_cast<int>(index);
    playing_      = true;

    // Decoder::startAsync() runs cb on its own dedicated decode thread — one
    // at a time (stopPlayback() joins it via decoder_.stop() before a new
    // playTrack() call reuses dither_/i16Scratch_), so touching those members
    // here without a lock is safe.
    decoder_.startAsync([this](const float* data, int numSamples) {
        if (numSamples <= 0) return;
        i16Scratch_.resize(static_cast<size_t>(numSamples));
        floatToInt16Dither(data, i16Scratch_.data(), numSamples, dither_);

        const auto* bytes = reinterpret_cast<const uint8_t*>(i16Scratch_.data());
        int total = numSamples * 2;
        int off   = 0;
        while (off < total) {
            int written = sink_.write(bytes + off, total - off);
            if (written <= 0) break;  // stopped mid-write
            off += written;
        }
    });

    LOGI("playing [%zu] %s (%d Hz, %d ch)", index, t.filePath.c_str(),
         fmt.sampleRate, fmt.channels);
}

void AndroidPlayerView::stopPlayback() {
    if (!playing_) return;
    decoder_.stop();
    sink_.stop();
    decoder_.close();
    playing_      = false;
    playingIndex_ = -1;
}

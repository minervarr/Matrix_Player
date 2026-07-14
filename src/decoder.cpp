#include "decoder.h"
#include <cstdio>
#include <algorithm>
#include <vector>
#include <string>
#ifdef _WIN32
#include <windows.h>
#include <avrt.h>
#endif

// dr_flac.h/dr_wav.h are vendored single-header third-party code (mackron/dr_libs,
// see CLAUDE.md) — their own internal narrowing conversions aren't ours to fix
// (same reasoning as sqlite3's /W0 in CMakeLists.txt, just scoped to the include
// instead of a whole target since these are header-only).
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#define DR_FLAC_IMPLEMENTATION
#include "../libs/thirdparty/dr_flac.h"

#define DR_WAV_IMPLEMENTATION
#include "../libs/thirdparty/dr_wav.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

struct Decoder::Impl {
    drflac* flac = nullptr;
    drwav   wav  = {};
    bool    wavOpen = false;
};

Decoder::Decoder() : impl_(new Impl) {}

Decoder::~Decoder() {
    close();
    delete impl_;
}

static std::string fileExt(const std::string& path) {
    auto pos = path.rfind('.');
    if (pos == std::string::npos) return "";
    std::string ext = path.substr(pos);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext;
}

static std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    return w;
}

bool Decoder::open(const std::string& path) {
    close();
    seekTarget_.store(-1, std::memory_order_relaxed);
    std::string ext = fileExt(path);
    std::wstring wpath = utf8ToWide(path);

    if (ext == ".wav") {
        if (!drwav_init_file_w(&impl_->wav, wpath.c_str(), nullptr)) {
            printf("[Decoder][ERROR] drwav_init_file_w FAILED: '%s'\n", path.c_str());
            fflush(stdout);
            return false;
        }
        impl_->wavOpen = true;
        sampleRate_    = (int)impl_->wav.sampleRate;
        channels_      = (int)impl_->wav.channels;
        totalFrames_   = (int64_t)impl_->wav.totalPCMFrameCount;
        bitsPerSample_ = (int)impl_->wav.bitsPerSample;
    } else {
        impl_->flac = drflac_open_file_w(wpath.c_str(), nullptr);
        if (!impl_->flac) {
            printf("[Decoder][ERROR] drflac_open_file_w FAILED: '%s'\n", path.c_str());
            fflush(stdout);
            return false;
        }
        sampleRate_    = (int)impl_->flac->sampleRate;
        channels_      = (int)impl_->flac->channels;
        totalFrames_   = (int64_t)impl_->flac->totalPCMFrameCount;
        bitsPerSample_ = (int)impl_->flac->bitsPerSample;
    }
    return true;
}

void Decoder::close() {
    stop();
    if (impl_->flac) {
        drflac_close(impl_->flac);
        impl_->flac = nullptr;
    }
    if (impl_->wavOpen) {
        drwav_uninit(&impl_->wav);
        impl_->wavOpen = false;
    }
    sampleRate_ = 0;
    channels_ = 0;
    totalFrames_ = 0;
    bitsPerSample_ = 0;
}

void Decoder::startAsync(PcmCallback cb) {
    stop();
    running_.store(true);
    thread_ = std::thread([this, cb]{
#ifdef _WIN32
        DWORD taskIndex = 0;
        HANDLE hTask = AvSetMmThreadCharacteristics(L"Pro Audio", &taskIndex);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#endif
        decodeLoop(cb);
#ifdef _WIN32
        if (hTask) AvRevertMmThreadCharacteristics(hTask);
#endif
    });
}

void Decoder::startAsyncInt32(PcmS32Callback cb) {
    stop();
    running_.store(true);
    thread_ = std::thread([this, cb]{
#ifdef _WIN32
        DWORD taskIndex = 0;
        HANDLE hTask = AvSetMmThreadCharacteristics(L"Pro Audio", &taskIndex);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#endif
        decodeLoopInt32(cb);
#ifdef _WIN32
        if (hTask) AvRevertMmThreadCharacteristics(hTask);
#endif
    });
}

void Decoder::stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
}

void Decoder::seekMs(int positionMs) {
    if (sampleRate_ == 0) return;
    seekTarget_.store(positionMs);
}

void Decoder::decodeLoop(PcmCallback cb) {
    static const int CHUNK_FRAMES = 4096;
    std::vector<float> buf(CHUNK_FRAMES * channels_);

    while (running_.load()) {
        int seekMs = seekTarget_.exchange(-1);
        if (seekMs >= 0) {
            uint64_t frame = (uint64_t)seekMs * sampleRate_ / 1000;
            if (impl_->flac)
                drflac_seek_to_pcm_frame(impl_->flac, frame);
            else if (impl_->wavOpen)
                drwav_seek_to_pcm_frame(&impl_->wav, frame);
        }

        drwav_uint64 got = 0;
        if (impl_->flac)
            got = drflac_read_pcm_frames_f32(impl_->flac, CHUNK_FRAMES, buf.data());
        else if (impl_->wavOpen)
            got = drwav_read_pcm_frames_f32(&impl_->wav, CHUNK_FRAMES, buf.data());

        if (got == 0) {
            cb(nullptr, 0);
            running_.store(false);
            if (doneCallback_) doneCallback_();
            break;
        }

        cb(buf.data(), (int)(got * channels_));
    }
}

void Decoder::decodeLoopInt32(PcmS32Callback cb) {
    static const int CHUNK_FRAMES = 4096;
    std::vector<int32_t> buf(CHUNK_FRAMES * channels_);

    while (running_.load()) {
        int seekMs = seekTarget_.exchange(-1);
        if (seekMs >= 0) {
            uint64_t frame = (uint64_t)seekMs * sampleRate_ / 1000;
            if (impl_->flac)
                drflac_seek_to_pcm_frame(impl_->flac, frame);
            else if (impl_->wavOpen)
                drwav_seek_to_pcm_frame(&impl_->wav, frame);
        }

        drwav_uint64 got = 0;
        if (impl_->flac)
            got = drflac_read_pcm_frames_s32(impl_->flac, CHUNK_FRAMES, buf.data());
        else if (impl_->wavOpen)
            got = drwav_read_pcm_frames_s32(&impl_->wav, CHUNK_FRAMES, buf.data());

        if (got == 0) {
            cb(nullptr, 0);
            running_.store(false);
            if (doneCallback_) doneCallback_();
            break;
        }

        cb(buf.data(), (int)(got * channels_));
    }
}

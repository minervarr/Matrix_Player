#include "core/decoder.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <memory>
#include <vector>
#include <string>
#ifdef _WIN32
#include <windows.h>
#include <avrt.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

// Decoding runs through audio_engine's own backends — libFLAC, libmpg123 and
// the DFF/DSF parsers — rather than a single-header library, so this player
// decodes byte-identically to the Android sibling that already used them.
// They implement ae::Decoder (fd in, interleaved PCM bytes out).
// Each is gated on the engine having produced that target (see core/
// CMakeLists.txt): libFLAC is a submodule and libmpg123 is a download, so a
// clone missing either still builds a player — it just won't open that format.
#include "core/decoder.hpp"
#if MATRIX_HAVE_FLAC
#include "flac_decoder.h"
#endif
#if MATRIX_HAVE_MP3
#include "mp3_decoder.h"
#endif
#if MATRIX_HAVE_DSD
#include "dsd_decoder.h"
#endif

// dr_wav stays: the engine has no WAV decoder, and WAV is a container this
// player still opens. It is vendored single-header third-party code
// (mackron/dr_libs), so its own narrowing conversions aren't ours to fix —
// same reasoning as sqlite3's /W0, scoped to the include since it's a header.
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace {

// Open a UTF-8 path as a read-only binary fd. The engine's decoders take an
// fd rather than a path (their Android origin: a content:// Uri only ever
// resolves to one), so this is where a portable path turns into one.
// _O_BINARY matters on Windows: a text-mode fd would mangle 0x1A bytes.
int openBinary(const std::string& path) {
#ifdef _WIN32
    int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (n <= 0) return -1;
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w.data(), n);
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    int fd = -1;
    if (_wsopen_s(&fd, w.c_str(), _O_RDONLY | _O_BINARY, _SH_DENYNO, _S_IREAD) != 0)
        return -1;
    return fd;
#else
    return ::open(path.c_str(), O_RDONLY);
#endif
}

void closeFd(int& fd) {
    if (fd < 0) return;
#ifdef _WIN32
    _close(fd);
#else
    ::close(fd);
#endif
    fd = -1;
}

// Read `n` bytes from the very start of `fd` without disturbing its offset.
int peekMagic(int fd, unsigned char* out, int n) {
#ifdef _WIN32
    // No pread on Windows: seek, read, seek back. Nothing else holds this fd
    // yet (the decoders dup() it later), so the round trip is safe.
    long saved = _lseek(fd, 0, SEEK_CUR);
    if (_lseek(fd, 0, SEEK_SET) < 0) return 0;
    int got = _read(fd, out, (unsigned)n);
    _lseek(fd, saved, SEEK_SET);
    return got < 0 ? 0 : got;
#else
    ssize_t got = ::pread(fd, out, (size_t)n, 0);
    return got < 0 ? 0 : (int)got;
#endif
}

bool eq4(const unsigned char* m, const char* s) {
    return std::memcmp(m, s, 4) == 0;
}

// Mirrors ae's own sniffer (api/src/audio_engine.cpp): an ID3v2 tag, or a raw
// MPEG *Layer III* frame header — deliberately strict about the layer bits so
// it never steals ADTS-AAC, which shares the 0xFFE sync word.
bool sniffMp3(const unsigned char* m, int n) {
    if (n >= 3 && m[0] == 'I' && m[1] == 'D' && m[2] == '3') return true;
    if (n >= 2 && m[0] == 0xFF && (m[1] & 0xE0) == 0xE0) {
        int version = (m[1] >> 3) & 0x03;   // 01 = reserved
        int layer   = (m[1] >> 1) & 0x03;   // 01 = Layer III
        if (version != 0x01 && layer == 0x01) return true;
    }
    return false;
}

} // namespace

struct Decoder::Impl {
    // Engine-backed path (FLAC / MP3 / DSD).
    std::unique_ptr<ae::Decoder> dec;
    ae::AudioFormat              fmt{};
    int                          fd = -1;
    std::vector<uint8_t>         raw;      // read staging, sized per chunk

    // WAV path (dr_wav), mutually exclusive with the above.
    drwav wav = {};
    bool  wavOpen = false;
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

#ifdef _WIN32
static std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    return w;
}
#endif

bool Decoder::open(const std::string& path) {
    close();
    seekTarget_.store(-1, std::memory_order_relaxed);

    // Format is decided by MAGIC BYTES, not by extension: the library is full
    // of files whose names disagree with their contents. The extension is
    // consulted only for WAV, whose RIFF header is checked anyway.
    int fd = openBinary(path);
    if (fd < 0) {
        printf("[Decoder][ERROR] cannot open '%s'\n", path.c_str());
        fflush(stdout);
        return false;
    }
    unsigned char magic[4] = {0};
    int got = peekMagic(fd, magic, 4);

    if (got == 4 && eq4(magic, "RIFF")) {
        // WAV: dr_wav owns its own file handle, so this fd has done its job.
        closeFd(fd);
#ifdef _WIN32
        std::wstring wpath = utf8ToWide(path);
        bool opened = drwav_init_file_w(&impl_->wav, wpath.c_str(), nullptr);
#else
        bool opened = drwav_init_file(&impl_->wav, path.c_str(), nullptr);
#endif
        if (!opened) {
            printf("[Decoder][ERROR] drwav_init_file FAILED: '%s'\n", path.c_str());
            fflush(stdout);
            return false;
        }
        impl_->wavOpen = true;
        sampleRate_    = (int)impl_->wav.sampleRate;
        channels_      = (int)impl_->wav.channels;
        totalFrames_   = (int64_t)impl_->wav.totalPCMFrameCount;
        bitsPerSample_ = (int)impl_->wav.bitsPerSample;
        return true;
    }

    std::unique_ptr<ae::Decoder> dec;
    const char* want = nullptr;
    if (got == 4 && eq4(magic, "fLaC")) {
        want = "FLAC";
#if MATRIX_HAVE_FLAC
        dec.reset(new ae::FlacDecoder());
#endif
    } else if (got == 4 && (eq4(magic, "DSD ") || eq4(magic, "FRM8"))) {
        want = "DSD";                      // DSF and DFF both; it re-sniffs
#if MATRIX_HAVE_DSD
        dec.reset(new ae::DsdDecoder());
#endif
    } else if (sniffMp3(magic, got)) {
        want = "MP3";
#if MATRIX_HAVE_MP3
        dec.reset(new ae::Mp3Decoder());
#endif
    }

    if (!dec) {
        if (want)
            printf("[Decoder][ERROR] this build has no %s decoder: '%s'\n", want, path.c_str());
        else
            printf("[Decoder][ERROR] unrecognized container (magic %02X %02X %02X %02X): '%s'\n",
                   magic[0], magic[1], magic[2], magic[3], path.c_str());
        fflush(stdout);
        closeFd(fd);
        return false;
    }

    // length < 0 = "to end of file". The decoders dup() the fd when they need
    // it to outlive this call, but they do NOT take ownership — we hold ours
    // until close(), exactly as ae::Decoder::open documents.
    if (!dec->open(fd, 0, -1)) {
        printf("[Decoder][ERROR] decoder open FAILED: '%s'\n", path.c_str());
        fflush(stdout);
        closeFd(fd);
        return false;
    }

    impl_->fd  = fd;
    impl_->dec = std::move(dec);
    impl_->fmt = impl_->dec->format();

    sampleRate_    = impl_->fmt.sampleRate;
    channels_      = impl_->fmt.channels;
    bitsPerSample_ = impl_->fmt.bitDepth;
    // ae::Decoder reports duration, not a frame count; the app wants frames.
    int64_t ms = impl_->dec->durationMs();
    totalFrames_ = (ms > 0 && sampleRate_ > 0) ? ms * sampleRate_ / 1000 : 0;

    if (!impl_->fmt.valid()) {
        printf("[Decoder][ERROR] decoder reported an invalid format: '%s'\n", path.c_str());
        fflush(stdout);
        close();
        return false;
    }
    return true;
}

void Decoder::close() {
    stop();
    if (impl_->dec) {
        impl_->dec->close();
        impl_->dec.reset();
    }
    closeFd(impl_->fd);
    impl_->fmt = {};
    impl_->raw.clear();
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

namespace {

// ae::Decoder hands back raw interleaved samples in its format's subslot
// width; both of this class's callbacks want one fixed encoding. These two
// converters are the whole impedance match.
//
// LEFT-JUSTIFIED is the contract for the int32 path (see decoder.h): a 16-bit
// sample becomes s<<16, a 24-bit one s<<8, so the top bits of the 32-bit word
// always carry the signal and a UAC2 DAC's int32 write path is losslessly fed
// whatever the source depth. This reproduces dr_flac's own s32 output exactly,
// which is what the bit-perfect path was built around.
int32_t widenToS32(const uint8_t* p, int subslotBytes) {
    switch (subslotBytes) {
    case 2: {
        int16_t v;
        std::memcpy(&v, p, 2);
        return (int32_t)v << 16;
    }
    case 3: {
        // Packed 24-bit little-endian. Assemble into the TOP three bytes so
        // the sign lands in bit 31 — that is the sign extension, no branch.
        return (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 |
                         (uint32_t)p[2] << 24);
    }
    case 4: {
        int32_t v;
        std::memcpy(&v, p, 4);
        return v;
    }
    default:
        return 0;
    }
}

// Turns ae::Decoder's byte-oriented read() into whole interleaved FRAMES of
// left-justified int32.
//
// Two things make this more than a cast. read() returns bytes and promises no
// frame alignment (FlacDecoder::servePending hands back min(remaining, maxLen),
// and `remaining` is whatever is left of a decoded block), while both PCM
// callbacks are specified as numSamples = frames * channels — the Reference EQ
// path feeds that straight into soxr, which reads per frame. So a partial frame
// is carried at the head of `raw` and completed by the next read instead of
// being emitted. And "no data this cycle" (0) stays distinct from "stream over"
// (-1); conflating them is what broke gapless advance.
struct FrameReader {
    ae::Decoder*          dec;
    int                   subslot;
    int                   channels;
    std::vector<uint8_t>* raw;
    size_t                carry = 0;   // bytes of an incomplete frame at raw[0]

    void reset() { carry = 0; }        // after a seek: stale bytes are wrong now

    // >0 = samples written, 0 = nothing this cycle (call again), -1 = EOS.
    int next(int32_t* out, int maxSamples) {
        const size_t frameBytes = (size_t)subslot * channels;
        int n = dec->read(raw->data() + carry, (int)(raw->size() - carry));
        if (n < 0) return -1;
        size_t have  = carry + (size_t)n;
        size_t whole = (have / frameBytes) * frameBytes;
        if (whole == 0) {
            // Either the decoder gave us nothing, or not yet a full frame.
            carry = have;
            return 0;
        }
        int samples = (int)(whole / subslot);
        if (samples > maxSamples) samples = maxSamples;   // can't happen; cheap guard
        for (int i = 0; i < samples; i++)
            out[i] = widenToS32(&(*raw)[(size_t)i * subslot], subslot);
        carry = have - whole;
        if (carry) std::memmove(raw->data(), raw->data() + whole, carry);
        return samples;
    }
};

} // namespace

void Decoder::decodeLoop(PcmCallback cb) {
    constexpr int CHUNK_FRAMES = 4096;

    // END-OF-STREAM IS NOT THE SAME AS STOPPED. doneCallback_ is what drives
    // the gapless coordinator (PlayerWindow::startGaplessCoordinator), and
    // prepareNextTrack() routinely close()s the idle decoder — which still has
    // a callback installed from when it was active. Firing on a stop()-driven
    // exit therefore reads as a phantom track-end and sends playback running
    // away through the album. Only a real EOS may signal.
    bool reachedEos = false;

    if (impl_->wavOpen) {
        std::vector<float> buf(CHUNK_FRAMES * channels_);
        while (running_.load()) {
            int seekMs = seekTarget_.exchange(-1);
            if (seekMs >= 0)
                drwav_seek_to_pcm_frame(&impl_->wav,
                                        (uint64_t)seekMs * sampleRate_ / 1000);
            drwav_uint64 got = drwav_read_pcm_frames_f32(&impl_->wav, CHUNK_FRAMES, buf.data());
            if (got == 0) { reachedEos = true; break; }
            cb(buf.data(), (int)(got * channels_));
        }
    } else if (impl_->dec) {
        const int subslot = impl_->fmt.subslotBytes;
        impl_->raw.assign((size_t)CHUNK_FRAMES * channels_ * subslot, 0);
        std::vector<float>   buf(CHUNK_FRAMES * channels_);
        std::vector<int32_t> wide(CHUNK_FRAMES * channels_);
        FrameReader fr{ impl_->dec.get(), subslot, channels_, &impl_->raw };

        while (running_.load()) {
            int seekMs = seekTarget_.exchange(-1);
            if (seekMs >= 0) { impl_->dec->seekMs(seekMs); fr.reset(); }

            int samples = fr.next(wide.data(), (int)wide.size());
            if (samples < 0) { reachedEos = true; break; }
            if (samples == 0) { std::this_thread::yield(); continue; }

            // Normalize from the left-justified word, so every source depth
            // maps to [-1, 1) by the same divisor.
            for (int i = 0; i < samples; i++)
                buf[(size_t)i] = (float)wide[(size_t)i] / 2147483648.0f;
            cb(buf.data(), samples);
        }
    }

    if (reachedEos) {
        cb(nullptr, 0);
        running_.store(false);
        if (doneCallback_) doneCallback_();
    }
}

void Decoder::decodeLoopInt32(PcmS32Callback cb) {
    constexpr int CHUNK_FRAMES = 4096;

    // See decodeLoop(): only a genuine end-of-stream may fire doneCallback_.
    bool reachedEos = false;

    if (impl_->wavOpen) {
        std::vector<int32_t> buf(CHUNK_FRAMES * channels_);
        while (running_.load()) {
            int seekMs = seekTarget_.exchange(-1);
            if (seekMs >= 0)
                drwav_seek_to_pcm_frame(&impl_->wav,
                                        (uint64_t)seekMs * sampleRate_ / 1000);
            drwav_uint64 got = drwav_read_pcm_frames_s32(&impl_->wav, CHUNK_FRAMES, buf.data());
            if (got == 0) { reachedEos = true; break; }
            cb(buf.data(), (int)(got * channels_));
        }
    } else if (impl_->dec) {
        const int subslot = impl_->fmt.subslotBytes;
        impl_->raw.assign((size_t)CHUNK_FRAMES * channels_ * subslot, 0);
        std::vector<int32_t> buf(CHUNK_FRAMES * channels_);
        FrameReader fr{ impl_->dec.get(), subslot, channels_, &impl_->raw };

        while (running_.load()) {
            int seekMs = seekTarget_.exchange(-1);
            if (seekMs >= 0) { impl_->dec->seekMs(seekMs); fr.reset(); }

            int samples = fr.next(buf.data(), (int)buf.size());
            if (samples < 0) { reachedEos = true; break; }
            if (samples == 0) { std::this_thread::yield(); continue; }
            cb(buf.data(), samples);
        }
    }

    if (reachedEos) {
        cb(nullptr, 0);
        running_.store(false);
        if (doneCallback_) doneCallback_();
    }
}

#pragma once
#include <string>
#include <functional>
#include <atomic>
#include <thread>

// TODO(FFmpeg): Swap this interface implementation for FFmpeg when formats beyond
// FLAC are needed (DSF/DFF containers, MP3, WAV, etc.). The interface stays the same.

// Called from the decode thread with interleaved float32 PCM samples.
// numSamples = total samples across all channels (frames * channels).
using PcmCallback = std::function<void(const float* data, int numSamples)>;

// Bit-perfect variant: interleaved signed 32-bit PCM, left-justified to the full
// 32-bit range (dr_flac/dr_wav s32 output shifts each sample left by 32-bitsPerSample,
// a lossless upscale of any 16/24-bit source). Feed straight to a UAC2 DAC's int32
// write path for a mathematically lossless wire — no float, no rounding.
using PcmS32Callback = std::function<void(const int32_t* data, int numSamples)>;

class Decoder {
public:
    Decoder();
    ~Decoder();

    bool open(const std::string& path);
    void close();

    int  sampleRate()    const { return sampleRate_; }
    int  channels()      const { return channels_; }
    int64_t totalFrames()   const { return totalFrames_; }
    int  bitsPerSample() const { return bitsPerSample_; }

    // Start decode loop on a background thread, calling cb with PCM chunks.
    void startAsync(PcmCallback cb);
    // Bit-perfect decode loop: emits left-justified int32 chunks (see PcmS32Callback).
    void startAsyncInt32(PcmS32Callback cb);
    void stop();

    // Optional callback fired from the decode thread when EOF is reached.
    // Use to trigger gapless pre-roll of the next track.
    void setDoneCallback(std::function<void()> cb) { doneCallback_ = std::move(cb); }

    // Seek to position in milliseconds. Thread-safe.
    void seekMs(int positionMs);

    bool isRunning() const { return running_.load(); }

private:
    void decodeLoop(PcmCallback cb);
    void decodeLoopInt32(PcmS32Callback cb);

    struct Impl;
    Impl* impl_ = nullptr;

    int sampleRate_    = 0;
    int channels_      = 0;
    int64_t totalFrames_   = 0;
    int bitsPerSample_ = 0;

    std::thread              thread_;
    std::atomic<bool>        running_{false};
    std::atomic<int>         seekTarget_{-1}; // -1 = no pending seek
    std::function<void()>    doneCallback_;
};

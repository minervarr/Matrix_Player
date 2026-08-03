#pragma once
// Secondary Linux output backend — parallels WasapiOutput's role on Windows
// (USB direct stays primary/bit-perfect everywhere). Thin AudioOutput adapter
// over audio_engine's AlsaSink (backends/alsa/alsa_sink.h), which already
// does the ALSA hw: negotiation and blocking snd_pcm_writei loop; this class
// only repacks left-justified 32-bit samples into whatever wire format
// AlsaSink negotiated (S32_LE/S24_3LE/S16_LE).
#include "audio_output.h"
#include "alsa_sink.h"
#include <string>
#include <vector>

struct AlsaDeviceInfo {
    std::string deviceId;   // e.g. "hw:1,0"
    std::string name;       // "<card name> — <device name>"
};

class AlsaOutput : public AudioOutput {
public:
    explicit AlsaOutput(std::string deviceId = "default") : deviceId_(std::move(deviceId)) {}

    // Playback-capable ALSA devices, direct hw: identifiers. Does not include
    // "default" — that's a UI-level pseudo-entry (mirrors WasapiOutput's
    // "(Default device)" convention), not a real card/device pair.
    static std::vector<AlsaDeviceInfo> enumerateDevices();

    bool configure(int rate, int channels, int bitDepth, bool strictBitperfect = false) override;
    bool start() override;

    // Rates this card actually accepts, asked of the driver rather than assumed.
    // Without it AudioOutput's default returns {} and pickOutputRate() falls
    // back to a hardcoded 48000 for every device in existence — a guess that
    // happens to be right often enough to hide that it is a guess.
    std::vector<int> probeRates(int channels) const override;
    // ALSA's own words for why configure() failed, e.g. "Device or resource
    // busy" when PipeWire is holding the card. Shown on screen by PlayerWindow.
    std::string lastError() const override { return lastError_; }

    int  writeFloat32(const float* data, int numSamples) override;
    int  writeInt32(const int32_t* data, int numSamples) override;
    void stop() override;
    void close() override;
    int  getConfiguredRate()     const override { return fmt_.sampleRate; }
    int  getConfiguredChannels() const override { return fmt_.channels; }

    // Same six AudioOutput no-ops JackOutput had to implement (audio_output.h:32-50).
    // The device buffer is this backend's only cushion, so these report on it
    // directly rather than on an application ring.
    void   flush() override;
    int    pendingPlaybackMs() const override;
    bool   hasFaulted() const override;
    size_t ringAvailable() const override;

private:
    std::string deviceId_;
    std::string lastError_;
    AlsaSink sink_;
    ae::AudioFormat fmt_{};
    std::vector<uint8_t> wireBuf_;
    // Reused by writeFloat32 so the decode thread never allocates mid-track.
    std::vector<int32_t> floatConvBuf_;
};

#pragma once
// Secondary Linux output backend, alongside AlsaOutput. Thin AudioOutput
// adapter over audio_engine's JackSink (backends/jack/jack_sink.h), which
// already owns the realtime process callback and its own lock-free ring.
#include "audio_output.h"
#include "jack_sink.h"
#include <string>
#include <vector>

class JackOutput : public AudioOutput {
public:
    // startPort selects the first physical playback port to connect to (the
    // remaining channels connect to the ports immediately after it in
    // enumeratePorts()'s order). Empty (the default) auto-connects to the
    // first N physical ports, same as before this class had selection.
    explicit JackOutput(std::string startPort = "") : startPort_(std::move(startPort)) {}

    // Physical playback ports in the running server's graph. Opens the
    // client if not already open, so this can be called before configure()
    // (e.g. to populate the Audio Settings panel's device list).
    std::vector<JackPlaybackPortInfo> enumeratePorts();

    bool configure(int rate, int channels, int bitDepth, bool strictBitperfect = false) override;
    bool start() override;
    int  writeFloat32(const float* data, int numSamples) override;
    int  writeFloat32Blocking(const float* data, int numSamples, int timeoutMs = 500) override;
    void stop() override;
    void close() override;
    int  getConfiguredRate()     const override { return sink_.activeFormat().sampleRate; }
    int  getConfiguredChannels() const override { return channels_; }

private:
    bool ensureClientOpen();

    JackSink sink_;
    bool clientOpen_ = false;
    int  channels_   = 0;
    std::string startPort_;
};

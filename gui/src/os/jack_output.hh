#pragma once
// Secondary Linux output backend, alongside AlsaOutput. Thin AudioOutput
// adapter over audio_engine's JackSink (backends/jack/jack_sink.h), which
// already owns the realtime process callback and its own lock-free ring —
// this class just forwards float32 samples and auto-connects to the first
// available physical playback ports (see JackSink::start()).
#include "audio_output.h"
#include "jack_sink.h"

class JackOutput : public AudioOutput {
public:
    JackOutput() = default;

    bool configure(int rate, int channels, int bitDepth, bool strictBitperfect = false) override;
    bool start() override;
    int  writeFloat32(const float* data, int numSamples) override;
    void stop() override;
    void close() override;
    int  getConfiguredRate()     const override { return sink_.activeFormat().sampleRate; }
    int  getConfiguredChannels() const override { return channels_; }

private:
    JackSink sink_;
    bool clientOpen_ = false;
    int  channels_   = 0;
};

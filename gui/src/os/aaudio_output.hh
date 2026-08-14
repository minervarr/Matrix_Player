#pragma once
// Android's on-device output backend — the same role ALSA plays on Linux and
// WASAPI on Windows: what the app plays through when there is no USB DAC. Thin
// AudioOutput adapter over audio_engine's AAudioSink
// (backends/aaudio/aaudio_sink.h), built the same way alsa_output.hh wraps
// AlsaSink.
//
// Two things are different from the ALSA adapter, and both are AAudio's doing,
// not simplifications:
//
//  1. There is NO device enumeration and no device id. AAudio picks the route
//     itself and follows the system's own choice — the listener plugs in
//     headphones and the stream moves. A device list here would be a list the
//     app cannot honour.
//  2. The speaker stream is ALWAYS 16-bit (AAUDIO_FORMAT_PCM_I16), so this can
//     never be a bit-perfect path for a 24-bit source: AAudioSink dithers down
//     to 16 with persistent TPDF. strictBitperfect is therefore refused
//     outright rather than accepted and quietly not honoured — the same rule
//     the ALSA adapter applies to a rate the card snapped away from.
#include "audio_output.h"
#include "aaudio_sink.h"
#include <string>
#include <vector>

class AAudioOutput : public AudioOutput {
public:
    bool configure(int rate, int channels, int bitDepth, bool strictBitperfect = false) override;
    bool start() override;

    int  writeFloat32(const float* data, int numSamples) override;
    int  writeInt32(const int32_t* data, int numSamples) override;

    void stop() override;
    void close() override;

    int  getConfiguredRate()     const override { return fmt_.sampleRate; }
    int  getConfiguredChannels() const override { return fmt_.channels; }

    std::string lastError() const override { return lastError_; }

    void flush() override;
    int  pendingPlaybackMs() const override;

private:
    std::string     lastError_;
    ae::AAudioSink  sink_;
    ae::AudioFormat fmt_{};
    // Reused so the decode thread never allocates mid-track — same reason
    // AlsaOutput keeps floatConvBuf_.
    std::vector<int16_t> i16Buf_;
};

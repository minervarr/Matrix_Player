#pragma once
// Secondary Linux output backend — parallels WasapiOutput's role on Windows
// (USB direct stays primary/bit-perfect everywhere). Thin AudioOutput adapter
// over audio_engine's AlsaSink (backends/alsa/alsa_sink.h), which already
// does the ALSA hw: negotiation and blocking snd_pcm_writei loop; this class
// only repacks left-justified 32-bit samples into whatever wire format
// AlsaSink negotiated (S32_LE/S24_3LE/S16_LE).
#include "audio_output.h"
#include "alsa_sink.h"
#include <vector>

class AlsaOutput : public AudioOutput {
public:
    AlsaOutput() = default;

    bool configure(int rate, int channels, int bitDepth, bool strictBitperfect = false) override;
    bool start() override;
    int  writeFloat32(const float* data, int numSamples) override;
    int  writeInt32(const int32_t* data, int numSamples) override;
    void stop() override;
    void close() override;
    int  getConfiguredRate()     const override { return fmt_.sampleRate; }
    int  getConfiguredChannels() const override { return fmt_.channels; }

private:
    AlsaSink sink_;
    ae::AudioFormat fmt_{};
    std::vector<uint8_t> wireBuf_;
};

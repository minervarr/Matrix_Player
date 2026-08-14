#include "aaudio_output.hh"

#include <algorithm>
#include <cstdint>
#include <string>

bool AAudioOutput::configure(int rate, int channels, int bitDepth, bool strictBitperfect) {
    lastError_.clear();

    // The stream is opened as AAUDIO_FORMAT_PCM_I16 and nothing else
    // (aaudio_sink.cpp), so a 24-bit source cannot reach the speaker
    // unchanged. Say so instead of accepting the request and silently
    // dithering — "bit-perfect" that isn't is worse than a refusal, because
    // the listener has no way to notice.
    if (strictBitperfect && bitDepth != 16) {
        lastError_ = "AAudio plays 16-bit only; " + std::to_string(bitDepth) +
                     "-bit cannot be bit-perfect on this output";
        return false;
    }

    // subslotBytes = 2 puts AAudioSink on its 16-bit passthrough branch. The
    // depth conversion is done HERE, in writeInt32, for the same reason the
    // ALSA adapter does it: the app's own pipeline has already quantized and
    // dithered to the wire depth, and letting the sink dither a second time
    // would be two noise shapers in series.
    ae::AudioFormat req{};
    req.sampleRate   = rate;
    req.channels     = channels;
    req.bitDepth     = 16;
    req.subslotBytes = 2;
    req.isFloat      = false;
    if (!sink_.configure(req)) {
        lastError_ = "AAudio rejected " + std::to_string(rate) + " Hz / " +
                     std::to_string(channels) + " ch";
        return false;
    }
    fmt_ = sink_.activeFormat();

    // AAudio grants a rate rather than negotiating one: ask for 192k on a
    // handset and you get 48k back with no error. The caller resamples to
    // getConfiguredRate(), so this only has to be true, not equal.
    if (strictBitperfect && fmt_.sampleRate != rate) {
        lastError_ = "AAudio runs " + std::to_string(fmt_.sampleRate) +
                     " Hz, not the requested " + std::to_string(rate) + " Hz";
        return false;
    }
    return true;
}

bool AAudioOutput::start() { return sink_.start(); }

int AAudioOutput::writeInt32(const int32_t* data, int numSamples) {
    if (numSamples <= 0) return 0;
    if ((int)i16Buf_.size() < numSamples) i16Buf_.resize((size_t)numSamples);
    // Left-justified 32-bit -> S16: the top 16 bits, exactly as the ALSA
    // adapter's S16_LE branch takes them.
    for (int i = 0; i < numSamples; ++i)
        i16Buf_[i] = (int16_t)((uint32_t)data[i] >> 16);

    int wrote = sink_.write(reinterpret_cast<const uint8_t*>(i16Buf_.data()),
                            numSamples * 2);
    return wrote > 0 ? wrote / 2 : wrote;
}

int AAudioOutput::writeFloat32(const float* data, int numSamples) {
    if (numSamples <= 0) return 0;
    if ((int)i16Buf_.size() < numSamples) i16Buf_.resize((size_t)numSamples);
    for (int i = 0; i < numSamples; ++i) {
        float f = std::clamp(data[i], -1.0f, 1.0f);
        i16Buf_[i] = (int16_t)(f * 32767.0f);
    }
    int wrote = sink_.write(reinterpret_cast<const uint8_t*>(i16Buf_.data()),
                            numSamples * 2);
    return wrote > 0 ? wrote / 2 : wrote;
}

void AAudioOutput::flush() { sink_.flush(); }

int AAudioOutput::pendingPlaybackMs() const { return sink_.pendingPlaybackMs(); }

void AAudioOutput::stop() { sink_.stop(); }

// AAudioSink has no close() of its own: the stream is torn down by
// configure()'s closeStream() and by the destructor. stop() is what actually
// releases the device here, and it has already run — applyAudioSettingsPanel()
// and shutdown() both call stop() then close(), by name and in that order.
void AAudioOutput::close() {}

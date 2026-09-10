#include "aaudio_output.hh"

#include <algorithm>
#include <cstdint>
#include <string>

bool AAudioOutput::configure(int rate, int channels, int bitDepth, bool strictBitperfect) {
    lastError_.clear();
    (void)bitDepth;

    // ── AAudio can never be bit-perfect, so it refuses outright ──────────────
    //
    // This used to refuse only two specific mismatches — a source deeper than
    // 16-bit, and a rate the system granted differently — and let everything
    // else through as EXACT. A 16/44.1 track that the phone happened to grant
    // at 44.1 therefore passed every check and lit the BITPERFECT badge, which
    // was untrue for two independent reasons, either one of which is fatal:
    //
    //   1. AAudioSink opens AAUDIO_SHARING_MODE_SHARED (aaudio_sink.cpp), so
    //      every sample goes through AudioFlinger's mixer and its volume stage.
    //      Measured on a moto g06: the mixer thread runs at 48000 Hz with a
    //      PCM_FLOAT processing format, so a 44.1 kHz stream is resampled and
    //      scaled downstream of anything this app can see.
    //   2. When the route is a pair of Bluetooth headphones — which on a phone
    //      is the common case, and the one this was reported from — what leaves
    //      the device is SBC, AAC, aptX or LDAC. Every one of those is a LOSSY
    //      ENCODE. LDAC at its highest 990 kbps is still lossy, and a CD-rate
    //      FLAC passage can exceed that rate outright.
    //
    // The Linux Bluetooth backend has always said this (bt_output.cc:61); the
    // phone said the opposite about the same headphones, because Android
    // reaches A2DP through AAudio and MATRIX_HAVE_BLUETOOTH is Linux-only.
    // Refusing here is what makes bit-perfect mode mean one thing on all three
    // platforms. On a phone it leaves USB and AOAS, which are genuinely
    // sample-exact, as the ways to have it.
    //
    // Refusing rather than degrading is the listener's own instruction: a path
    // that is not bit-perfect must say so and not play. onPlay() turns this
    // false into the audio notice, using lastError() as its words.
    if (strictBitperfect) {
        lastError_ = "Bit-perfect is not possible through Android's audio "
                     "system — it mixes and resamples every stream, and a "
                     "Bluetooth route re-encodes it. Use Reference EQ, or a "
                     "USB DAC.";
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
    // getConfiguredRate(), so this only has to be true, not equal — and there
    // is no strict case left to check here, because the block at the top of
    // this function has already refused every one of them.
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

#include "alsa_output.hh"
#include <algorithm>
#include <cstdint>

bool AlsaOutput::configure(int rate, int channels, int bitDepth, bool strictBitperfect) {
    if (!sink_.open("default")) return false;
    ae::AudioFormat req{};
    req.sampleRate   = rate;
    req.channels     = channels;
    req.bitDepth     = bitDepth;
    req.subslotBytes = (bitDepth + 7) / 8;
    if (!sink_.configure(req)) return false;
    fmt_ = sink_.activeFormat();
    // Bit-perfect requires the exact requested rate; ALSA's set_rate_near can
    // silently snap to the nearest hardware-supported rate otherwise (fine
    // for the Reference-EQ path, which reads the real rate back via
    // getConfiguredRate() regardless of whether it matched the request).
    if (strictBitperfect && fmt_.sampleRate != rate) return false;
    return true;
}

bool AlsaOutput::start() { return sink_.start(); }

int AlsaOutput::writeInt32(const int32_t* data, int numSamples) {
    int subslot = fmt_.subslotBytes;
    if (subslot <= 0) return -1;
    wireBuf_.resize((size_t)numSamples * subslot);
    uint8_t* out = wireBuf_.data();
    for (int i = 0; i < numSamples; ++i) {
        uint32_t v = (uint32_t)data[i];
        if (subslot == 4) {
            out[i * 4 + 0] = (uint8_t)(v);
            out[i * 4 + 1] = (uint8_t)(v >> 8);
            out[i * 4 + 2] = (uint8_t)(v >> 16);
            out[i * 4 + 3] = (uint8_t)(v >> 24);
        } else if (subslot == 3) {
            // S24_3LE: the top 24 bits of the left-justified 32-bit sample.
            out[i * 3 + 0] = (uint8_t)(v >> 8);
            out[i * 3 + 1] = (uint8_t)(v >> 16);
            out[i * 3 + 2] = (uint8_t)(v >> 24);
        } else {
            // S16_LE
            int16_t s16 = (int16_t)(v >> 16);
            out[i * 2 + 0] = (uint8_t)(s16);
            out[i * 2 + 1] = (uint8_t)(s16 >> 8);
        }
    }
    int wrote = sink_.write(out, numSamples * subslot);
    return wrote > 0 ? wrote / subslot : wrote;
}

int AlsaOutput::writeFloat32(const float* data, int numSamples) {
    std::vector<int32_t> tmp(numSamples);
    for (int i = 0; i < numSamples; ++i) {
        float f = std::clamp(data[i], -1.0f, 1.0f);
        tmp[i] = (int32_t)((double)f * 2147483647.0);
    }
    return writeInt32(tmp.data(), numSamples);
}

void AlsaOutput::stop()  { sink_.stop(); }
void AlsaOutput::close() { sink_.close(); }

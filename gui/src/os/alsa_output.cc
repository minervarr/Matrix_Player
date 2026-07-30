#include "alsa_output.hh"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <alsa/asoundlib.h>

// Mirrors AlsaSource::enumerateCaptureDevices' shape (audio_engine's own
// capture-side enumeration), but walks SND_PCM_STREAM_PLAYBACK instead —
// that method lives on the capture class, not something this adapter (in
// matrix_player's own tree, not the audio_engine submodule) can call for
// playback devices.
std::vector<AlsaDeviceInfo> AlsaOutput::enumerateDevices() {
    std::vector<AlsaDeviceInfo> out;

    int card = -1;
    while (snd_card_next(&card) >= 0 && card >= 0) {
        char ctlName[32];
        snprintf(ctlName, sizeof(ctlName), "hw:%d", card);

        snd_ctl_t* ctl = nullptr;
        if (snd_ctl_open(&ctl, ctlName, 0) < 0) continue;

        snd_ctl_card_info_t* cardInfo = nullptr;
        snd_ctl_card_info_alloca(&cardInfo);
        const char* cardName = "(unknown card)";
        if (snd_ctl_card_info(ctl, cardInfo) >= 0)
            cardName = snd_ctl_card_info_get_name(cardInfo);

        int device = -1;
        while (snd_ctl_pcm_next_device(ctl, &device) >= 0 && device >= 0) {
            snd_pcm_info_t* pcmInfo = nullptr;
            snd_pcm_info_alloca(&pcmInfo);
            snd_pcm_info_set_device(pcmInfo, (unsigned)device);
            snd_pcm_info_set_subdevice(pcmInfo, 0);
            snd_pcm_info_set_stream(pcmInfo, SND_PCM_STREAM_PLAYBACK);
            if (snd_ctl_pcm_info(ctl, pcmInfo) < 0) continue;   // not playback-capable

            AlsaDeviceInfo d;
            char id[32];
            snprintf(id, sizeof(id), "hw:%d,%d", card, device);
            d.deviceId = id;
            d.name = std::string(cardName) + " \xE2\x80\x94 " + snd_pcm_info_get_name(pcmInfo);
            out.push_back(std::move(d));
        }
        snd_ctl_close(ctl);
    }
    return out;
}

bool AlsaOutput::configure(int rate, int channels, int bitDepth, bool strictBitperfect) {
    if (!sink_.open(deviceId_)) return false;
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
    // Grow-only scratch: allocating a fresh vector per call put a malloc on the
    // decode thread, which is exactly where a stall turns into a dropout.
    if ((int)floatConvBuf_.size() < numSamples) floatConvBuf_.resize((size_t)numSamples);
    for (int i = 0; i < numSamples; ++i) {
        float f = std::clamp(data[i], -1.0f, 1.0f);
        floatConvBuf_[i] = (int32_t)((double)f * 2147483647.0);
    }
    return writeInt32(floatConvBuf_.data(), numSamples);
}

void AlsaOutput::flush() { sink_.flush(); }

bool AlsaOutput::hasFaulted() const { return sink_.hasFaulted(); }

int AlsaOutput::pendingPlaybackMs() const {
    if (fmt_.sampleRate <= 0) return 0;
    return (int)((int64_t)sink_.pendingFrames() * 1000 / fmt_.sampleRate);
}

// Bytes still queued in the device, in the caller's sample units. There is no
// application-side ring here, so this is the hardware's own backlog.
size_t AlsaOutput::ringAvailable() const {
    return (size_t)sink_.pendingFrames() * (size_t)fmt_.channels * sizeof(int32_t);
}

void AlsaOutput::stop()  { sink_.stop(); }
void AlsaOutput::close() { sink_.close(); }

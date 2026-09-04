#include "bt_output.hh"

#include <algorithm>
#include <cstdio>

std::vector<BtDeviceInfo> BtOutput::enumerateDevices() {
    std::vector<BtDeviceInfo> out;
    ae::bluez::Bus bus;
    if (!bus.open()) return out;

    for (const ae::bluez::SinkDevice& d : bus.sinks()) {
        BtDeviceInfo info;
        info.mac       = d.address;
        info.name      = d.name.empty() ? d.address : d.name;
        info.path      = d.path;
        info.connected = d.connected;
        // A transport under the device means some source endpoint already owns
        // it. Asked here so the panel can say "in use" on the row rather than
        // letting the listener pick it and meet the failure a second later.
        info.busy      = d.connected && bus.transportFor(d.path).exists;
        out.push_back(std::move(info));
    }
    return out;
}

bool BtOutput::ensureOpen() {
    if (opened_) return true;
    if (devicePath_.empty()) {
        lastError_ = "no Bluetooth device selected";
        return false;
    }

    ae::bluez::Bus bus;
    if (!bus.open()) {
        lastError_ = bus.lastError().empty() ? "BlueZ is not running" : bus.lastError();
        return false;
    }
    for (const ae::bluez::SinkDevice& d : bus.sinks()) {
        if (d.path != devicePath_) continue;
        deviceName_ = d.name.empty() ? d.address : d.name;
        mac_        = d.address;
        if (!sink_.open(d)) {
            lastError_ = sink_.lastError();
            return false;
        }
        opened_ = true;
        return true;
    }
    lastError_ = "that Bluetooth device is no longer paired";
    return false;
}

bool BtOutput::configure(int rate, int channels, int bitDepth, bool strictBitperfect) {
    lastError_.clear();
    (void)bitDepth;

    // Refused rather than silently downgraded. SBC is a lossy encode over a
    // radio link: there is no configuration of this backend that is
    // bit-perfect, and a player whose badge says EXACT over Bluetooth would be
    // lying about the one thing the badge exists to state.
    if (strictBitperfect) {
        lastError_ = "Bluetooth cannot be bit-perfect — SBC is a lossy encode. "
                     "Turn off bit-perfect playback, or use a USB DAC.";
        return false;
    }
    if (!ensureOpen()) return false;

    ae::AudioFormat req{};
    req.sampleRate   = rate;
    req.channels     = channels;
    req.bitDepth     = 16;
    req.subslotBytes = 2;
    if (!sink_.configure(req)) {
        lastError_ = sink_.lastError();
        if (lastError_.empty())
            lastError_ = deviceName_ + " refused " + std::to_string(rate) + " Hz";
        return false;
    }
    fmt_ = sink_.activeFormat();
    return true;
}

bool BtOutput::start() {
    if (!sink_.start()) {
        lastError_ = sink_.lastError();
        return false;
    }
    return true;
}

// Left-justified int32 down to the S16 the encoder takes. A plain shift, and
// the signal-chain readout reports it as a truncation for exactly that reason —
// the same treatment ALSA's S16_LE branch and AAudio's >> 16 already get. Not
// dithered here: adding noise on the way into a lossy encoder is spending bits
// the encoder is about to throw away.
int BtOutput::writeInt32(const int32_t* data, int numSamples) {
    if (numSamples <= 0) return 0;
    wireBuf_.resize((size_t)numSamples * 2);
    uint8_t* out = wireBuf_.data();
    for (int i = 0; i < numSamples; ++i) {
        const int16_t s16 = (int16_t)((uint32_t)data[i] >> 16);
        out[i * 2 + 0] = (uint8_t)(s16);
        out[i * 2 + 1] = (uint8_t)(s16 >> 8);
    }
    const int wrote = sink_.write(out, numSamples * 2);
    if (wrote < 0) {
        lastError_ = sink_.lastError();
        return wrote;
    }
    return wrote / 2;
}

int BtOutput::writeFloat32(const float* data, int numSamples) {
    if ((int)floatConvBuf_.size() < numSamples) floatConvBuf_.resize((size_t)numSamples);
    for (int i = 0; i < numSamples; ++i) {
        const float f = std::clamp(data[i], -1.0f, 1.0f);
        floatConvBuf_[i] = (int32_t)((double)f * 2147483647.0);
    }
    return writeInt32(floatConvBuf_.data(), numSamples);
}

std::string BtOutput::wireFormat() const {
    if (!sink_.streaming() && fmt_.sampleRate <= 0) return {};
    const ae::a2dp::SbcCaps& c = sink_.configuration();
    std::string s = "SBC, bitpool " + std::to_string((int)c.maxBitpool);
    if (c.channelMode == ae::a2dp::kChanJointStereo) s += ", joint stereo";
    else if (c.channelMode == ae::a2dp::kChanStereo) s += ", stereo";
    else if (c.channelMode == ae::a2dp::kChanMono)   s += ", mono";
    return s;
}

// Asked of BlueZ rather than of the sink, because the panel wants this BEFORE
// anything is opened — pickOutputRate() runs while the listener is still
// choosing a device. An empty answer means "unknown", which is what the base
// class returned before this override existed, so the caller's fallback is
// unchanged for a device BlueZ cannot describe.
std::vector<int> BtOutput::probeRates(int channels) const {
    (void)channels;
    if (sink_.streaming()) return sink_.supportedRates();

    ae::bluez::Bus bus;
    if (!bus.open() || devicePath_.empty()) return {};
    for (const ae::bluez::SinkDevice& d : bus.sinks()) {
        if (d.path != devicePath_ || !d.hasSbc) continue;
        std::vector<int> out;
        if (d.sbcCaps.freq & ae::a2dp::kFreq16000) out.push_back(16000);
        if (d.sbcCaps.freq & ae::a2dp::kFreq32000) out.push_back(32000);
        if (d.sbcCaps.freq & ae::a2dp::kFreq44100) out.push_back(44100);
        if (d.sbcCaps.freq & ae::a2dp::kFreq48000) out.push_back(48000);
        return out;
    }
    return {};
}

void BtOutput::flush() { sink_.flush(); }
void BtOutput::stop()  { sink_.stop(); }

void BtOutput::close() {
    sink_.close();
    opened_ = false;
    fmt_ = ae::AudioFormat{};
}

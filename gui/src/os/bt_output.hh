#pragma once
// Bluetooth A2DP output — a third Linux backend beside ALSA and JACK, over
// audio_engine's BluetoothSink (backends/bluetooth/bluetooth_sink.h).
//
// Built only when audio_engine found sd-bus and libsbc (MATRIX_HAVE_BLUETOOTH,
// gated in gui/CMakeLists.txt exactly as ALSA and JACK are), so the Audio
// Settings panel only ever offers a backend this build actually has.
//
// --- what this backend is honest about ---------------------------------------
//
// It is the ONE output in this player that cannot be bit-perfect, and it says
// so in three places rather than one: strictBitperfect is refused outright,
// getConfiguredBits() answers 16 because SBC's input is S16, and wireFormat()
// names the codec and the bitpool. That is what makes the signal-chain readout
// say "SBC, bitpool 53" instead of drawing a chain that stops at the socket and
// implies nothing happens after it. The same rule AAudioOutput follows for
// being always-16-bit, applied to a link that is also lossy.
//
// What it buys in return is the thing this project keeps buying: no sound
// server between the samples and the device. PipeWire would have resampled,
// mixed and attenuated before its own encoder ever saw them.
#include <string>
#include <vector>

#include "audio_output.h"
#include "backends/bluetooth/bluetooth_sink.h"
#include "backends/bluetooth/bluez_a2dp.h"

struct BtDeviceInfo {
    std::string mac;      // XX:XX:XX:XX:XX:XX — the same key bt_codec produces
    std::string name;     // the Alias, which is what a listener recognises
    std::string path;     // BlueZ's object path; the id this backend opens by
    bool connected = false;
    bool busy = false;    // something else already holds its transport
};

class BtOutput : public AudioOutput {
public:
    explicit BtOutput(std::string devicePath = {}) : devicePath_(std::move(devicePath)) {}

    // Every A2DP sink BlueZ knows about, with the two facts the panel needs to
    // draw a row that can be acted on: is it connected, and is it already
    // somebody else's.
    static std::vector<BtDeviceInfo> enumerateDevices();

    bool configure(int rate, int channels, int bitDepth, bool strictBitperfect = false) override;
    bool start() override;
    int  writeFloat32(const float* data, int numSamples) override;
    int  writeInt32(const int32_t* data, int numSamples) override;
    void flush() override;
    void stop() override;
    void close() override;

    int  getConfiguredRate()     const override { return fmt_.sampleRate; }
    int  getConfiguredChannels() const override { return fmt_.channels; }
    int  getConfiguredBits()     const override { return fmt_.bitDepth; }
    std::string wireFormat() const override;
    std::string deviceName() const override { return deviceName_.empty() ? devicePath_ : deviceName_; }
    std::vector<int> probeRates(int channels) const override;
    int  pendingPlaybackMs() const override { return sink_.pendingPlaybackMs(); }
    std::string lastError() const override { return lastError_; }

    // The MAC, so getActiveDeviceKey() can build a2dp:<MAC> and an AutoEQ
    // profile follows the HEADPHONES rather than this machine — the same key
    // the Android side produces for the same pair.
    const std::string& mac() const { return mac_; }

private:
    bool ensureOpen();

    std::string devicePath_;
    std::string deviceName_;
    std::string mac_;
    std::string lastError_;

    ae::BluetoothSink sink_;
    ae::AudioFormat   fmt_{};
    bool opened_ = false;

    // Reused so the decode thread never allocates mid-track, the same reason
    // AlsaOutput keeps its own two scratch buffers.
    std::vector<uint8_t> wireBuf_;
    std::vector<int32_t> floatConvBuf_;
};

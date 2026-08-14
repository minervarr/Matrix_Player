#pragma once
#include "usb_audio.h"
#include <vector>
#include <string>
#include <chrono>
#include <thread>
#include <cstdio>

class AudioOutput {
public:
    virtual ~AudioOutput() = default;
    virtual bool configure(int rate, int channels, int bitDepth, bool strictBitperfect = false) = 0;
    virtual bool start() = 0;
    virtual int  writeFloat32(const float* data, int numSamples) = 0;
    virtual int  writeFloat32Blocking(const float* data, int numSamples, int timeoutMs = 500) {
        return writeFloat32(data, numSamples);
    }
    // Bit-perfect int32 path (left-justified full-range samples). Default routes
    // through float for outputs without a native integer sink (e.g. WASAPI); the
    // USB adapter overrides it to hand bytes straight to the DAC, losslessly.
    virtual int  writeInt32(const int32_t* data, int numSamples) {
        // Fallback: scale full-range int32 to [-1,1] float. Not bit-perfect, but
        // only used by outputs that didn't override (WASAPI), where the float
        // mixer already precludes bit-exactness.
        std::vector<float> tmp(numSamples);
        for (int i = 0; i < numSamples; ++i)
            tmp[i] = (float)((double)data[i] / 2147483648.0);
        return writeFloat32(tmp.data(), numSamples);
    }
    virtual int  writeInt32Blocking(const int32_t* data, int numSamples, int timeoutMs = 500) {
        return writeInt32(data, numSamples);
    }
    virtual void flush() {}
    virtual void stop() = 0;
    virtual void close() = 0;
    virtual int  getConfiguredRate()     const = 0;
    virtual int  getConfiguredChannels() const = 0;
    // The rest of what was NEGOTIATED, for the signal-chain readout.
    //
    // Zero and empty mean "this backend does not know", and the readout OMITS
    // the row rather than filling it in. That is the whole point of the
    // neutral default: a chain that claims a wire format it never checked is
    // worse than one that admits the gap, and this app's badge exists
    // precisely to not overclaim. Backends that do know (ALSA and AAudio both
    // hold a whole ae::AudioFormat already) override them.
    virtual int  getConfiguredBits() const { return 0; }
    virtual std::string wireFormat()  const { return {}; }
    // The device actually in use, in words a listener would recognise. Not the
    // key getActiveDeviceKey() builds, which is for the EQ assignment table.
    virtual std::string deviceName()  const { return {}; }
    virtual size_t ringAvailable() const { return 0; }
    // Milliseconds of audio accepted by this output but not yet rendered by the
    // device (ring buffer + any in-flight/device queue). Lets the caller report
    // true playback position and drain the buffered tail before stop()/reconfig
    // at track boundaries. Outputs with negligible buffering may return 0.
    virtual int  pendingPlaybackMs() const { return 0; }
    virtual bool waitForData(int minSamples, int timeoutMs) { (void)minSamples; (void)timeoutMs; return true; }
    virtual int  getPreBufferSamples() const { return 4096; }
    // Returns exclusive-mode sample rates the device accepts. Empty = unknown (USB uses descriptor).
    virtual std::vector<int> probeRates(int /*channels*/) const { return {}; }
    // True once the backend has caught an unrecoverable device/driver error
    // (e.g. a crash inside a Windows audio DLL) and torn down its own stream.
    // The caller should stop playback rather than keep feeding a dead output.
    virtual bool hasFaulted() const { return false; }
    // Why the last configure()/start() failed, in the DRIVER's own words, or
    // empty when the backend has nothing more specific than "it failed".
    //
    // Exists because "Audio output failed to configure. Check Audio Settings."
    // is not a diagnosis: when PipeWire is holding the card, ALSA already knows
    // the answer is "Device or resource busy" and it was being thrown away one
    // frame below the call. The caller puts this on screen (audioNotice_).
    virtual std::string lastError() const { return {}; }
};

namespace detail {
inline int64_t monotonicMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace detail

// Thin adapter so UsbAudioDriver satisfies AudioOutput.
// close() is a no-op — the driver's lifetime is owned by PlayerWindow.
class UsbAudioOutput : public AudioOutput {
public:
    explicit UsbAudioOutput(UsbAudioDriver& d) : d_(d) {}
    bool configure(int r, int ch, int bd, bool strictBitperfect = false) override { return d_.configure(r, ch, bd); }
    // Note: timer resolution (timeBeginPeriod(1) on Windows) is raised once at
    // app startup in os/windows_host.cc, not per-start, so the pre-buffer wait
    // loop in PlayerWindow::onPlay runs at 1 ms grain instead of ~15 ms.
    bool start()  override { return d_.start(); }
    int  writeFloat32(const float* p, int n) override { return d_.writeFloat32(p, n); }
    int  writeInt32(const int32_t* p, int n) override { return d_.writeInt32(p, n); }
    void flush()  override { d_.flush(); }
    void stop()   override { d_.stop(); }
    void close()  override {}
    int  getConfiguredRate()     const override { return d_.getConfiguredRate(); }
    int  getConfiguredChannels() const override { return d_.getConfiguredChannels(); }
    // The driver has known all of this since parseDescriptors(); it simply had
    // no way out through this interface. The wire format is expressed as the
    // significant bits inside the SUBSLOT, because that pair is the whole
    // story on a UAC endpoint (24 significant bits in a 4-byte slot is a
    // different thing from 32, and the difference is audible to nobody but
    // matters to anyone reading this page).
    int  getConfiguredBits() const override { return d_.getConfiguredBitDepth(); }
    std::string wireFormat() const override {
        const int b = d_.getConfiguredBitDepth(), sub = d_.getConfiguredSubslotSize();
        if (b <= 0 || sub <= 0) return {};
        return std::to_string(b) + "-bit in " + std::to_string(sub) + "-byte slot";
    }
    std::string deviceName() const override { return d_.getDeviceInfo(); }
    size_t ringAvailable() const override { return d_.ringAvailable(); }
    int  pendingPlaybackMs() const override { return d_.getPendingPlaybackMs(); }

    int writeFloat32Blocking(const float* p, int n, int timeoutMs = 500) override {
        int total = 0;
        int64_t t0 = detail::monotonicMs();
        int spins = 0;
        while (total < n) {
            int written = d_.writeFloat32(p + total, n - total);
            if (written > 0) {
                total += written;
                spins = 0;
                continue;
            }
            if ((int)(detail::monotonicMs() - t0) >= timeoutMs) {
                static int64_t lastLog = 0;
                int64_t nowMs = detail::monotonicMs();
                if ((nowMs - lastLog) >= 1000) {
                    printf("[USB][WARN] writeFloat32Blocking timeout: wanted=%d got=%d elapsed=%ums ring=%zu\n",
                           n, total, (unsigned)(nowMs - t0), d_.ringAvailable());
                    fflush(stdout);
                    lastLog = nowMs;
                }
                break;
            }
            if (spins < 4)
                std::this_thread::yield();
            else
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            ++spins;
        }
        return total;
    }

    int writeInt32Blocking(const int32_t* p, int n, int timeoutMs = 500) override {
        int total = 0;
        int64_t t0 = detail::monotonicMs();
        int spins = 0;
        while (total < n) {
            int written = d_.writeInt32(p + total, n - total);
            if (written > 0) {
                total += written;
                spins = 0;
                continue;
            }
            if ((int)(detail::monotonicMs() - t0) >= timeoutMs) {
                static int64_t lastLog = 0;
                int64_t nowMs = detail::monotonicMs();
                if ((nowMs - lastLog) >= 1000) {
                    printf("[USB][WARN] writeInt32Blocking timeout: wanted=%d got=%d elapsed=%ums ring=%zu\n",
                           n, total, (unsigned)(nowMs - t0), d_.ringAvailable());
                    fflush(stdout);
                    lastLog = nowMs;
                }
                break;
            }
            if (spins < 4)
                std::this_thread::yield();
            else
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            ++spins;
        }
        return total;
    }

    bool waitForData(int minSamples, int timeoutMs) override {
        int minBytes = minSamples * d_.getConfiguredSubslotSize();
        int64_t t0 = detail::monotonicMs();
        while ((int)d_.ringAvailable() < minBytes) {
            if ((int)(detail::monotonicMs() - t0) >= timeoutMs) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return true;
    }

private:
    UsbAudioDriver& d_;
};

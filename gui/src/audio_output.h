#pragma once
#include "usb_audio.h"
#include <vector>
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

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
};

// Thin adapter so UsbAudioDriver satisfies AudioOutput.
// close() is a no-op — the driver's lifetime is owned by PlayerWindow.
class UsbAudioOutput : public AudioOutput {
public:
    explicit UsbAudioOutput(UsbAudioDriver& d) : d_(d) {}
    bool configure(int r, int ch, int bd, bool strictBitperfect = false) override { return d_.configure(r, ch, bd); }
    // Note: timer resolution (timeBeginPeriod(1)) is raised once at app
    // startup in main.cpp, not per-start, so the pre-buffer wait loop in
    // PlayerWindow::onPlay runs at 1 ms grain instead of ~15 ms.
    bool start()  override { return d_.start(); }
    int  writeFloat32(const float* p, int n) override { return d_.writeFloat32(p, n); }
    int  writeInt32(const int32_t* p, int n) override { return d_.writeInt32(p, n); }
    void flush()  override { d_.flush(); }
    void stop()   override { d_.stop(); }
    void close()  override {}
    int  getConfiguredRate()     const override { return d_.getConfiguredRate(); }
    int  getConfiguredChannels() const override { return d_.getConfiguredChannels(); }
    size_t ringAvailable() const override { return d_.ringAvailable(); }
    int  pendingPlaybackMs() const override { return d_.getPendingPlaybackMs(); }

    int writeFloat32Blocking(const float* p, int n, int timeoutMs = 500) override {
        int total = 0;
        DWORD t0 = GetTickCount();
        int spins = 0;
        while (total < n) {
            int written = d_.writeFloat32(p + total, n - total);
            if (written > 0) {
                total += written;
                spins = 0;
                continue;
            }
            if ((int)(GetTickCount() - t0) >= timeoutMs) {
                static DWORD lastLog = 0;
                DWORD nowMs = GetTickCount();
                if ((nowMs - lastLog) >= 1000) {
                    printf("[USB][WARN] writeFloat32Blocking timeout: wanted=%d got=%d elapsed=%ums ring=%zu\n",
                           n, total, (unsigned)(nowMs - t0), d_.ringAvailable());
                    fflush(stdout);
                    lastLog = nowMs;
                }
                break;
            }
            if (spins < 4)
                SwitchToThread();
            else
                Sleep(1);
            ++spins;
        }
        return total;
    }

    int writeInt32Blocking(const int32_t* p, int n, int timeoutMs = 500) override {
        int total = 0;
        DWORD t0 = GetTickCount();
        int spins = 0;
        while (total < n) {
            int written = d_.writeInt32(p + total, n - total);
            if (written > 0) {
                total += written;
                spins = 0;
                continue;
            }
            if ((int)(GetTickCount() - t0) >= timeoutMs) {
                static DWORD lastLog = 0;
                DWORD nowMs = GetTickCount();
                if ((nowMs - lastLog) >= 1000) {
                    printf("[USB][WARN] writeInt32Blocking timeout: wanted=%d got=%d elapsed=%ums ring=%zu\n",
                           n, total, (unsigned)(nowMs - t0), d_.ringAvailable());
                    fflush(stdout);
                    lastLog = nowMs;
                }
                break;
            }
            if (spins < 4)
                SwitchToThread();
            else
                Sleep(1);
            ++spins;
        }
        return total;
    }

    bool waitForData(int minSamples, int timeoutMs) override {
        int minBytes = minSamples * d_.getConfiguredSubslotSize();
        DWORD t0 = GetTickCount();
        while ((int)d_.ringAvailable() < minBytes) {
            if ((int)(GetTickCount() - t0) >= timeoutMs) return false;
            Sleep(2);
        }
        return true;
    }

private:
    UsbAudioDriver& d_;
};

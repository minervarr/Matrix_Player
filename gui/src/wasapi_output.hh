#pragma once
#include "audio_output.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>

enum class WasapiMode   { Shared, Exclusive };
enum class WireFormat   { Float32, Int32, Int24In32, Int16 };

struct WasapiDeviceInfo {
    std::wstring id;
    std::wstring name;
};

class WasapiOutput : public AudioOutput {
public:
    static std::vector<WasapiDeviceInfo> enumerateDevices();

    explicit WasapiOutput(std::wstring deviceId = L"",
                          WasapiMode   mode     = WasapiMode::Shared);
    ~WasapiOutput() override;

    bool configure(int rate, int channels, int bitDepth, bool strictBitperfect = false) override;
    // Returns all exclusive-mode sample rates the device accepts (uses IsFormatSupported only).
    std::vector<int> probeRates(int channels) const override;
    // Returns the highest bit depth the device supports at (rate, channels) in exclusive mode.
    int getMaxBitDepth(int rate, int channels) const;
    bool start()  override;
    int  writeFloat32(const float* data, int numSamples) override;
    int  writeFloat32Blocking(const float* data, int numSamples, int timeoutMs = 30000) override;
    int  writeInt32Blocking(const int32_t* data, int numSamples, int timeoutMs = 30000) override;
    void flush()  override;
    void stop()   override;
    void close()  override;
    int  getConfiguredRate()     const override { return rate_; }
    int  getConfiguredChannels() const override { return channels_; }
    size_t ringAvailable() const override;
    int  pendingPlaybackMs() const override;
    bool waitForData(int minSamples, int timeoutMs) override;
    int  getPreBufferSamples() const override;
    bool hasFaulted() const override { return deviceFaulted_.load(std::memory_order_acquire); }

private:
    bool configureShared(int srcChannels);
    bool configureExclusive(int rate, int channels, bool strictBitperfect);
    void allocRing();
    int  writeRing(const float* src, int n);
    int  readRing(float* dst, int n);
    void convertToWire(const float* src, BYTE* dst, int n);
    void renderLoop();

    std::wstring deviceId_;
    WasapiMode   mode_;
    WireFormat   wireFormat_ = WireFormat::Float32;
    int rate_ = 0, channels_ = 0;
    bool useEvent_ = true;
    int  periodMs_ = 10;

    IMMDevice*          pDevice_       = nullptr;
    IAudioClient*       pAudioClient_  = nullptr;
    IAudioRenderClient* pRenderClient_ = nullptr;
    HANDLE              hEvent_        = nullptr;
    HANDLE              hDrainEvent_   = nullptr;
    UINT32              bufferFrames_  = 0;

    // Lock-free power-of-2 float ring buffer
    std::vector<float>  ring_;
    size_t              ringMask_ = 0;
    std::atomic<size_t> readPos_{0};
    std::atomic<size_t> writePos_{0};

    std::thread       renderThread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> started_{false};
    std::atomic<int>  underrunCount_{0};
    // Set when a WASAPI call crashes inside the OS audio stack (see sehGetBuffer
    // et al. in wasapi_output.cpp) — the render loop has already torn itself down.
    // mutable: pendingPlaybackMs() is const but polls GetCurrentPadding and can
    // observe a crash there too.
    mutable std::atomic<bool> deviceFaulted_{false};
};

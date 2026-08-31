#pragma once
// Android's shared-USB output backend: Matrix Player as a CLIENT of AOAS (the
// Android One Audio Server). AOAS owns the USB permission and the live
// isochronous stream to the DAC and never closes it; this adapter acquires
// ownership, maps the shared ring AOAS creates, and writes wire-format PCM
// into it. Zero DSP, zero resampling — the ring copy is a memcpy and the
// packing is the same code UsbAudioDriver feeds its own ring with.
//
// Compiled ONLY by android/CMakeLists.txt (next to aaudio_output.cc). Desktop
// builds never see this file.
//
// The two server rules that shape everything here are stated in
// docs/superpowers/specs/2026-08-28-aoas-client-backend.md; the short form:
// the server refuses a second acquire() outright, so every format change and
// every flush() goes release -> acquire; and the producer may not move
// readPos, so discarding buffered audio is only possible through the server.
#include <android_native_app_glue.h>

// Called once from android/src/main.cc, before PlayerWindow::create(). The
// JNI upcalls need the activity's VM, and its classloader — app classes are
// invisible to FindClass() from a native thread, whose default loader is the
// system one.
void matrixAoasSetApp(android_app* app);

#include "audio_output.h"

#include "aoas/shm_ring.hh"     // third_party/aoas — AOAS's ring contract, verbatim
#include "core/dsp/audio_convert.h"  // ae::DitherLCG — the float path's 16-bit dither

#include <atomic>
#include <string>
#include <vector>

class AoasOutput : public AudioOutput {
public:
    AoasOutput() = default;
    ~AoasOutput() override;

    bool configure(int rate, int channels, int bitDepth,
                   bool strictBitperfect = false) override;
    bool start() override;
    void stop() override;
    void close() override;

    int  writeFloat32(const float* data, int numSamples) override;
    int  writeInt32(const int32_t* data, int numSamples) override;
    int  writeFloat32Blocking(const float* data, int numSamples,
                              int timeoutMs = 500) override;
    int  writeInt32Blocking(const int32_t* data, int numSamples,
                            int timeoutMs = 500) override;
    // Buffered audio cannot be discarded from the producer side (readPos is
    // AOAS's) — flush goes through the server as a silent self-handover.
    void flush() override;

    int  getConfiguredRate()     const override { return cfgRate_; }
    int  getConfiguredChannels() const override { return cfgCh_; }
    int  getConfiguredBits()     const override { return cfgBits_; }
    std::string wireFormat()  const override;
    std::string deviceName()  const override { return deviceInfo_; }
    size_t ringAvailable()    const override;
    int  pendingPlaybackMs()  const override;
    bool waitForData(int minSamples, int timeoutMs) override;
    bool hasFaulted()         const override;
    std::string lastError()   const override { return currentError(); }
    // IAoas exposes no rate list; the server's ERR_FORMAT_UNSUPPORTED is the
    // answer, and pickOutputRate's 48 kHz fallback is a guess that the second
    // configure() then either confirms or reports honestly.
    std::vector<int> probeRates(int /*channels*/) const override { return {}; }

private:
    bool acquireOwned(int rate, int channels, int bits);
    void releaseOwned();
    void releaseMapping();
    bool ringUsable() const;
    std::string currentError() const;

    aoas::ShmRing ring_;
    void*  base_        = nullptr;
    size_t regionBytes_ = 0;

    // One-generation guard for the mapping: releaseOwned() must not munmap
    // while a write/read on the decode thread is inside the ring — munmap
    // under a concurrent memcpy is a SIGSEGV. Every entry that touches the
    // ring takes a RingTouch; release waits for the count to drain before
    // unmapping (bounded, and it leaks one mapping rather than faulting).
    mutable std::atomic<int> ringTouches_{0};

    // What configure() asked for, and what the DAC is ACTUALLY running (the
    // server never converts; only the subslot may come back wider).
    int  reqRate_ = 0, reqCh_ = 0, reqBits_ = 0;
    int  cfgRate_ = 0, cfgCh_ = 0, cfgBits_ = 0;
    int  subslot_ = 0;
    bool owned_   = false;

    std::string deviceInfo_;
    std::string lastError_;
    DitherLCG dither_;   // same 16-bit TPDF the USB driver's float path uses
                         // (audio_convert.h declares it at global scope, not ae::)
};

#include "jack_output.hh"
#include <chrono>
#include <thread>

bool JackOutput::ensureClientOpen() {
    if (!clientOpen_) clientOpen_ = sink_.open("matrix_player");
    return clientOpen_;
}

std::vector<JackPlaybackPortInfo> JackOutput::enumeratePorts() {
    if (!ensureClientOpen()) return {};
    return sink_.enumeratePlaybackPorts();
}

bool JackOutput::configure(int rate, int channels, int bitDepth, bool strictBitperfect) {
    (void)bitDepth;
    if (!ensureClientOpen()) return false;
    channels_ = channels;
    ae::AudioFormat fmt{};
    fmt.sampleRate   = rate;
    fmt.channels     = channels;
    fmt.bitDepth     = 32;
    fmt.subslotBytes = 4;
    fmt.isFloat      = true;
    if (!sink_.configure(fmt)) return false;

    // The server dictates the real rate (JackSink is float32 at whatever
    // rate jackd is running); getConfiguredRate() reports it back to the
    // caller regardless of what was requested here.
    int serverRate = sink_.activeFormat().sampleRate;
    if (strictBitperfect && serverRate != rate) return false;
    return true;
}

bool JackOutput::start() {
    if (startPort_.empty()) return sink_.start();

    auto ports = sink_.enumeratePlaybackPorts();
    int startIdx = -1;
    for (int i = 0; i < (int)ports.size(); i++)
        if (ports[i].portName == startPort_) { startIdx = i; break; }
    if (startIdx < 0) return sink_.start();   // saved port no longer exists -> fall back to auto

    std::vector<std::string> dest;
    for (int i = startIdx; i < (int)ports.size() && (int)dest.size() < channels_; i++)
        dest.push_back(ports[i].portName);
    return sink_.start(dest);
}

int JackOutput::writeFloat32(const float* data, int numSamples) {
    int wrote = sink_.write((const uint8_t*)data, numSamples * (int)sizeof(float));
    return wrote > 0 ? wrote / (int)sizeof(float) : wrote;
}

int JackOutput::writeFloat32Blocking(const float* data, int numSamples, int timeoutMs) {
    // JackSink::write() is non-blocking (RT-safe lock-free ring) and returns
    // less than requested when the ring is full instead of waiting for the
    // process callback to drain it — without this retry loop, a caller that
    // treats the return value as "all consumed" silently drops samples under
    // backpressure. Mirrors UsbAudioOutput::writeFloat32Blocking's shape.
    int total = 0;
    int64_t t0 = detail::monotonicMs();
    int spins = 0;
    while (total < numSamples) {
        int wrote = writeFloat32(data + total, numSamples - total);
        if (wrote > 0) {
            total += wrote;
            spins = 0;
            continue;
        }
        // Negative means the sink is not streaming — stopped, or not started
        // yet. The ring will never drain, so waiting out the timeout would only
        // stall the decode thread before dropping the same samples anyway.
        if (wrote < 0) break;
        if ((int)(detail::monotonicMs() - t0) >= timeoutMs) break;
        if (spins < 4)
            std::this_thread::yield();
        else
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ++spins;
    }
    return total;
}

// The decoder produces left-justified int32; JACK is float32 end to end, so
// this is the conversion every int32 write funnels through. Scaling by a power
// of two is exact, so the only precision lost is int32 -> float32's 24-bit
// mantissa, which JACK imposes regardless.
static void toFloat(const int32_t* src, float* dst, int n) {
    constexpr double kScale = 1.0 / 2147483648.0;   // 1 / 2^31
    for (int i = 0; i < n; ++i) dst[i] = (float)(src[i] * kScale);
}

int JackOutput::writeInt32(const int32_t* data, int numSamples) {
    if (numSamples <= 0) return 0;
    if ((int)convBuf_.size() < numSamples) convBuf_.resize((size_t)numSamples);
    toFloat(data, convBuf_.data(), numSamples);
    return writeFloat32(convBuf_.data(), numSamples);
}

int JackOutput::writeInt32Blocking(const int32_t* data, int numSamples, int timeoutMs) {
    if (numSamples <= 0) return 0;
    if ((int)convBuf_.size() < numSamples) convBuf_.resize((size_t)numSamples);
    // Convert ONCE, then retry on the float buffer — re-converting on every
    // spin of the backpressure loop would burn the decode thread for nothing.
    toFloat(data, convBuf_.data(), numSamples);
    return writeFloat32Blocking(convBuf_.data(), numSamples, timeoutMs);
}

void JackOutput::flush() { sink_.flush(); }

size_t JackOutput::ringAvailable() const { return sink_.ringAvailable(); }

bool JackOutput::hasFaulted() const { return sink_.hasFaulted(); }

std::string JackOutput::lastError() const {
    if (sink_.serverIsGone())
        return "the JACK server shut down";
    if (!clientOpen_)
        return "no running JACK server (jackd is not started)";
    return {};
}

int JackOutput::pendingPlaybackMs() const {
    const int rate = sink_.activeFormat().sampleRate;
    if (rate <= 0) return 0;
    return (int)((int64_t)sink_.pendingFrames() * 1000 / rate);
}

// Half a second, not AudioOutput's 4096-sample default (~43 ms at 48 kHz).
// That default is right for USB, whose real cushion is the pre-filled
// isochronous queue; JACK's only cushion is this ring, so it has to be asked
// for explicitly or playback starts nearly empty and underruns immediately.
int JackOutput::getPreBufferSamples() const {
    const int rate = sink_.activeFormat().sampleRate;
    if (rate <= 0 || channels_ <= 0) return 4096;
    return rate * channels_ / 2;
}

bool JackOutput::waitForData(int minSamples, int timeoutMs) {
    // Same shape as UsbAudioOutput::waitForData (audio_output.h:142-150); JACK
    // is float32, so a sample is 4 bytes.
    const size_t minBytes = (size_t)minSamples * sizeof(float);
    const int64_t t0 = detail::monotonicMs();
    while (sink_.ringAvailable() < minBytes) {
        if (sink_.hasFaulted()) return false;
        if ((int)(detail::monotonicMs() - t0) >= timeoutMs) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

void JackOutput::stop() { sink_.stop(); }

void JackOutput::close() {
    sink_.close();
    clientOpen_ = false;
}

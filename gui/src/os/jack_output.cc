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
        if ((int)(detail::monotonicMs() - t0) >= timeoutMs) break;
        if (spins < 4)
            std::this_thread::yield();
        else
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ++spins;
    }
    return total;
}

void JackOutput::stop() { sink_.stop(); }

void JackOutput::close() {
    sink_.close();
    clientOpen_ = false;
}

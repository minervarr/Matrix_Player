#include "jack_output.hh"

bool JackOutput::configure(int rate, int channels, int bitDepth, bool strictBitperfect) {
    (void)bitDepth;
    if (!clientOpen_) {
        clientOpen_ = sink_.open("matrix_player");
        if (!clientOpen_) return false;
    }
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

bool JackOutput::start() { return sink_.start(); }

int JackOutput::writeFloat32(const float* data, int numSamples) {
    int wrote = sink_.write((const uint8_t*)data, numSamples * (int)sizeof(float));
    return wrote > 0 ? wrote / (int)sizeof(float) : wrote;
}

void JackOutput::stop() { sink_.stop(); }

void JackOutput::close() {
    sink_.close();
    clientOpen_ = false;
}

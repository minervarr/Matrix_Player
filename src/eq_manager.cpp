#include "eq_manager.h"
#include <cstdio>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void EqManager::applyProfile(const EqProfile* profile, int sampleRate, int channels) {
    if (!profile || profile->filters.empty()) {
        active_.store(false, std::memory_order_release);
        processors_[0].setEnabled(false);
        processors_[1].setEnabled(false);
        return;
    }

    int numFilters = std::min((int)profile->filters.size(), EqProcessor::MAX_FILTERS);
    double coeffs[EqProcessor::MAX_FILTERS * 5];

    for (int i = 0; i < numFilters; i++) {
        auto& f = profile->filters[i];
        computeBiquad(f.type, f.fc, f.gain, f.q, sampleRate, &coeffs[i * 5]);
    }

    double preampLinear = pow(10.0, profile->preamp / 20.0);

    int backIdx = 1 - activeIdx_.load(std::memory_order_relaxed);
    // encoding 22 = PCM_32BIT: Reference EQ runs EqProcessor::process32 (64-bit
    // double biquads, single rounded quantization) directly on the lossless int32 stream.
    processors_[backIdx].configure(numFilters, coeffs, preampLinear, channels, 22);
    processors_[backIdx].setEnabled(true);
    active_.store(true, std::memory_order_relaxed);
    activeIdx_.store(backIdx, std::memory_order_release);

    printf("[EQ] Applied \"%s\" (%d filters, preamp %.1f dB) at %d Hz, %d ch\n",
           profile->name.c_str(), numFilters, profile->preamp, sampleRate, channels);
}

void EqManager::processInPlaceInt32(int32_t* data, int numSamples) {
    if (!active_.load(std::memory_order_relaxed)) return;
    int idx = activeIdx_.load(std::memory_order_acquire);
    processors_[idx].process(reinterpret_cast<uint8_t*>(data), numSamples * sizeof(int32_t));
}

void EqManager::processToDouble(const int32_t* in, double* out, int numSamples) {
    if (!active_.load(std::memory_order_relaxed)) {
        EqProcessor::scaleToDouble(in, out, numSamples);
        return;
    }
    int idx = activeIdx_.load(std::memory_order_acquire);
    processors_[idx].processToDouble(in, out, numSamples);
}

void EqManager::clear() {
    active_.store(false, std::memory_order_release);
    processors_[0].setEnabled(false);
    processors_[1].setEnabled(false);
}

// Robert Bristow-Johnson Audio EQ Cookbook
void EqManager::computeBiquad(const std::string& type, double fc, double gain,
                              double q, int sampleRate, double out[5]) {
    double w0 = 2.0 * M_PI * fc / sampleRate;
    double cosW0 = cos(w0);
    double sinW0 = sin(w0);
    double A = pow(10.0, gain / 40.0);
    double alpha = sinW0 / (2.0 * q);

    double b0, b1, b2, a0, a1, a2;

    if (type == "LSC") {
        double beta = 2.0 * sqrt(A) * alpha;
        b0 =     A * ((A + 1) - (A - 1) * cosW0 + beta);
        b1 = 2 * A * ((A - 1) - (A + 1) * cosW0);
        b2 =     A * ((A + 1) - (A - 1) * cosW0 - beta);
        a0 =          (A + 1) + (A - 1) * cosW0 + beta;
        a1 =    -2 * ((A - 1) + (A + 1) * cosW0);
        a2 =          (A + 1) + (A - 1) * cosW0 - beta;
    } else if (type == "HSC") {
        double beta = 2.0 * sqrt(A) * alpha;
        b0 =      A * ((A + 1) + (A - 1) * cosW0 + beta);
        b1 = -2 * A * ((A - 1) + (A + 1) * cosW0);
        b2 =      A * ((A + 1) + (A - 1) * cosW0 - beta);
        a0 =           (A + 1) - (A - 1) * cosW0 + beta;
        a1 =      2 * ((A - 1) - (A + 1) * cosW0);
        a2 =           (A + 1) - (A - 1) * cosW0 - beta;
    } else {
        b0 = 1 + alpha * A;
        b1 = -2 * cosW0;
        b2 = 1 - alpha * A;
        a0 = 1 + alpha / A;
        a1 = -2 * cosW0;
        a2 = 1 - alpha / A;
    }

    out[0] = b0 / a0;
    out[1] = b1 / a0;
    out[2] = b2 / a0;
    out[3] = a1 / a0;
    out[4] = a2 / a0;
}

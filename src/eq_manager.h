#pragma once
#include "eq_profiles.h"
#include "eq_processor.h"
#include <atomic>

class EqManager {
public:
    void applyProfile(const EqProfile* profile, int sampleRate, int channels);
    // Reference EQ path: EQ a left-justified int32 buffer in place (encoding 22 →
    // EqProcessor::process32, double math + single rounded snap to the 32-bit grid).
    void processInPlaceInt32(int32_t* data, int numSamples);
    // EQ int32 input to double[] without quantizing — used before resampling.
    // If EQ is inactive, just scales int32 to double.
    void processToDouble(const int32_t* in, double* out, int numSamples);
    bool isActive() const { return active_.load(std::memory_order_relaxed); }
    void clear();

private:
    static void computeBiquad(const std::string& type, double fc, double gain,
                              double q, int sampleRate, double out[5]);

    EqProcessor processors_[2];
    std::atomic<int>  activeIdx_{0};
    std::atomic<bool> active_{false};
};

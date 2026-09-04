// The platform-free half of bt_codec.hh: turning the stack's bitmasks and
// enum values into words. Compiled on every platform, because the settings
// panel and the signal-chain readout both print these and must not disagree.
#include "bt_codec.hh"

namespace bt_codec {

std::string codecName(int codec) {
    switch (codec) {
    case kSbc:    return "SBC";
    case kAac:    return "AAC";
    case kAptX:   return "aptX";
    case kAptXHd: return "aptX HD";
    case kLdac:   return "LDAC";
    default:      return "Unknown";
    }
}

std::string sampleRateLabel(int mask) {
    switch (mask) {
    case kRate44100: return "44.1 kHz";
    case kRate48000: return "48 kHz";
    case kRate88200: return "88.2 kHz";
    case kRate96000: return "96 kHz";
    default:         return "";
    }
}

std::string bitDepthLabel(int mask) {
    switch (mask) {
    case kBits16: return "16-bit";
    case kBits24: return "24-bit";
    case kBits32: return "32-bit";
    default:      return "";
    }
}

// The numbers are the LDAC bitrates, and they run the opposite way to the
// enum: 0 is the best. Naming the rate rather than the index is what stops a
// listener reading "quality 0" as the worst one.
std::string ldacQualityLabel(int quality) {
    switch (quality) {
    case kLdac990:      return "990 kbps";
    case kLdac660:      return "660 kbps";
    case kLdac330:      return "330 kbps";
    case kLdacAdaptive: return "Adaptive";
    default:            return "";
    }
}

std::string summary(const Config& c) {
    if (!c.valid()) return "";
    std::string s = codecName(c.codec);
    const std::string rate = sampleRateLabel(c.sampleRate);
    const std::string bits = bitDepthLabel(c.bits);
    if (!rate.empty()) s += " " + rate;
    if (!bits.empty()) s += " / " + bits;
    // Only LDAC carries a quality, and printing one for the others would be
    // inventing a field the codec does not have.
    if (c.codec == kLdac) {
        const std::string q = ldacQualityLabel(c.ldacQuality);
        if (!q.empty()) s += " / " + q;
    }
    return s;
}

}  // namespace bt_codec

#pragma once
#include <string>
#include <vector>

struct EqFilter {
    std::string type; // "PK", "LSC", "HSC"
    double fc;        // center frequency Hz
    double gain;      // dB
    double q;
};

struct EqProfile {
    std::string name;
    std::string source;
    std::string form; // "over-ear", "in-ear", "earbud", or ""
    double preamp;    // dB
    std::vector<EqFilter> filters;
};

class EqProfileStore {
public:
    bool load(const std::string& jsonPath);
    const std::vector<EqProfile>& getAll() const { return profiles_; }
    const EqProfile* findByKey(const std::string& name,
                               const std::string& source,
                               const std::string& form) const;
private:
    std::vector<EqProfile> profiles_;
};

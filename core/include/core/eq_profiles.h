#pragma once
#include <string>
#include <vector>

// The numeric defaults match what the loader falls back to for a missing key,
// so a default-constructed filter is the same neutral one the parser would
// produce. They were previously uninitialised — harmless only because the
// loader assigned every field, which is not a property worth relying on.
struct EqFilter {
    std::string type = "PK"; // "PK", "LSC", "HSC"
    double fc   = 1000.0;    // center frequency Hz
    double gain = 0.0;       // dB
    double q    = 1.0;
};

struct EqProfile {
    std::string name;
    std::string source;
    std::string form;        // "over-ear", "in-ear", "earbud", or ""
    double preamp = 0.0;     // dB
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

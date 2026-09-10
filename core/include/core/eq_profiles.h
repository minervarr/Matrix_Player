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
    std::string form;        // "over-ear", "in-ear", or "earbud"
    // Which measurement RIG this came from, e.g. "GRAS 43AG-7", "HMS II.3",
    // "711". Empty when the source has only one. It used to be lost: the old
    // generator looked for a path component that was exactly a form name, and
    // AutoEq puts the rig in that same component, so a third of the catalogue
    // shipped with no form AND no rig, and two rigs of one form collided onto
    // one key where whichever sorted first won.
    std::string rig;
    // AutoEq's own accuracy ranking, 0 = best, read out of its
    // dbtools/update_result_indexes.py at generation time (see
    // tools/eq_index/build_eq_index.py). It is what lets one model with a
    // dozen measurements show the one measurement AutoEq's website shows.
    // Larger is worse; the catalogue is emitted already sorted by (name, rank),
    // so the FIRST row of a name is the recommended one and nothing has to
    // sort 8850 entries at startup.
    int    rank = 0;
    double preamp = 0.0;     // dB
    std::vector<EqFilter> filters;
};

class EqProfileStore {
public:
    bool load(const std::string& jsonPath);

    // The same parse, over bytes somebody else read. Android has no file to
    // open: eq_profiles.json ships inside the APK, so it arrives through
    // Host::dataReader() as a buffer, exactly the way the typefaces do. `label`
    // is only what the error lines name, so the two callers report the same way.
    //
    // core/ stays free of the reader: this takes plain bytes and knows nothing
    // about where they came from.
    bool loadFromMemory(const char* data, size_t size, const std::string& label);
    const std::vector<EqProfile>& getAll() const { return profiles_; }
    // Exact (name, source, form), then (name, source) as a fallback.
    //
    // The triple is a PERSISTED key -- eq_assignments and eq_headphones store
    // it -- so it has to survive the catalogue being regenerated. Restoring
    // the forms the old generator lost changes `form` for every crinacle,
    // Rtings and HypetheSonics row, and without the fallback every listener
    // with one of those saved would have silently lost their profile. The
    // fallback is also what makes a future AutoEq update non-breaking.
    const EqProfile* findByKey(const std::string& name,
                               const std::string& source,
                               const std::string& form) const;
private:
    std::vector<EqProfile> profiles_;
};

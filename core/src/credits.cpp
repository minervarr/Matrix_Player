#include "core/credits.h"

#include <string_view>

namespace credits {
namespace {

struct Role {
    const char* key;    // letters only, already upper-case
    const char* label;
    Act act;
    int order;          // reading order inside the act, not a rank
};

// Two keys that share a label share an act and an order, so "Organization"
// and "Label" land on one line, and a remix sits with the arrangement.
const Role kRoles[] = {
    { "COMPOSER",       "Composer",   Act::Music,       0 },
    { "LYRICIST",       "Lyricist",   Act::Music,       1 },
    { "WRITER",         "Writer",     Act::Music,       2 },
    { "ARRANGER",       "Arranger",   Act::Setting,     0 },
    { "REMIXER",        "Remixer",    Act::Setting,     1 },
    { "CONDUCTOR",      "Conductor",  Act::Performance, 0 },
    { "ORCHESTRA",      "Ensemble",   Act::Performance, 1 },
    { "ENSEMBLE",       "Ensemble",   Act::Performance, 1 },
    { "SOLOIST",        "Soloist",    Act::Performance, 2 },
    { "PERFORMER",      "Performer",  Act::Performance, 3 },
    { "FEATUREDARTIST", "Featured",   Act::Performance, 4 },
    { "PRODUCER",       "Producer",   Act::Recording,   0 },
    { "ENGINEER",       "Engineer",   Act::Recording,   1 },
    { "MIXER",          "Mixer",      Act::Recording,   2 },
    { "MIXINGENGINEER", "Mixer",      Act::Recording,   2 },
    { "LABEL",          "Label",      Act::Issue,       0 },
    { "ORGANIZATION",   "Label",      Act::Issue,       0 },
    { "PUBLISHER",      "Label",      Act::Issue,       0 },
    { "COPYRIGHT",      "Copyright",  Act::Issue,       1 },
};

std::string foldKey(std::string_view key) {
    std::string o;
    o.reserve(key.size());
    for (unsigned char c : key) {
        if (c == ' ' || c == '_' || c == '-' || c == '\t') continue;
        if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 32);
        o.push_back((char)c);
    }
    return o;
}

const Role* findRole(std::string_view key) {
    const std::string folded = foldKey(key);
    for (const Role& r : kRoles)
        if (folded == r.key) return &r;
    return nullptr;
}

std::string trim(std::string_view v) {
    size_t b = 0, e = v.size();
    while (b < e && (v[b] == ' ' || v[b] == '\t' || v[b] == '\n' || v[b] == '\r'))
        ++b;
    while (e > b && (v[e - 1] == ' ' || v[e - 1] == '\t' || v[e - 1] == '\n' ||
                     v[e - 1] == '\r'))
        --e;
    return std::string(v.substr(b, e - b));
}

struct Bucket {
    const char* label = "";
    Act act = Act::Music;
    int order = 0;
    std::vector<std::string> people;
};

Bucket* bucketFor(std::vector<Bucket>& buckets, const Role& role) {
    for (Bucket& b : buckets)
        if (b.act == role.act && std::string_view(b.label) == role.label) return &b;
    Bucket b;
    b.label = role.label;
    b.act = role.act;
    b.order = role.order;
    buckets.push_back(std::move(b));
    return &buckets.back();
}

void addPerson(Bucket& b, const std::string& name) {
    if (name.empty()) return;
    for (const std::string& have : b.people)
        if (have == name) return;
    b.people.push_back(name);
}

} // namespace

bool isCreditTag(std::string_view key) {
    return findRole(key) != nullptr;
}

const char* actHeading(Act act) {
    switch (act) {
    case Act::Music:       return "Music";
    case Act::Setting:     return "Arrangement";
    case Act::Performance: return "Performance";
    case Act::Recording:   return "Recording";
    case Act::Issue:       return "Release";
    }
    return "";
}

bool settingIsTheWork(const std::vector<CreditLine>& lines) {
    bool setting = false;
    for (const CreditLine& line : lines) {
        if (line.act == Act::Music) return false;
        if (line.act == Act::Setting) setting = true;
    }
    return setting;
}

std::vector<CreditLine> fromTags(const std::vector<std::string>& entries) {
    std::vector<Bucket> buckets;
    for (const std::string& entry : entries) {
        const size_t eq = entry.find('=');
        if (eq == std::string::npos) continue;
        const Role* role = findRole(std::string_view(entry).substr(0, eq));
        if (!role) continue;
        Bucket* b = bucketFor(buckets, *role);
        const std::string raw = trim(std::string_view(entry).substr(eq + 1));
        size_t start = 0;
        while (start <= raw.size()) {
            const size_t semi = raw.find(';', start);
            const size_t n = semi == std::string::npos ? raw.size() - start : semi - start;
            addPerson(*b, trim(std::string_view(raw).substr(start, n)));
            if (semi == std::string::npos) break;
            start = semi + 1;
        }
    }

    // File order arrives arbitrary. Group by act, and within an act keep
    // the reading order. A handful of roles, so a direct insertion keeps
    // the file free of <algorithm> for one loop.
    for (size_t i = 1; i < buckets.size(); ++i) {
        Bucket cur = std::move(buckets[i]);
        size_t j = i;
        while (j > 0 &&
               ((int)buckets[j - 1].act > (int)cur.act ||
                (buckets[j - 1].act == cur.act && buckets[j - 1].order > cur.order))) {
            buckets[j] = std::move(buckets[j - 1]);
            --j;
        }
        buckets[j] = std::move(cur);
    }

    std::vector<CreditLine> out;
    out.reserve(buckets.size());
    for (const Bucket& b : buckets) {
        if (b.people.empty()) continue;
        CreditLine line;
        line.act = b.act;
        line.role = b.label;
        line.people = b.people;
        out.push_back(std::move(line));
    }
    return out;
}

} // namespace credits

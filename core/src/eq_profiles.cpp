#include "core/eq_profiles.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

// ── JSON ────────────────────────────────────────────────────────────────────
// A purpose-built reader for exactly the shape eq_profiles.json has: an array
// of objects whose values are strings, numbers, and one nested array of
// objects.
//
// It replaces nlohmann/json, which was the single most expensive header in
// this tree — 920 KB of it, included by this one 65-line file, compiling to an
// 848 KB object. All of that built a full DOM for the 8666 profiles and 86660
// filters in the shipped file, only for the loop below to copy four fields out
// of each node and throw the DOM away. This parses STRAIGHT into
// EqProfile/EqFilter, so no intermediate value is ever materialised.
//
// Only what this file's schema can contain is implemented, but what IS
// implemented follows RFC 8259: full string escapes including \uXXXX with
// surrogate pairs, and the complete number grammar (sign, fraction, exponent).
// Unknown keys and unknown value types are skipped rather than rejected, so a
// future field added to the generator does not break loading.
namespace {

constexpr bool jsonSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

void appendUtf8(std::string& s, unsigned cp) {
    if (cp < 0x80) {
        s += (char)cp;
    } else if (cp < 0x800) {
        s += (char)(0xC0 | (cp >> 6));
        s += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        s += (char)(0xE0 | (cp >> 12));
        s += (char)(0x80 | ((cp >> 6) & 0x3F));
        s += (char)(0x80 | (cp & 0x3F));
    } else {
        s += (char)(0xF0 | (cp >> 18));
        s += (char)(0x80 | ((cp >> 12) & 0x3F));
        s += (char)(0x80 | ((cp >> 6) & 0x3F));
        s += (char)(0x80 | (cp & 0x3F));
    }
}

class JsonReader {
public:
    JsonReader(const char* begin, const char* end) : p_(begin), end_(end) {}

    void skipWs() { while (p_ < end_ && jsonSpace(*p_)) p_++; }

    bool atEnd() const { return p_ >= end_; }
    char peek() const { return p_ < end_ ? *p_ : '\0'; }

    bool consume(char c) {
        skipWs();
        if (p_ < end_ && *p_ == c) { p_++; return true; }
        return false;
    }

    bool expect(char c) { return consume(c); }

    // A JSON string, escapes resolved, appended into `out` (cleared first).
    bool readString(std::string& out) {
        out.clear();
        skipWs();
        if (p_ >= end_ || *p_ != '"') return false;
        p_++;
        while (p_ < end_) {
            const char c = *p_;
            if (c == '"') { p_++; return true; }
            if (c != '\\') {
                // Copy the whole unescaped run in one go rather than a byte at
                // a time — almost every string in this file is exactly one run.
                const char* run = p_;
                while (p_ < end_ && *p_ != '"' && *p_ != '\\') p_++;
                out.append(run, (size_t)(p_ - run));
                continue;
            }
            p_++;                                   // the backslash
            if (p_ >= end_) return false;
            const char e = *p_++;
            switch (e) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'u': {
                    unsigned cp = 0;
                    if (!readHex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {          // high surrogate
                        if (p_ + 1 >= end_ || p_[0] != '\\' || p_[1] != 'u') return false;
                        p_ += 2;
                        unsigned lo = 0;
                        if (!readHex4(lo)) return false;
                        if (lo < 0xDC00 || lo > 0xDFFF) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {   // lone low surrogate
                        return false;
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;                               // unterminated
    }

    bool readNumber(double& out) {
        skipWs();
        const char* start = p_;
        if (p_ < end_ && *p_ == '-') p_++;      // JSON allows no leading '+'
        while (p_ < end_ && *p_ >= '0' && *p_ <= '9') p_++;
        if (p_ < end_ && *p_ == '.') {
            p_++;
            while (p_ < end_ && *p_ >= '0' && *p_ <= '9') p_++;
        }
        if (p_ < end_ && (*p_ == 'e' || *p_ == 'E')) {
            const char* save = p_;
            p_++;
            if (p_ < end_ && (*p_ == '-' || *p_ == '+')) p_++;
            if (p_ < end_ && *p_ >= '0' && *p_ <= '9') {
                while (p_ < end_ && *p_ >= '0' && *p_ <= '9') p_++;
            } else {
                p_ = save;                          // "1e" — the 'e' isn't ours
            }
        }
        if (p_ == start) return false;
        // strtod is correctly rounded, so this lands on the same double the DOM
        // parser produced. The buffer is NUL-terminated by the caller, and the
        // token is bounded by a delimiter either way.
        char* stop = nullptr;
        out = std::strtod(start, &stop);
        return stop == p_;
    }

    // Steps over any value, so an unrecognised key costs nothing but a scan.
    bool skipValue() {
        skipWs();
        if (p_ >= end_) return false;
        switch (*p_) {
            case '"': { std::string junk; return readString(junk); }
            case '{': case '[': {
                const char open = *p_, close = (open == '{') ? '}' : ']';
                p_++;
                int depth = 1;
                while (p_ < end_ && depth > 0) {
                    if (*p_ == '"') { std::string junk; if (!readString(junk)) return false; continue; }
                    if (*p_ == open)  depth++;
                    else if (*p_ == close) depth--;
                    p_++;
                }
                return depth == 0;
            }
            case 't': return lit("true");
            case 'f': return lit("false");
            case 'n': return lit("null");
            default:  { double junk; return readNumber(junk); }
        }
    }

private:
    bool readHex4(unsigned& out) {
        if (p_ + 4 > end_) return false;
        out = 0;
        for (int i = 0; i < 4; i++) {
            const char c = *p_++;
            unsigned d;
            if      (c >= '0' && c <= '9') d = (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
            else return false;
            out = (out << 4) | d;
        }
        return true;
    }

    bool lit(std::string_view s) {
        if ((size_t)(end_ - p_) < s.size()) return false;
        if (std::string_view(p_, s.size()) != s) return false;
        p_ += s.size();
        return true;
    }

    const char* p_;
    const char* end_;
};

// One {"type":..,"fc":..,"gain":..,"q":..} object. Defaults match what
// nlohmann's obj.value(key, default) returned for a missing key.
bool readFilter(JsonReader& r, EqFilter& f) {
    f.type = "PK";
    f.fc   = 1000.0;
    f.gain = 0.0;
    f.q    = 1.0;

    if (!r.expect('{')) return false;
    if (r.consume('}')) return true;
    std::string key;
    do {
        if (!r.readString(key)) return false;
        if (!r.expect(':'))     return false;
        if      (key == "type") { if (!r.readString(f.type)) return false; }
        else if (key == "fc")   { if (!r.readNumber(f.fc))   return false; }
        else if (key == "gain") { if (!r.readNumber(f.gain)) return false; }
        else if (key == "q")    { if (!r.readNumber(f.q))    return false; }
        else                    { if (!r.skipValue())        return false; }
    } while (r.consume(','));
    return r.expect('}');
}

bool readProfile(JsonReader& r, EqProfile& p) {
    p.name.clear();
    p.source.clear();
    p.form.clear();
    p.preamp = 0.0;
    p.filters.clear();

    if (!r.expect('{')) return false;
    if (r.consume('}')) return true;
    std::string key;
    do {
        if (!r.readString(key)) return false;
        if (!r.expect(':'))     return false;
        if      (key == "name")   { if (!r.readString(p.name))    return false; }
        else if (key == "source") { if (!r.readString(p.source))  return false; }
        else if (key == "form")   { if (!r.readString(p.form))    return false; }
        else if (key == "preamp") { if (!r.readNumber(p.preamp))  return false; }
        else if (key == "filters") {
            if (!r.expect('[')) return false;
            if (!r.consume(']')) {
                do {
                    EqFilter f;
                    if (!readFilter(r, f)) return false;
                    p.filters.push_back(std::move(f));
                } while (r.consume(','));
                if (!r.expect(']')) return false;
            }
        }
        else { if (!r.skipValue()) return false; }
    } while (r.consume(','));
    return r.expect('}');
}

// Whole file into memory. At ~4.8 MB this is one read and one allocation,
// where the stream parser it replaces refilled a buffer all the way through.
bool readWholeFile(const std::string& path, std::string& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    bool ok = false;
    if (std::fseek(f, 0, SEEK_END) == 0) {
        const long len = std::ftell(f);
        if (len >= 0 && std::fseek(f, 0, SEEK_SET) == 0) {
            out.resize((size_t)len);
            ok = len == 0 || std::fread(&out[0], 1, (size_t)len, f) == (size_t)len;
        }
    }
    std::fclose(f);
    return ok;
}

} // namespace

bool EqProfileStore::load(const std::string& jsonPath) {
    std::string text;
    if (!readWholeFile(jsonPath, text)) {
        fprintf(stderr, "[EQ][ERROR] Failed to open %s\n", jsonPath.c_str());
        return false;
    }

    JsonReader r(text.data(), text.data() + text.size());
    profiles_.clear();

    bool ok = r.expect('[');
    if (ok && !r.consume(']')) {
        do {
            EqProfile p;
            if (!readProfile(r, p)) { ok = false; break; }
            profiles_.push_back(std::move(p));
        } while (r.consume(','));
        if (ok) ok = r.expect(']');
    }

    if (!ok) {
        fprintf(stderr, "[EQ][ERROR] JSON parse error in %s\n", jsonPath.c_str());
        profiles_.clear();
        return false;
    }

    std::sort(profiles_.begin(), profiles_.end(),
        [](const EqProfile& a, const EqProfile& b) {
            return std::lexicographical_compare(
                a.name.begin(), a.name.end(), b.name.begin(), b.name.end(),
                [](char x, char y) {
                    // ASCII fold, locale-independent: the C locale's tolower
                    // did exactly this, and nothing here should start
                    // depending on a locale.
                    const auto lower = [](unsigned char c) {
                        return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + 32) : c;
                    };
                    return lower((unsigned char)x) < lower((unsigned char)y);
                });
        });

    printf("[EQ] Loaded %zu profiles from %s\n", profiles_.size(), jsonPath.c_str());
    return true;
}

const EqProfile* EqProfileStore::findByKey(const std::string& name,
                                           const std::string& source,
                                           const std::string& form) const {
    for (const EqProfile& p : profiles_) {
        if (p.name == name && p.source == source && p.form == form)
            return &p;
    }
    return nullptr;
}

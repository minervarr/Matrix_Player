#include "core/facets.h"

#include <algorithm>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace facets {
namespace {

// ── UTF-8 ───────────────────────────────────────────────────────────────────

// Decodes one codepoint at s[i], advancing i. Invalid bytes are returned as
// themselves so malformed tags degrade instead of truncating a name.
uint32_t nextCp(const std::string& s, size_t& i) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80) { i += 1; return c; }
    size_t len = (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
    if (len == 1 || i + len > s.size()) { i += 1; return c; }
    uint32_t cp = c & (0xFF >> (len + 1));
    for (size_t k = 1; k < len; k++) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
    i += len;
    return cp;
}

void appendCp(std::string& out, uint32_t cp) {
    if (cp < 0x80) { out += (char)cp; return; }
    if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
        return;
    }
    if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
        return;
    }
    out += (char)(0xF0 | (cp >> 18));
    out += (char)(0x80 | ((cp >> 12) & 0x3F));
    out += (char)(0x80 | ((cp >> 6) & 0x3F));
    out += (char)(0x80 | (cp & 0x3F));
}

// Latin-1 Supplement letters folded to ASCII, indexed by cp - 0xC0. Covers
// Spanish/Portuguese/French/German/Nordic, which is what this library's tags
// actually contain. Latin Extended-A (Polish, Czech, Turkish...) is passed
// through unfolded on purpose: exact equality still matches those names, and
// a half-remembered mapping table is worse than none. Non-Latin scripts
// (Cyrillic, CJK) are never touched — normalize() must not damage them, since
// matching there is by equality.
const char* const kLatin1Fold[64] = {
    "a","a","a","a","a","a","ae","c", "e","e","e","e", "i","i","i","i",
    "d","n","o","o","o","o","o","",  "o","u","u","u","u","y","th","ss",
    "a","a","a","a","a","a","ae","c", "e","e","e","e", "i","i","i","i",
    "d","n","o","o","o","o","o","",  "o","u","u","u","u","y","th","y",
};

// ── Album-derived values ────────────────────────────────────────────────────

// The most common non-zero value in `counts`; ties break to the smallest, so
// the answer never depends on map iteration luck. 0 when nothing was tagged.
int majority(const std::map<int, int>& counts) {
    int best = 0, bestN = 0;
    for (const auto& kv : counts) {
        if (kv.first == 0) continue;
        if (kv.second > bestN || (kv.second == bestN && best != 0 && kv.first < best)) {
            best  = kv.first;
            bestN = kv.second;
        }
    }
    return best;
}

const char* typeName(Album::ReleaseType t) {
    switch (t) {
        case Album::ReleaseType::Album:       return "Album";
        case Album::ReleaseType::Ep:          return "EP";
        case Album::ReleaseType::Single:      return "Single";
        case Album::ReleaseType::Remix:       return "Remix";
        case Album::ReleaseType::Compilation: return "Compilation";
        case Album::ReleaseType::Live:        return "Live";
    }
    return "Album";
}

// ── Facts: everything the matcher can ask about ONE album, derived once ─────
//
// This struct is the whole performance story of this file. The matching code
// used to call albumYear()/albumQuality()/albumGenre() from inside the
// per-chip test, so every one of them ran once per album PER CANDIDATE, each
// building a fresh std::map — and suggest() has a candidate per distinct value
// in the library. That product, not the library walk, is what made one
// keystroke cost 344 ms at 2000 albums and 1.5 s at 4000.
//
// The tie-breaks below are load-bearing and easy to lose in a rewrite: the
// year and the bit depth resolve to the SMALLEST value on a tie (majority()
// over an ordered map), and the genre resolves to the alphabetically first.
// Anything that replaces the maps must keep both, or the answers change
// silently for exactly the albums whose tags disagree.
struct Facts {
    int         year   = 0;      // 0 = nothing tagged, and it renders "Unknown"
    int         decade = 0;      // 0 whenever year is 0
    std::string quality;         // "24-bit" / "16-bit" / "DSD", empty if absent
    std::string genre;           // empty if absent
    const char* type = "Album";  // never null — typeName() is total

    // The name haystacks, ALREADY NORMALIZED, in the order nameHit() tried
    // them: artist, then displayName, then every track title. Hoisting the
    // normalization here is the single largest constant-factor win, because it
    // used to run per candidate per album per field.
    std::vector<std::string> hay;
    // Which character classes each haystack contains — see charMask(). Lets a
    // needle be ruled out of a substring search by one AND, which matters
    // because the name path is the one that cannot be bucketed by value.
    std::vector<uint64_t>    hayMask;
};

// A set bit per character class present: a-z, 0-9, space, and ONE catch-all
// bit for everything else (which after normalize() means non-Latin scripts).
// Collapsing those into a single bit is safe in the only direction that
// matters: it can only make the prune below say "might match" when it does
// not, never "cannot match" when it can.
uint64_t charMask(const std::string& s) {
    uint64_t m = 0;
    for (unsigned char c : s) {
        if (c >= 'a' && c <= 'z')      m |= 1ull << (c - 'a');
        else if (c >= '0' && c <= '9') m |= 1ull << (26 + (c - '0'));
        else if (c == ' ')             m |= 1ull << 36;
        else                           m |= 1ull << 37;
    }
    return m;
}

Facts makeFacts(const Album& a) {
    Facts f;

    std::map<int, int>         years;
    std::map<int, int>         depths;
    std::map<std::string, int> genres;
    for (const Track& t : a.tracks) {
        years[t.year]++;
        depths[t.bitDepth]++;
        if (!t.genre.empty()) genres[t.genre]++;
    }

    f.year   = majority(years);
    f.decade = f.year != 0 ? (f.year / 10) * 10 : 0;

    if (a.hasDsd) {
        f.quality = "DSD";
    } else {
        int d = majority(depths);
        if (d > 0) f.quality = std::to_string(d) + "-bit";
    }

    int bestN = 0;
    for (const auto& kv : genres)
        if (kv.second > bestN) { f.genre = kv.first; bestN = kv.second; }

    f.type = typeName(a.releaseType);

    f.hay.reserve(2 + a.tracks.size());
    f.hay.push_back(normalize(a.artist));
    f.hay.push_back(normalize(a.displayName));
    for (const Track& t : a.tracks) f.hay.push_back(normalize(t.title));

    f.hayMask.reserve(f.hay.size());
    for (const std::string& h : f.hay) f.hayMask.push_back(charMask(h));
    return f;
}

std::vector<Facts> makeFacts(const std::vector<Album>& lib) {
    std::vector<Facts> out;
    out.reserve(lib.size());
    for (const Album& a : lib) out.push_back(makeFacts(a));
    return out;
}

// ── Matching one chip ───────────────────────────────────────────────────────

// `needle` is already normalized. An EMPTY needle matches everything, which is
// correct for its other caller (an empty box means "no name constraint") and
// is exactly why suggest() must never build a Name candidate out of an
// untagged field — see the guard there.
bool nameHitFacts(const Facts& f, const std::string& needle) {
    if (needle.empty()) return true;
    const uint64_t needleMask = charMask(needle);
    const size_t   nlen       = needle.size();

    for (size_t i = 0; i < f.hay.size(); i++) {
        const std::string& h = f.hay[i];

        // The substring search is the expensive half, and a needle holding a
        // character this haystack does not contain anywhere cannot possibly be
        // inside it. One AND rules that out without touching the bytes — which
        // is what keeps this path affordable, since a fuzzy name match cannot
        // be bucketed by value the way every other kind is.
        if ((needleMask & ~f.hayMask[i]) == 0 &&
            h.find(needle) != std::string::npos)
            return true;

        // A chip's value came from a suggestion, so it is already a real
        // library value; the distance check only absorbs the listener's own
        // typo when the chip was accepted from partial text. Two edits cannot
        // bridge a length gap of more than two, so the window is checked here
        // rather than inside the O(n·m) loop.
        const size_t hlen = h.size();
        const size_t gap  = hlen > nlen ? hlen - nlen : nlen - hlen;
        if (gap <= 2 && editDistance(h, needle, 2) <= 2) return true;
    }
    return false;
}

bool chipHitFacts(const Facts& f, const Chip& c) {
    switch (c.kind) {
        case Kind::Name:    return nameHitFacts(f, normalize(c.value));
        case Kind::Year:
        case Kind::Decade:  return f.year != 0 && f.year >= c.from && f.year <= c.to;
        case Kind::Quality: return f.quality == c.value;
        case Kind::Type:    return c.value == f.type;
        case Kind::Genre:   return f.genre == c.value;
    }
    return false;
}

bool matchesFacts(const Facts& f, const std::vector<Chip>& chips);

// Year and Decade are one group: they are alternative ways to say "when".
Kind groupOf(Kind k) { return k == Kind::Year ? Kind::Decade : k; }

int countMatchingFacts(const std::vector<Facts>& facts,
                       const std::vector<Chip>& chips) {
    int n = 0;
    for (const Facts& f : facts)
        if (matchesFacts(f, chips)) n++;
    return n;
}

bool alreadyPlaced(const std::vector<Chip>& chips, const Chip& c) {
    for (const Chip& x : chips)
        if (x == c) return true;
    return false;
}

Chip mkChip(Kind k, const std::string& value, int from = 0, int to = 0) {
    Chip c;
    c.kind  = k;
    c.value = value;
    c.from  = from;
    c.to    = to;
    return c;
}

bool allDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

// Does `typed` plausibly refer to this candidate? Substring on the normalized
// form, or a small edit distance so a typo still finds the record.
//
// Years get their own rule rather than being compared as text: typing "1993"
// must offer BOTH the exact year and the decade holding it, because those are
// two different questions and the app must never guess which one was meant.
// As plain strings "1993" and "1990s" are two edits apart and the decade would
// silently vanish.
bool typedMatches(const std::string& typedNorm, const std::string& label, const Chip& c) {
    if (typedNorm.empty()) return true;

    if (allDigits(typedNorm) && (c.kind == Kind::Year || c.kind == Kind::Decade)) {
        if (label.rfind(typedNorm, 0) == 0) return true;      // prefix: "199" → 1993, 1990s
        // The width test guards the CONVERSION, and that order is load-bearing:
        // a search box accepts as many digits as the listener cares to hold
        // down, and stoi answers anything past INT_MAX by THROWING. Testing
        // size() afterwards — which is what this did — meant thirteen digits
        // aborted the process outright.
        if (typedNorm.size() != 4) return false;
        int v = std::stoi(typedNorm);
        return v >= c.from && v <= c.to;                      // 1993 → its decade
    }

    std::string l = normalize(label);
    if (l.find(typedNorm) != std::string::npos) return true;
    int budget = typedNorm.size() <= 4 ? 1 : 2;
    return editDistance(l, typedNorm, budget) <= budget;
}

}  // namespace

// ── Public API ──────────────────────────────────────────────────────────────

int albumYear(const Album& a) {
    std::map<int, int> years;
    for (const Track& t : a.tracks) years[t.year]++;
    return majority(years);
}

std::string normalize(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool lastSpace = true;   // leading whitespace collapses away
    size_t i = 0;
    while (i < s.size()) {
        uint32_t cp = nextCp(s, i);
        std::string piece;
        if (cp >= 0xC0 && cp <= 0xFF) {
            piece = kLatin1Fold[cp - 0xC0];
        } else if (cp < 0x80) {
            if (cp >= 'A' && cp <= 'Z')                       piece = (char)(cp + 32);
            else if ((cp >= 'a' && cp <= 'z') || (cp >= '0' && cp <= '9')) piece = (char)cp;
            else if (cp == ' ' || cp == '\t' || cp == '\n')   piece = " ";
            // everything else is punctuation: dropped
        } else {
            appendCp(piece, cp);   // Cyrillic, CJK, Latin Extended: untouched
        }
        if (piece == " ") {
            if (!lastSpace) out += ' ';
            lastSpace = true;
        } else if (!piece.empty()) {
            out += piece;
            lastSpace = false;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

int editDistance(const std::string& a, const std::string& b, int max) {
    if (a == b) return 0;
    int n = (int)a.size(), m = (int)b.size();
    if (std::abs(n - m) > max) return max + 1;
    std::vector<int> prev(m + 1), cur(m + 1);
    for (int j = 0; j <= m; j++) prev[j] = j;
    for (int i = 1; i <= n; i++) {
        cur[0] = i;
        int rowBest = cur[0];
        for (int j = 1; j <= m; j++) {
            int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            cur[j] = std::min({ prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost });
            rowBest = std::min(rowBest, cur[j]);
        }
        if (rowBest > max) return max + 1;   // no completion can come back under
        prev.swap(cur);
    }
    return prev[m] > max ? max + 1 : prev[m];
}

bool sameGroup(const Chip& a, const Chip& b) {
    return groupOf(a.kind) == groupOf(b.kind);
}

// The public entry point is a one-album adapter over the same code the fast
// path uses. That is deliberate: an internal "fast matcher" beside a public
// "reference matcher" would be two answers to one question, and the day they
// disagree the grid and the counts beside it disagree. Here there is nothing
// to keep in agreement.
//
// rebuildGridIndices() in the GUI calls this per album, and it is CHEAPER than
// what it replaced — the old body re-derived year/quality/genre once per chip
// and re-normalized every haystack per Name chip.
bool matches(const Album& a, const std::vector<Chip>& chips) {
    return matchesFacts(makeFacts(a), chips);
}

// Same anonymous namespace as above — declared before matches() so the adapter
// can call it, defined here so the reading order stays public-first.
namespace {

bool matchesFacts(const Facts& f, const std::vector<Chip>& chips) {
    // OR inside a group, AND across groups: walk the groups present and
    // require a hit in each.
    for (const Chip& c : chips) {
        Kind g = groupOf(c.kind);
        bool firstOfGroup = true;
        for (const Chip& e : chips) {
            if (&e == &c) break;
            if (groupOf(e.kind) == g) { firstOfGroup = false; break; }
        }
        if (!firstOfGroup) continue;   // group already evaluated

        bool any = false;
        for (const Chip& e : chips)
            if (groupOf(e.kind) == g && chipHitFacts(f, e)) { any = true; break; }
        if (!any) return false;
    }
    return true;
}

}  // namespace

std::vector<Suggestion> suggest(const std::vector<Album>& library,
                                const std::string& typed,
                                const std::vector<Chip>& chips) {
    const std::string typedNorm = normalize(typed);
    const std::vector<Facts> facts = makeFacts(library);

    // Candidates are built ONLY from values the library actually holds, which
    // is what guarantees a value that exists nowhere is never offered — the
    // listener never meets a dead "(0)" row for something they cannot have.
    //
    // Dedup goes through a hash of the SAME four fields Chip::operator==
    // compares. It used to be a linear scan over everything accepted so far,
    // which with two Name candidates per album made building the list
    // quadratic in the library on its own. A field added to Chip must be added
    // to this key too, or two different chips start colliding.
    std::vector<Chip> cands;
    std::vector<std::string> labels;
    std::unordered_set<std::string> seen;
    auto add = [&](const Chip& c, const std::string& label) {
        std::string key = std::to_string((int)c.kind) + '\x1f' + c.value + '\x1f' +
                          std::to_string(c.from) + '-' + std::to_string(c.to);
        if (!seen.insert(std::move(key)).second) return;
        cands.push_back(c);
        labels.push_back(label);
    };

    for (size_t i = 0; i < library.size(); i++) {
        const Album& a = library[i];
        const Facts& f = facts[i];

        if (f.year != 0) {
            add(mkChip(Kind::Year, std::to_string(f.year), f.year, f.year),
                std::to_string(f.year));
            add(mkChip(Kind::Decade, std::to_string(f.decade) + "s",
                       f.decade, f.decade + 9),
                std::to_string(f.decade) + "s");
        }
        if (!f.quality.empty()) add(mkChip(Kind::Quality, f.quality), f.quality);

        add(mkChip(Kind::Type, f.type), f.type);

        if (!f.genre.empty()) add(mkChip(Kind::Genre, f.genre), f.genre);

        // Names are only offered once the listener is actually typing —
        // with an empty box they would bury the handful of attribute rows
        // under every artist and record in the library.
        //
        // Both are guarded like quality and genre above, and for a sharper
        // reason than tidiness: nameHitFacts() treats an EMPTY needle as
        // matching everything, so an untagged artist would put up a chip with
        // no text on it that filtered nothing — and being a match for the whole
        // library, the count ranking would sort it FIRST, which is the row
        // Tab/Enter accepts. The guard is on the NORMALIZED form (f.hay), so a
        // name that is nothing but punctuation is caught by the same test.
        if (!typedNorm.empty()) {
            if (!f.hay[0].empty()) add(mkChip(Kind::Name, a.artist),      a.artist);
            if (!f.hay[1].empty()) add(mkChip(Kind::Name, a.displayName), a.displayName);
        }
    }

    // ── Which candidates survive, before anything is counted ────────────────
    // Filtering first is what bounds the counting below: a typed query throws
    // away most of the attribute candidates and all but a few names.
    std::vector<size_t> live;
    live.reserve(cands.size());
    for (size_t i = 0; i < cands.size(); i++) {
        if (alreadyPlaced(chips, cands[i])) continue;
        if (!typedMatches(typedNorm, labels[i], cands[i])) continue;
        live.push_back(i);
    }

    // ── The context every candidate is counted against, computed ONCE ───────
    // A candidate of group g is counted against withoutGroup(chips, g) — every
    // OTHER placed group satisfied. So instead of rebuilding that chip vector
    // and rescanning the library per candidate, record per album which placed
    // groups it satisfies, and the per-candidate question becomes one integer
    // compare: "all placed groups except possibly my own".
    std::vector<Kind> chipGroups;              // distinct, in first-seen order
    for (const Chip& c : chips) {
        Kind g = groupOf(c.kind);
        bool known = false;
        for (Kind k : chipGroups) known = known || k == g;
        if (!known) chipGroups.push_back(g);
    }
    const uint32_t allGroups = chipGroups.empty()
        ? 0u : (uint32_t)((1u << chipGroups.size()) - 1u);

    auto bitOf = [&](Kind g) -> uint32_t {
        for (size_t i = 0; i < chipGroups.size(); i++)
            if (chipGroups[i] == g) return 1u << i;
        return 0u;   // a group with no chip placed constrains nothing
    };

    std::vector<uint32_t> okMask(facts.size(), 0u);
    for (size_t gi = 0; gi < chipGroups.size(); gi++) {
        const uint32_t bit = 1u << gi;
        for (const Chip& c : chips) {
            if (groupOf(c.kind) != chipGroups[gi]) continue;
            for (size_t ai = 0; ai < facts.size(); ai++) {
                if (okMask[ai] & bit) continue;           // this group already hit
                if (chipHitFacts(facts[ai], c)) okMask[ai] |= bit;
            }
        }
    }

    // ── Counting, one library pass per GROUP rather than per candidate ──────
    // For every kind except Name, a chip test is an equality on ONE derived
    // value of the album, so an album contributes to exactly one candidate of
    // that group — two in the Year/Decade group, which is precisely why those
    // two kinds share a group and share this pass. Bucketing those hits counts
    // every candidate of the group in a single walk.
    std::vector<int> count(cands.size(), 0);

    auto bucketPass = [&](Kind group, const std::vector<size_t>& members) {
        if (members.empty()) return;
        std::unordered_map<std::string, size_t> byValue;   // chip value → cands index
        for (size_t i : members) byValue.emplace(cands[i].value, i);

        const uint32_t ownBit = bitOf(group);
        for (size_t ai = 0; ai < facts.size(); ai++) {
            if ((okMask[ai] | ownBit) != allGroups) continue;
            const Facts& f = facts[ai];

            auto bump = [&](const std::string& value) {
                if (value.empty()) return;
                auto it = byValue.find(value);
                if (it != byValue.end()) count[it->second]++;
            };
            if (group == Kind::Decade) {
                if (f.year == 0) continue;      // matches chipHitFacts's own guard
                bump(std::to_string(f.year));                 // the Year candidate
                bump(std::to_string(f.decade) + "s");         // the Decade candidate
            } else if (group == Kind::Quality) {
                bump(f.quality);
            } else if (group == Kind::Genre) {
                bump(f.genre);
            } else if (group == Kind::Type) {
                bump(f.type);
            }
        }
    };

    std::vector<size_t> byGroup[5];   // Decade(Year+Decade), Quality, Type, Genre, Name
    std::vector<size_t> nameCands;
    for (size_t i : live) {
        switch (groupOf(cands[i].kind)) {
            case Kind::Decade:  byGroup[0].push_back(i); break;
            case Kind::Quality: byGroup[1].push_back(i); break;
            case Kind::Type:    byGroup[2].push_back(i); break;
            case Kind::Genre:   byGroup[3].push_back(i); break;
            case Kind::Name:    nameCands.push_back(i);  break;
            default: break;
        }
    }
    bucketPass(Kind::Decade,  byGroup[0]);
    bucketPass(Kind::Quality, byGroup[1]);
    bucketPass(Kind::Type,    byGroup[2]);
    bucketPass(Kind::Genre,   byGroup[3]);

    // ── Names: the one group that cannot be bucketed ────────────────────────
    // A name hit is FUZZY, so one album can satisfy several name candidates at
    // once and the value is not a key — the count has to be a real scan per
    // distinct needle. Two things bound it: names only exist while the box has
    // text in it, and the needle is deduplicated NORMALIZED, since that is all
    // chipHitFacts() looks at.
    //
    // Neither bound is enough on its own. One common letter typed into a large
    // library matches most artists AND most record titles, which is thousands
    // of needles times thousands of albums — so the field is also CAPPED, and
    // the cap is a real narrowing of what gets offered, not an optimization
    // that hides behind an unchanged answer:
    //
    //   * The row on screen holds eight suggestions (kMaxSuggestRows in the
    //     GUI). A search that offers two thousand names is not offering
    //     anything, so the ones past the cap could never be reached anyway.
    //   * Which ones survive is decided by how well they match what was
    //     TYPED — a prefix beats a match in the middle, and a shorter label
    //     beats a longer one (typing "rad" should reach the artist Radiohead
    //     before every track whose title happens to contain it). Ties go
    //     alphabetically, so the choice never depends on library order.
    //
    // Attribute candidates (year, decade, quality, type, genre) are NOT capped:
    // there are only ever a handful of them and they are counted by bucketing,
    // which costs one pass regardless.
    static constexpr size_t kMaxNameCands = 64;
    if (nameCands.size() > kMaxNameCands) {
        std::stable_sort(nameCands.begin(), nameCands.end(),
                         [&](size_t a, size_t b) {
            const std::string la = normalize(labels[a]);
            const std::string lb = normalize(labels[b]);
            const bool pa = la.rfind(typedNorm, 0) == 0;
            const bool pb = lb.rfind(typedNorm, 0) == 0;
            if (pa != pb) return pa;
            if (la.size() != lb.size()) return la.size() < lb.size();
            return la < lb;
        });
        nameCands.resize(kMaxNameCands);
    }

    if (!nameCands.empty()) {
        const uint32_t ownBit = bitOf(Kind::Name);
        std::unordered_map<std::string, int> byNeedle;
        for (size_t i : nameCands) {
            const std::string needle = normalize(cands[i].value);
            auto it = byNeedle.find(needle);
            if (it == byNeedle.end()) {
                int n = 0;
                for (size_t ai = 0; ai < facts.size(); ai++) {
                    if ((okMask[ai] | ownBit) != allGroups) continue;
                    if (nameHitFacts(facts[ai], needle)) n++;
                }
                it = byNeedle.emplace(needle, n).first;
            }
            count[i] = it->second;
        }
    }

    // Built from the groups that were actually COUNTED, not from `live` — a
    // name dropped by the cap above must not come back as a row whose count
    // was never computed, which would read on screen as "this exists but is
    // blocked", the one thing a disabled row is supposed to mean.
    std::vector<size_t> kept;
    kept.reserve(live.size());
    for (int g = 0; g < 4; g++)
        kept.insert(kept.end(), byGroup[g].begin(), byGroup[g].end());
    kept.insert(kept.end(), nameCands.begin(), nameCands.end());

    std::vector<Suggestion> out;
    out.reserve(kept.size());
    for (size_t i : kept) {
        Suggestion s;
        s.chip    = cands[i];
        s.label   = labels[i];
        s.count   = count[i];
        s.enabled = s.count > 0;
        out.push_back(s);
    }

    // Reachable first, then the biggest result set, then alphabetically so
    // the order never depends on library iteration.
    std::sort(out.begin(), out.end(), [](const Suggestion& a, const Suggestion& b) {
        if (a.enabled != b.enabled) return a.enabled;
        if (a.count   != b.count)   return a.count > b.count;
        return a.label < b.label;
    });
    return out;
}

EmptyReason explainEmpty(const std::vector<Album>& library,
                         const std::vector<Chip>& chips) {
    EmptyReason r;
    if (chips.empty()) return r;

    // Derived once for every scan below. This function walks the library once
    // per chip (dropping each in turn), and the GUI's searchEmptyReason() calls
    // it again per blocked suggestion row — so re-deriving per walk multiplied
    // the same work several times over.
    const std::vector<Facts> facts = makeFacts(library);
    if (countMatchingFacts(facts, chips) > 0) return r;
    r.empty = true;

    // The LAST chip is what the listener just asked for, so it is the subject
    // of the sentence; the culprit is whichever earlier chip, removed, brings
    // results back. Saying "24-bit matched nothing" would blame the wish
    // instead of the constraint that blocked it.
    const Chip& subject = chips.back();
    for (size_t i = 0; i + 1 < chips.size(); i++) {
        std::vector<Chip> without;
        for (size_t j = 0; j < chips.size(); j++)
            if (j != i) without.push_back(chips[j]);
        if (countMatchingFacts(facts, without) == 0) continue;

        r.culpritIndex = (int)i;
        r.message = "No " + subject.value + " in " + chips[i].value;

        // Where the subject DOES live, so the listener has somewhere to go.
        int lo = 0, hi = 0;
        for (const Facts& f : facts) {
            if (!chipHitFacts(f, subject)) continue;
            if (f.year == 0) continue;
            if (lo == 0 || f.year < lo) lo = f.year;
            if (f.year > hi) hi = f.year;
        }
        if (lo != 0 && chips[i].to != 0 && lo > chips[i].to)
            r.message += " — your " + subject.value + " releases are from " +
                         std::to_string(lo) + " onward.";
        else if (hi != 0 && chips[i].from != 0 && hi < chips[i].from)
            r.message += " — your " + subject.value + " releases stop at " +
                         std::to_string(hi) + ".";
        else
            r.message += ".";
        return r;
    }

    // Nothing to drop brings it back: the whole set is impossible together.
    r.message = "No results for this combination.";
    return r;
}

}  // namespace facets

#include "core/facets.h"

#include <algorithm>
#include <map>

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

// "24-bit" / "16-bit", or "DSD". Empty when the album carries no bit depth.
std::string albumQuality(const Album& a) {
    if (a.hasDsd) return "DSD";
    std::map<int, int> depths;
    for (const Track& t : a.tracks) depths[t.bitDepth]++;
    int d = majority(depths);
    return d > 0 ? std::to_string(d) + "-bit" : std::string();
}

std::string albumGenre(const Album& a) {
    std::map<std::string, int> counts;
    for (const Track& t : a.tracks)
        if (!t.genre.empty()) counts[t.genre]++;
    std::string best;
    int bestN = 0;
    for (const auto& kv : counts)
        if (kv.second > bestN) { best = kv.first; bestN = kv.second; }
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

// ── Matching one chip ───────────────────────────────────────────────────────

bool nameHit(const Album& a, const std::string& needle) {
    if (needle.empty()) return true;
    auto hit = [&](const std::string& hay) {
        std::string h = normalize(hay);
        if (h.find(needle) != std::string::npos) return true;
        // A chip's value came from a suggestion, so it is already a real
        // library value; the distance check only absorbs the listener's own
        // typo when the chip was accepted from partial text.
        return editDistance(h, needle, 2) <= 2;
    };
    if (hit(a.artist) || hit(a.displayName)) return true;
    for (const Track& t : a.tracks)
        if (hit(t.title)) return true;
    return false;
}

bool chipHit(const Album& a, const Chip& c) {
    switch (c.kind) {
        case Kind::Name:    return nameHit(a, normalize(c.value));
        case Kind::Year:
        case Kind::Decade: {
            int y = albumYear(a);
            return y != 0 && y >= c.from && y <= c.to;
        }
        case Kind::Quality: return albumQuality(a) == c.value;
        case Kind::Type:    return typeName(a.releaseType) == c.value;
        case Kind::Genre:   return albumGenre(a) == c.value;
    }
    return false;
}

// Year and Decade are one group: they are alternative ways to say "when".
Kind groupOf(Kind k) { return k == Kind::Year ? Kind::Decade : k; }

int countMatching(const std::vector<Album>& lib, const std::vector<Chip>& chips) {
    int n = 0;
    for (const Album& a : lib)
        if (matches(a, chips)) n++;
    return n;
}

// The chips that are NOT in the same group as `k` — the context a suggestion
// of kind `k` is counted against. Counting a suggestion against its own group
// would make picking one option grey out all its siblings, which is exactly
// backwards: siblings are alternatives (OR), not extra filters.
std::vector<Chip> withoutGroup(const std::vector<Chip>& chips, Kind k) {
    std::vector<Chip> out;
    for (const Chip& c : chips)
        if (groupOf(c.kind) != groupOf(k)) out.push_back(c);
    return out;
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
        int v = std::stoi(typedNorm);
        return typedNorm.size() == 4 && v >= c.from && v <= c.to;  // 1993 → its decade
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

bool matches(const Album& a, const std::vector<Chip>& chips) {
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
            if (groupOf(e.kind) == g && chipHit(a, e)) { any = true; break; }
        if (!any) return false;
    }
    return true;
}

std::vector<Suggestion> suggest(const std::vector<Album>& library,
                                const std::string& typed,
                                const std::vector<Chip>& chips) {
    const std::string typedNorm = normalize(typed);

    // Candidates are built ONLY from values the library actually holds, which
    // is what guarantees a value that exists nowhere is never offered — the
    // listener never meets a dead "(0)" row for something they cannot have.
    std::vector<Chip> cands;
    std::vector<std::string> labels;
    auto add = [&](const Chip& c, const std::string& label) {
        for (size_t i = 0; i < cands.size(); i++)
            if (cands[i] == c) return;
        cands.push_back(c);
        labels.push_back(label);
    };

    for (const Album& a : library) {
        int y = albumYear(a);
        if (y != 0) {
            add(mkChip(Kind::Year, std::to_string(y), y, y), std::to_string(y));
            int d = (y / 10) * 10;
            add(mkChip(Kind::Decade, std::to_string(d) + "s", d, d + 9),
                std::to_string(d) + "s");
        }
        std::string q = albumQuality(a);
        if (!q.empty()) add(mkChip(Kind::Quality, q), q);

        add(mkChip(Kind::Type, typeName(a.releaseType)), typeName(a.releaseType));

        std::string g = albumGenre(a);
        if (!g.empty()) add(mkChip(Kind::Genre, g), g);

        // Names are only offered once the listener is actually typing —
        // with an empty box they would bury the handful of attribute rows
        // under every artist and record in the library.
        if (!typedNorm.empty()) {
            add(mkChip(Kind::Name, a.artist), a.artist);
            add(mkChip(Kind::Name, a.displayName), a.displayName);
        }
    }

    std::vector<Suggestion> out;
    for (size_t i = 0; i < cands.size(); i++) {
        if (alreadyPlaced(chips, cands[i])) continue;
        if (!typedMatches(typedNorm, labels[i], cands[i])) continue;

        std::vector<Chip> ctx = withoutGroup(chips, cands[i].kind);
        ctx.push_back(cands[i]);

        Suggestion s;
        s.chip    = cands[i];
        s.label   = labels[i];
        s.count   = countMatching(library, ctx);
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
    if (chips.empty() || countMatching(library, chips) > 0) return r;
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
        if (countMatching(library, without) == 0) continue;

        r.culpritIndex = (int)i;
        r.message = "No " + subject.value + " in " + chips[i].value;

        // Where the subject DOES live, so the listener has somewhere to go.
        int lo = 0, hi = 0;
        for (const Album& a : library) {
            if (!chipHit(a, subject)) continue;
            int y = albumYear(a);
            if (y == 0) continue;
            if (lo == 0 || y < lo) lo = y;
            if (y > hi) hi = y;
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

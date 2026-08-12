#pragma once
#include <string>
#include <vector>

#include "core/library.h"   // Album, Track (types only — no library.cpp needed)

// ── Guided search ───────────────────────────────────────────────────────────
//
// The search box does NOT parse a sentence. Free text like "more than 10
// minutes" hangs on accents, spacing and remembering the exact phrase — a
// human-error factory. Instead the listener builds a query out of CHIPS:
//
//     [ Björk ]  AND  [ 1990–1999 ]  AND  [ 24-bit ]
//
// and every chip is accepted from a suggestion computed against the REAL
// library, so a value that does not exist can never be typed in.
//
// Two rules give the thing its character, and both are asserted in
// core/tests/facets_test.cc:
//
//   1. A value that exists NOWHERE in the library is never suggested at all —
//      the listener never sees a dead "(0)" row.
//   2. A value that exists but not ALONGSIDE the chips already placed IS
//      suggested, disabled, and can name the chip that killed it. "24-bit (0)"
//      alone cannot tell those two situations apart, which is precisely the
//      confusion this split exists to remove.
//
// PURE: no sqlite, no Canvas, no OS. facets_test links this translation unit
// and nothing else. Keep it that way — Android consumes the same code.
namespace facets {

// What a chip constrains. Chips of the SAME kind combine with OR (two decades
// = either), chips of DIFFERENT kinds with AND. The listener never picks the
// connective; matches() applies it and the UI merely displays it.
enum class Kind {
    Name,      // artist / album / track title — the one fuzzy-matched field
    Year,      // one exact year
    Decade,    // ten years, e.g. 1990–1999
    Quality,   // bit depth, or DSD
    Type,      // Album/EP/Single/Compilation/Live/Remix
    Genre,
};

struct Chip {
    Kind        kind  = Kind::Name;
    std::string value;          // canonical value AND display text
    int         from  = 0;      // Year/Decade: inclusive lower bound
    int         to    = 0;      // Year/Decade: inclusive upper bound

    bool operator==(const Chip& o) const {
        return kind == o.kind && value == o.value && from == o.from && to == o.to;
    }
};

struct Suggestion {
    Chip        chip;           // what gets added when accepted
    std::string label;          // "1990s", "24-bit", "Björk"
    int         count   = 0;    // albums remaining if accepted
    bool        enabled = true; // false = exists, but not with the current chips
};

// Reason a chip set matched nothing, in the app's UI language (English).
struct EmptyReason {
    bool        empty        = false;
    int         culpritIndex = -1;  // index into the chip vector, or -1
    std::string message;            // ready to draw, empty when !empty
};

// ── The release year ────────────────────────────────────────────────────────
// Album carries no year of its own — only Track::year does — so it is derived:
// the most common non-zero year among its tracks. Returns 0 when NO track is
// tagged, which is a real case (nothing reads ID3, so MP3s carry no year) and
// must render as "Unknown", never as 1970.
int albumYear(const Album& a);

// Lowercase, strip diacritics, drop punctuation. This is why "bjork" finds
// "Björk" and why a missing accent stops mattering. Non-Latin scripts pass
// through unchanged — equality still works for them.
std::string normalize(const std::string& s);

// Levenshtein distance, abandoned once it exceeds `max` (returns max + 1).
int editDistance(const std::string& a, const std::string& b, int max);

// Do these two chips belong to the same group — i.e. are they alternatives?
// Year and Decade are one group: both answer "when". The UI reads this to
// LABEL the connective it draws between chips ("OR" for same, "AND" for
// different); the listener never picks one, because picking a boolean
// operator is exactly the kind of complexity this design keeps out of sight.
bool sameGroup(const Chip& a, const Chip& b);

// Does this album satisfy the chips? OR within a kind, AND across kinds.
bool matches(const Album& a, const std::vector<Chip>& chips);

// Suggestions for the text being typed, given the chips already placed.
// `typed` empty = offer the whole menu of what is still reachable.
std::vector<Suggestion> suggest(const std::vector<Album>& library,
                                const std::string& typed,
                                const std::vector<Chip>& chips);

// Why the current chips match nothing, and which chip is to blame.
EmptyReason explainEmpty(const std::vector<Album>& library,
                         const std::vector<Chip>& chips);

}  // namespace facets

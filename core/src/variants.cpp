#include "core/variants.h"

#include <algorithm>
#include <array>
#include <string_view>
#include <unordered_map>

// ── ASCII character classes ─────────────────────────────────────────────────
// Hand-rolled and constexpr rather than <cctype>. Three reasons, in order of
// weight:
//
//  1. <cctype>'s functions read the global C locale through a table pointer, so
//     they are opaque calls the optimizer cannot fold — and this file calls
//     them once per byte of every album and track name in the library.
//  2. The answer here must not depend on a locale. Nothing in this tree calls
//     setlocale(), so the program runs in the "C" locale and these are exactly
//     equivalent today; writing them out means they stay equivalent even if a
//     dependency ever calls setlocale() behind our back.
//  3. wordByte/spaceByte are ECMAScript's \w and \s, which is what the
//     std::regex patterns this file used to carry resolved to for `char` in
//     that same "C" locale. They are spelled out here so the matchers below
//     are demonstrably the same rule the regexes were.
namespace {

constexpr bool asciiUpper(unsigned char c) { return c >= 'A' && c <= 'Z'; }
constexpr bool asciiDigit(unsigned char c) { return c >= '0' && c <= '9'; }
constexpr bool asciiAlpha(unsigned char c) { return (c | 32u) >= 'a' && (c | 32u) <= 'z'; }
constexpr bool asciiAlnum(unsigned char c) { return asciiAlpha(c) || asciiDigit(c); }
constexpr char asciiLower(unsigned char c) { return asciiUpper(c) ? char(c + 32) : char(c); }

constexpr bool wordByte(unsigned char c) { return asciiAlnum(c) || c == '_'; }
constexpr bool spaceByte(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

// A line terminator, which ECMAScript's '.' does NOT match. Only relevant to
// the two bracket-and-mix patterns below, whose '.*' cannot cross one.
constexpr bool lineBreak(unsigned char c) { return c == '\n' || c == '\r'; }

} // namespace

// ── Name splitting ──────────────────────────────────────────────────────────
// Moved here verbatim from gui/src/player_view.cc, where it was file-static:
// grouping and drawing must split names identically, and the logic never
// needed anything from the GUI. Behaviour is unchanged.

// Strips ONE trailing bracketed group off `s`, honoring nesting: for
// "Tears (feat. Sleepnet & Joker)" or "Mixed Signals [VIP (Extended)]" it
// returns the group's opener index and its inner text. Both () and [] count
// — Skrillex-style feat. credits use square brackets. Returns npos if the
// string doesn't end in a well-formed group preceded by a space.
static size_t trailingGroup(const std::string& s, std::string& inner) {
    if (s.size() < 4) return std::string::npos;
    const char close = s.back();
    if (close != ')' && close != ']') return std::string::npos;
    const char open = (close == ')') ? '(' : '[';
    int depth = 0;
    for (size_t i = s.size(); i-- > 0; ) {
        if (s[i] == close) depth++;
        else if (s[i] == open) {
            depth--;
            if (depth == 0) {
                // Must be a *suffix* group, not the whole string
                // ("(What's The Story) Morning Glory?" keeps its parens).
                if (i == 0) return std::string::npos;
                if (s[i - 1] != ' ') {
                    // No space before the bracket: only accept groups that
                    // are unmistakably credits ("Name(feat. X)" happens in
                    // sloppy tags), never bare ones — "R(A)W" stays whole.
                    // Tested on a view of `s`, so the rejected case costs no
                    // allocation at all.
                    const std::string_view body(s.data() + i + 1, s.size() - i - 2);
                    const std::string_view low = body.substr(0, std::min<size_t>(5, body.size()));
                    const auto startsWith = [low](std::string_view lit) {
                        if (low.size() < lit.size()) return false;
                        for (size_t k = 0; k < lit.size(); k++)
                            if (asciiLower((unsigned char)low[k]) != lit[k]) return false;
                        return true;
                    };
                    if (!(startsWith("feat") || startsWith("ft.") ||
                          startsWith("ft ")  || startsWith("with ")))
                        return std::string::npos;
                }
                inner.assign(s, i + 1, s.size() - i - 2);
                return i;
            }
        }
    }
    return std::string::npos;
}

// A trailing dash-separated modifier: "Avalancha- Edición Especial",
// "Senderos De Traición - Edición Especial", "Song – Remastered". Returns the
// dash's offset and fills `tail`, or npos.
//
// The guard is whitespace ADJACENCY, not symmetry. "Avalancha- Edición
// Especial" has no space before its dash and still has to split, while
// "Jay-Z", "Blink-182" and "Post-Traumatic" must not: a dash with whitespace
// on neither side is inside a word, one with whitespace on either side is
// punctuation. Takes the LAST such dash, so "A - B - C" keeps "A - B" as the
// name. En/em dashes (U+2013/U+2014) count too — they are three UTF-8 bytes,
// hence the explicit byte test rather than a char compare.
//
// Deliberately blind to what the tail SAYS. Reading its meaning is
// classifyModifier()'s job, below, and only the grouping asks — the drawing
// code still treats every modifier the same way.
static size_t trailingDashModifier(const std::string& s, std::string& tail) {
    size_t best = std::string::npos, bestLen = 0;
    for (size_t i = 0; i < s.size(); ) {
        size_t len = 0;
        if (s[i] == '-') {
            len = 1;
        } else if (i + 2 < s.size() && (unsigned char)s[i] == 0xE2 &&
                   (unsigned char)s[i + 1] == 0x80 &&
                   ((unsigned char)s[i + 2] == 0x93 || (unsigned char)s[i + 2] == 0x94)) {
            len = 3;
        }
        if (!len) { i++; continue; }
        const bool spaceBefore = i > 0 && s[i - 1] == ' ';
        const bool spaceAfter  = i + len < s.size() && s[i + len] == ' ';
        if (spaceBefore || spaceAfter) { best = i; bestLen = len; }
        i += len;
    }
    if (best == std::string::npos) return std::string::npos;

    size_t t = best + bestLen;
    while (t < s.size() && s[t] == ' ') t++;
    if (t >= s.size()) return std::string::npos;          // trailing dash, no tail
    // Head length measured in place — the original built a substring only to
    // ask how long it was after trimming.
    size_t headLen = best;
    while (headLen > 0 && s[headLen - 1] == ' ') headLen--;
    if (headLen < 2) return std::string::npos;            // "- Title" IS the title

    // The tail must START CAPITALISED. Edition tags are, by convention, in
    // every script that has cases: "Edición Especial", "Remastered 2011",
    // "Deluxe". Titles that merely contain a dash usually are not — this is
    // what keeps "v.i.p. - very important pony" whole, which the whitespace
    // rule alone would have chopped in half. A non-ASCII first byte is taken
    // on trust: telling case there needs Unicode tables this app has no reason
    // to carry, and an accented edition tag ("Édition Collector") is
    // capitalised anyway.
    const unsigned char f = (unsigned char)s[t];
    if (f < 0x80 && !(asciiUpper(f) || asciiDigit(f))) return std::string::npos;

    tail.assign(s, t, s.size() - t);
    return best;
}

// '·' (U+00B7), the separator stacked modifiers are joined with.
static constexpr std::string_view kModSep = " \xC2\xB7 ";

bool splitNameModifier(const std::string& s, std::string& base, std::string& mod) {
    base = s;
    mod.clear();
    // Metadata strings often carry invisible trailing whitespace
    // ("... (feat. X) " / a stray \r) — without this trim the last char
    // isn't ')' and the group detection misses entirely, so the whole
    // title renders as the base name (the "Tears (feat. Sleepnet &
    // Joker) rendered all-white" bug).
    while (!base.empty() && (base.back() == ' ' || base.back() == '\t' ||
                             base.back() == '\r' || base.back() == '\n'))
        base.pop_back();
    std::string inner;
    for (int pass = 0; pass < 3; pass++) {
        const size_t at = trailingGroup(base, inner);
        if (at == std::string::npos || inner.empty()) break;
        size_t restLen = at;
        while (restLen > 0 && base[restLen - 1] == ' ') restLen--;
        if (restLen == 0) break;
        base.resize(restLen);
        if (mod.empty()) mod.swap(inner);
        else             mod.insert(0, inner + std::string(kModSep));
    }
    // Then at most one dash-separated tail, after the brackets are off — so
    // "Album - Deluxe (feat. X)" reads base "Album", mod "Deluxe · feat. X".
    {
        std::string tail;
        const size_t at = trailingDashModifier(base, tail);
        if (at != std::string::npos) {
            base.resize(at);
            while (!base.empty() && base.back() == ' ') base.pop_back();
            if (mod.empty()) mod.swap(tail);
            else             mod.insert(0, tail + std::string(kModSep));
        }
    }
    // No group found: keep the (whitespace-trimmed) name as the base.
    return !mod.empty();
}

// ── Pattern matching ────────────────────────────────────────────────────────
// What used to be six std::regex objects. They were replaced because
// <regex> is by far the most expensive thing this translation unit contained:
// it compiled to over a megabyte of object code for a 650-line file, it
// interprets a parsed NFA at run time (allocating as it backtracks), and its
// headers dominated this file's compile time. The patterns it was asked to run
// are all one shape — a literal, or a two-word phrase, anchored on word
// boundaries — so they are spelled out directly below.
//
// The rules replicated are ECMAScript's, as libstdc++ resolves them for `char`
// in the "C" locale: \w is [A-Za-z0-9_], \s is the six ASCII spaces, \b is a
// \w/non-\w transition, and '.' matches anything but a line terminator. Bytes
// >= 0x80 are therefore NON-word bytes, which is what makes a word boundary
// fall between an accented letter's UTF-8 bytes and a following ASCII letter —
// preserved here deliberately, because the old behaviour depended on it.
namespace {

// A phrase is one or two lowercase tokens; where the pattern wrote `\s+`
// between them, the text must carry at least one space byte. Two tokens covers
// every alternative these patterns use ("en directo", "dal vivo", "live at").
struct Phrase {
    std::string_view a;
    std::string_view b;   // empty = single-token phrase
};

// Case-insensitive compare of `lit` (already lowercase) against s at `pos`.
constexpr bool literalAt(std::string_view s, size_t pos, std::string_view lit) {
    if (pos + lit.size() > s.size()) return false;
    for (size_t k = 0; k < lit.size(); k++)
        if (asciiLower((unsigned char)s[pos + k]) != lit[k]) return false;
    return true;
}

// ECMAScript \b at offset i.
constexpr bool boundaryAt(std::string_view s, size_t i) {
    const bool before = i > 0        && wordByte((unsigned char)s[i - 1]);
    const bool after  = i < s.size() && wordByte((unsigned char)s[i]);
    return before != after;
}

// Offset just past the phrase if it matches at `pos`, else npos.
constexpr size_t phraseAt(std::string_view s, size_t pos, const Phrase& p) {
    if (!literalAt(s, pos, p.a)) return std::string_view::npos;
    size_t at = pos + p.a.size();
    if (p.b.empty()) return at;
    const size_t sp = at;
    while (at < s.size() && spaceByte((unsigned char)s[at])) at++;
    if (at == sp) return std::string_view::npos;          // \s+ needs one
    if (!literalAt(s, at, p.b)) return std::string_view::npos;
    return at + p.b.size();
}

// `\b(p1|p2|...)\b` anywhere in [from, to].
bool anyPhraseWord(std::string_view s, const Phrase* phrases, size_t n,
                   size_t from = 0, size_t to = std::string_view::npos) {
    if (to > s.size()) to = s.size();
    for (size_t i = from; i <= to; i++) {
        if (!boundaryAt(s, i)) continue;
        for (size_t k = 0; k < n; k++) {
            const size_t end = phraseAt(s, i, phrases[k]);
            if (end != std::string_view::npos && boundaryAt(s, end)) return true;
        }
    }
    return false;
}

// Case-insensitive substring search (the old `lower.find(lit)`).
bool icaseContains(std::string_view s, std::string_view lit) {
    if (lit.size() > s.size()) return false;
    for (size_t i = 0; i + lit.size() <= s.size(); i++)
        if (literalAt(s, i, lit)) return true;
    return false;
}

// Case-insensitive whole-string equality.
bool icaseEquals(std::string_view s, std::string_view lit) {
    return s.size() == lit.size() && literalAt(s, 0, lit);
}

// `\b\w+\s+mix\b` — a word, whitespace, then "mix" ending on a boundary. The
// leading \b needs no test: taking \w+ as the maximal run before the spaces
// puts its start at a non-word byte or at the string start either way.
bool matchWordSpaceMix(std::string_view s) {
    for (size_t p = 0; p + 3 <= s.size(); p++) {
        if (!literalAt(s, p, "mix")) continue;
        if (p + 3 < s.size() && wordByte((unsigned char)s[p + 3])) continue;  // trailing \b
        size_t q = p;
        while (q > 0 && spaceByte((unsigned char)s[q - 1])) q--;
        if (q == p) continue;                                    // \s+ needs one
        if (q > 0 && wordByte((unsigned char)s[q - 1])) return true;   // \w+ before it
    }
    return false;
}

// `\(.*mix.*\)` / `\[.*mix.*\]`. Because '.' cannot cross a line terminator,
// the whole match lives inside one line — so each line is scanned on its own.
// Within a line only the FIRST opener and the FIRST "mix" after it need
// testing: a later opener admits no closer an earlier one does not, and if no
// closer follows the first "mix" then none follows a later one either.
bool bracketedMix(std::string_view s, char open, char close) {
    size_t lineStart = 0;
    while (lineStart <= s.size()) {
        size_t lineEnd = lineStart;
        while (lineEnd < s.size() && !lineBreak((unsigned char)s[lineEnd])) lineEnd++;

        const std::string_view line = s.substr(lineStart, lineEnd - lineStart);
        const size_t o = line.find(open);
        if (o != std::string_view::npos) {
            for (size_t p = o + 1; p + 3 <= line.size(); p++) {
                if (!literalAt(line, p, "mix")) continue;
                if (line.find(close, p + 3) != std::string_view::npos) return true;
                break;
            }
        }
        if (lineEnd >= s.size()) break;
        lineStart = lineEnd + 1;
    }
    return false;
}

} // namespace

// ── Release-type classification ─────────────────────────────────────────────
// Ported verbatim from the sibling Android player's AlbumDao.java
// (isRemixTrack/isRemixAlbum/classifyRelease) — see the design spec at
// docs/superpowers/specs/2026-07-27-release-type-and-quality-color-design.md.
// Moved here from library.cpp so the test can link it: it is pure string and
// Track logic, and it belongs beside the grouping that reads its answer.

bool isRemixTrackTitle(const std::string& title) {
    if (title.empty()) return false;
    std::string_view t(title);
    const size_t b = t.find_first_not_of(" \t");
    const std::string_view trimmed =
        (b == std::string_view::npos) ? std::string_view{}
                                      : t.substr(b, t.find_last_not_of(" \t") - b + 1);
    if (icaseEquals(trimmed, "remix") || icaseEquals(trimmed, "mix") ||
        icaseEquals(trimmed, "the remix") || icaseEquals(trimmed, "the mix")) return false;
    if (icaseContains(t, "remix")) return true;
    if (icaseContains(t, "rmx"))   return true;
    if (matchWordSpaceMix(t))      return true;
    if (bracketedMix(t, '(', ')')) return true;
    if (bracketedMix(t, '[', ']')) return true;
    return false;
}

bool isLiveTrackTitle(const std::string& title) {
    if (title.empty()) return false;
    // ONLY inside a bracket, unlike the remix rule, which also accepts a bare
    // "remix" anywhere in the title. "Live" is an ordinary English word and a
    // studio track can be called "Live and Let Die" or "Live Forever"; the
    // parenthetical is what makes it a marker rather than a lyric. The keyword
    // may sit anywhere inside the bracket ("(Medley, Live)"), and the closing
    // bracket is not required.
    //
    // A title whose tag was truncated mid-marker still misses — this library
    // holds one, "...(Víspera De Resplandores) (Direct" — and that is left
    // alone deliberately. Matching a bare "direct" would catch "(Direct to
    // Disc)", which is a STUDIO mastering credit, on a library made of
    // audiophile downloads. The per-disc majority absorbs the odd miss; a
    // wrong keyword would not.
    //
    // Was: [(\[][^)\]]*\b(live|directo|...)\b
    static constexpr Phrase kLiveWords[] = {
        {"live", ""},   {"directo", ""},     {"en", "directo"}, {"en", "vivo"},
        {"ao", "vivo"}, {"dal", "vivo"},     {"unplugged", ""}, {"en", "concert"},
    };
    const std::string_view s(title);
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '(' && s[i] != '[') continue;
        // [^)\]]* stops at the first closing bracket of either kind. The
        // keywords contain none, so a match starting inside the span also ends
        // inside it.
        size_t end = i + 1;
        while (end < s.size() && s[end] != ')' && s[end] != ']') end++;
        if (anyPhraseWord(s, kLiveWords, std::size(kLiveWords), i + 1, end)) return true;
    }
    return false;
}

namespace {

using TitlePredicate = bool (*)(const std::string&);

// "Most of this is X" over one flat list of titles — the Android reference's
// rule, unchanged: all of them, or a strict majority of at least two.
constexpr bool majority(int hits, int total) {
    if (total == 0) return false;
    return hits == total || (hits >= 2 && hits * 2 > total);
}

// The per-disc majority, shared by remix and live detection because both ask
// the identical question of the track listing and both have the identical
// bonus-disc failure mode.
bool everyDiscIsMajority(const std::vector<Track>& tracks, TitlePredicate pred) {
    if (tracks.empty()) return false;

    // Group by tagged disc. discNumber is 0 for every track of a single-disc
    // release (see core/library.h), so a library with no DISCNUMBER tags lands
    // in a single bucket and behaves exactly as before.
    //
    // Only the two COUNTS per disc are needed, never the tracks themselves, so
    // this is a flat tally rather than the map-of-vectors it used to build —
    // a release has a handful of discs, and a linear probe over that beats a
    // node allocation per disc plus one per track.
    struct Disc { int id; int total; int hits; };
    std::array<Disc, 8> inlineDiscs{};
    std::vector<Disc>   overflow;
    size_t discCount = 0;

    const auto find = [&](int id) -> Disc* {
        for (size_t k = 0; k < discCount && k < inlineDiscs.size(); k++)
            if (inlineDiscs[k].id == id) return &inlineDiscs[k];
        for (Disc& d : overflow)
            if (d.id == id) return &d;
        return nullptr;
    };

    for (const Track& t : tracks) {
        Disc* d = find(t.discNumber);
        if (!d) {
            if (discCount < inlineDiscs.size()) d = &inlineDiscs[discCount];
            else { overflow.push_back({t.discNumber, 0, 0}); d = &overflow.back(); }
            d->id = t.discNumber; d->total = 0; d->hits = 0;
            discCount++;
        }
        d->total++;
        if (pred(t.title)) d->hits++;
    }

    if (discCount < 2) return majority(inlineDiscs[0].hits, inlineDiscs[0].total);

    // MULTI-DISC: every disc must qualify. One disc of remixes bolted onto a
    // disc of new material is a bonus disc, and the release is still an album —
    // but counted flat, those remixes can outnumber the originals and drag the
    // whole thing into the Remixes tab. Anyma's *Genesys II* is the case that
    // exposed this: 10 original tracks on disc 1, 11 remixes on disc 2, so 11
    // of 21 titles matched and a sequel album was filed as a remix set. Per
    // disc, disc 1 answers no, and the album stays an album.
    //
    // The live rule inherits the same protection, and needs it just as badly:
    // this library's Héroes del Silencio "Edición Especial" reissues each pair
    // a studio disc with a live bonus disc (0 of 12 live, then 12 of 13). Flat,
    // Senderos reads 48% live and El Mar No Cesa 32%; per disc, both stay
    // albums, and only *Parasiempre* — 19 of 19 on one disc — is a live record.
    for (size_t k = 0; k < discCount && k < inlineDiscs.size(); k++)
        if (!majority(inlineDiscs[k].hits, inlineDiscs[k].total)) return false;
    for (const Disc& d : overflow)
        if (!majority(d.hits, d.total)) return false;
    return true;
}

bool isRemixAlbum(const std::string& albumName, const std::vector<Track>& tracks) {
    // Was: \b(remix|remixes|remixed|rmx)\b
    static constexpr Phrase kRemixName[] = {
        {"remix", ""}, {"remixes", ""}, {"remixed", ""}, {"rmx", ""},
    };
    if (!albumName.empty() &&
        anyPhraseWord(albumName, kRemixName, std::size(kRemixName))) return true;
    return everyDiscIsMajority(tracks, isRemixTrackTitle);
}

bool isLiveAlbum(const std::string& albumName, const std::vector<Track>& tracks) {
    // Album names that say it outright. "concert"/"concierto" are absent on
    // purpose — "Concierto para..." is a composition title, not a venue.
    //
    // Was: \b(live|unplugged|en\s+directo|en\s+vivo|ao\s+vivo|dal\s+vivo|
    //          en\s+concert|live\s+at|live\s+in)\b
    // "live at"/"live in" are kept for fidelity with that pattern although a
    // bare "live" already decides them.
    static constexpr Phrase kLiveName[] = {
        {"live", ""},      {"unplugged", ""}, {"en", "directo"}, {"en", "vivo"},
        {"ao", "vivo"},    {"dal", "vivo"},   {"en", "concert"}, {"live", "at"},
        {"live", "in"},
    };
    if (!albumName.empty() &&
        anyPhraseWord(albumName, kLiveName, std::size(kLiveName))) return true;
    return everyDiscIsMajority(tracks, isLiveTrackTitle);
}

} // namespace

Album::ReleaseType classifyReleaseType(const std::string& albumName,
                                       const std::vector<Track>& tracks) {
    // Remix first, so every existing answer is unchanged: a remix set is
    // genuinely different music, and "Greatest Hits Remixed" is that before it
    // is a compilation — the same precedence classifyModifier() already applies
    // between its two tables.
    if (isRemixAlbum(albumName, tracks)) return Album::ReleaseType::Remix;
    // Live before Compilation, by the same reasoning: a live "Grandes Éxitos"
    // is a concert recording first — that is what changes what you hear — and a
    // collection of it second.
    if (isLiveAlbum(albumName, tracks))  return Album::ReleaseType::Live;
    // Before the track counts, not after: a two-track "Grandes Éxitos" is still
    // a compilation, and an anthology's length says nothing about what it is.
    if (isCompilationAlbum(albumName))   return Album::ReleaseType::Compilation;
    const int trackCount = (int)tracks.size();
    if (trackCount == 1) return Album::ReleaseType::Single;
    if (trackCount <= 4) return Album::ReleaseType::Ep;
    return Album::ReleaseType::Album;
}

// ── Folding ─────────────────────────────────────────────────────────────────

// Lowercases ASCII and folds the Latin-1 accented letters onto their bare
// forms, so "Edición" and "Edicion" — and "HÉROES" and "héroes" — reduce to
// one spelling. The library is tagged by many hands and by several
// downloaders; whether an accent survived is not information about the music.
//
// Only the two-byte UTF-8 range (U+00C0–U+00FF) is folded. That covers every
// Latin script this library's edition vocabulary is written in; anything
// outside it (Cyrillic, CJK) passes through byte-for-byte, which still
// compares correctly against itself.
static constexpr std::string_view kFolded =
    // U+00C0..U+00FF, one ASCII letter each ('?' where there is no
    // sensible single-letter fold — those compare against themselves).
    "aaaaaaaceeeeiiiidnooooo?ouuuuy??"   // C0-DF
    "aaaaaaaceeeeiiiidnooooo?ouuuuy?y";  // E0-FF

static void foldInto(std::string_view s, std::string& out) {
    out.reserve(out.size() + s.size());
    for (size_t i = 0; i < s.size(); ) {
        const unsigned char c = (unsigned char)s[i];
        if (c < 0x80) {
            out += asciiLower(c);
            i++;
        } else if ((c == 0xC3) && i + 1 < s.size()) {
            const unsigned char lo = (unsigned char)s[i + 1];
            // C3 80..BF is U+00C0..U+00FF.
            if (lo >= 0x80 && lo <= 0xBF) {
                const char f = kFolded[lo - 0x80];
                if (f == '?') { out += (char)c; out += (char)lo; }
                else            out += f;
            } else {
                out += (char)c; out += (char)lo;
            }
            i += 2;
        } else {
            out += (char)c;
            i++;
        }
    }
}

static std::string fold(std::string_view s) {
    std::string out;
    foldInto(s, out);
    return out;
}

// ── Modifier classification ─────────────────────────────────────────────────

// The "Borderless" vocabulary: terms that mark another PRESSING of a release
// rather than a different release. Stored folded (lowercase, unaccented), so
// the table is compared against fold()'s output directly.
//
// Adding a language means adding rows here and nothing else — that is the
// whole point of keeping this a table rather than a chain of conditionals.
// "disc" is deliberately absent: "Album (Disc 2)" is one half of a release,
// not another pressing of it, and folding the two together would hide music.
//
// string_view, not const char*: the lookup below compares against a
// std::string_view word, so a char* table would call strlen on every row of
// every probe. The lengths are known at compile time here.
static constexpr std::string_view kEditionTerms[] = {
    // English
    "deluxe", "special", "expanded", "extended", "remaster", "remastered",
    "anniversary", "edition", "collector", "collectors", "bonus", "reissue",
    "complete", "definitive", "legacy", "gold", "platinum", "ultimate",
    "super", "version",
    // Spanish
    "especial", "edicion", "reedicion", "aniversario", "remasterizado",
    "remasterizada", "coleccionista", "lujo",
    // French / Italian / Portuguese / German — same job, other spellings
    "edicao", "edizione", "speciale", "sammleredition", "jubilaumsedition",
};

// Terms marking a REWORK of the release rather than another pressing of it.
// Kept separate from the edition table because the two mean different things
// downstream: an edition adds or remasters tracks, so one member's cover can
// stand for the group; a remix set is genuinely different music, which is why
// the grid draws remix groups as a mosaic of all their covers instead.
//
// "mix" alone is absent on purpose — "Final Mix", "Original Mix" and any
// number of ordinary album names contain it.
static constexpr std::string_view kRemixTerms[] = {
    "remix", "remixes", "remixed", "remixe", "rmx",
    "bootleg", "rework", "reworked", "flip", "vip", "edits",
    "remezcla", "remezclas",
};

// ── Compilation classification ──────────────────────────────────────────────

// Names that mark a release as a COLLECTION of previously issued material by
// one artist — an anthology, a greatest-hits, a rarities set. Same conventions
// as the two tables above: written folded (lowercase, unaccented), and adding a
// language means adding rows and nothing else.
//
// Kept short and SPECIFIC on purpose. Several of these words also live in
// kEditionTerms ("complete", "gold", "definitive", "collector"), where they
// mean another pressing of one record rather than a collection across many.
// The two tables never see the same input — kEditionTerms is matched against
// the parenthetical MODIFIER by classifyModifier(), this one against the whole
// name — but a bare "gold" or "complete" here would still swallow "Gold
// (Deluxe)" and "The Complete Sessions". So the ambiguous words appear only
// inside a longer phrase that is unambiguous ("disco de oro"), never alone.
static constexpr std::string_view kCompilationTerms[] = {
    // English
    "anthology", "greatest hits", "best of", "the singles", "singles collection",
    "rarities", "b sides", "retrospective", "the essential", "the collection",
    "collected",
    // Spanish
    "antologia", "grandes exitos", "exitos", "lo mejor de", "rarezas",
    "coleccion", "disco de oro", "lo esencial",
    // Portuguese / French / Italian / German — same job, other spellings
    "o melhor de", "os maiores sucessos", "anthologie", "le meilleur de",
    "l essentiel", "il meglio di", "i grandi successi", "das beste von",
};

// Folds, then reduces every run of non-alphanumeric bytes to ONE space and pads
// both ends with a space. That makes a plain substring search a word-boundary
// search: " best of " cannot match inside "The Best Offer", and a term written
// with any punctuation ("b-sides") normalizes to the same shape as the name it
// has to match. Bytes >= 0x80 count as word characters, exactly as
// classifyModifier() treats them, so accented text folded by fold() stays whole.
static std::string normalizeWords(std::string_view s) {
    const std::string f = fold(s);
    std::string out = " ";
    out.reserve(f.size() + 2);
    bool pendingSpace = false;
    for (size_t i = 0; i < f.size(); i++) {
        const unsigned char c = (unsigned char)f[i];
        const bool wordChar = (c >= 0x80) || asciiAlnum(c);
        if (wordChar) {
            if (pendingSpace) { out += ' '; pendingSpace = false; }
            out += (char)c;
        } else if (out.size() > 1) {
            pendingSpace = true;
        }
    }
    out += ' ';
    return out;
}

static bool isWordIn(std::string_view word, const std::string_view* table, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (word == table[i]) return true;
    return false;
}

// The public entry point takes a std::string; the work is done on a view so the
// internal callers (variantKey/stripEditionMods, which hold segments of a
// larger string) need not materialise one per segment.
static ModifierKind classifyModifierView(std::string_view mod) {
    const std::string f = fold(mod);
    // Trim, then reject an all-whitespace modifier the same as an empty one.
    const size_t b = f.find_first_not_of(" \t");
    if (b == std::string::npos) return ModifierKind::None;

    // Word-boundary matching, not substring: "Superstition" must not match
    // "super", and "Reversion" must not match "version". Any non-alphanumeric
    // byte separates words, which also splits "20th Anniversary Edition" and
    // "Remastered 2011" the way you'd expect.
    //
    // Remix wins over Edition when a modifier somehow says both ("Deluxe
    // Remixes"): the reworked music is the more surprising fact, and it is the
    // one that changes how the group is drawn.
    //
    // The words are VIEWS into `f` rather than a string built up a byte at a
    // time — every byte kept here is part of a contiguous run, so there is
    // nothing to copy.
    bool edition = false, remix = false;
    const std::string_view fv(f);
    size_t wordStart = std::string_view::npos;
    const auto flush = [&](size_t endPos) {
        if (wordStart == std::string_view::npos) return;
        const std::string_view word = fv.substr(wordStart, endPos - wordStart);
        if (isWordIn(word, kEditionTerms, std::size(kEditionTerms))) edition = true;
        if (isWordIn(word, kRemixTerms,   std::size(kRemixTerms)))   remix   = true;
        wordStart = std::string_view::npos;
    };
    for (size_t i = b; i < fv.size(); i++) {
        const unsigned char c = (unsigned char)fv[i];
        // Non-ASCII bytes stay in-word, same as before.
        if (c >= 0x80 || asciiAlnum(c)) {
            if (wordStart == std::string_view::npos) wordStart = i;
        } else {
            flush(i);
        }
    }
    flush(fv.size());
    if (remix)   return ModifierKind::Remix;
    if (edition) return ModifierKind::Edition;
    return ModifierKind::Other;
}

ModifierKind classifyModifier(const std::string& mod) {
    return classifyModifierView(mod);
}

bool isCompilationAlbum(const std::string& albumName) {
    if (albumName.empty()) return false;
    const std::string name = normalizeWords(albumName);
    if (name.size() <= 2) return false;   // nothing but the padding

    // The terms go through the SAME normalizer rather than being trusted to be
    // written in normalized form: that way a row added later as "B-Sides" or
    // "Éxitos" still matches instead of silently never firing. They are
    // constants, so that normalization is done ONCE for the process rather than
    // rebuilding all ~30 strings on every album the scan classifies.
    static const std::vector<std::string> kNormalized = [] {
        std::vector<std::string> v;
        v.reserve(std::size(kCompilationTerms));
        for (const std::string_view term : kCompilationTerms) {
            std::string t = normalizeWords(term);
            if (t.size() > 2) v.push_back(std::move(t));
        }
        return v;
    }();

    for (const std::string& t : kNormalized)
        if (name.find(t) != std::string::npos) return true;
    return false;
}

// ── Grouping ────────────────────────────────────────────────────────────────

// Appends every segment of `mod` (the " · "-joined modifier list) that
// `keep` accepts, folded and separated by \x1e. Shared by variantKey and
// appendEditionStripped, which differ in which kinds they keep and in whether
// a separator precedes the first kept segment:
//
//   variantKey       builds a fresh string, so \x1e only goes BETWEEN segments
//                    (separateFirst = false — matches the old `if (!kept.empty())`).
//   trackKey's half  appends after the folded base, so every segment including
//                    the first is preceded by one (separateFirst = true).
template <typename KeepFn>
static void appendKeptSegments(const std::string& mod, std::string& out,
                               bool separateFirst, KeepFn keep) {
    const std::string_view mv(mod);
    size_t pos = 0;
    while (pos <= mv.size()) {
        const size_t at = mv.find(kModSep, pos);
        const std::string_view seg =
            mv.substr(pos, at == std::string_view::npos ? std::string_view::npos : at - pos);
        if (keep(classifyModifierView(seg))) {
            if (separateFirst || !out.empty()) out += '\x1e';
            foldInto(seg, out);
        }
        if (at == std::string_view::npos) break;
        pos = at + kModSep.size();
    }
}

std::string variantKey(const Album& a) {
    std::string base, mod;
    splitNameModifier(a.displayName, base, mod);

    // Drop the Edition and Remix segments, keep everything else.
    // "Album (Deluxe)" collapses onto "Album"; "Obsessed (Remixes)" and
    // "Obsessed (X & Y VIP Remix)" collapse onto each other. But
    // "Album (feat. X)" stays apart, and "Album (Deluxe) [feat. X]" lands with
    // the feat. version — the release it is actually another pressing of.
    std::string kept;
    appendKeptSegments(mod, kept, /*separateFirst=*/false,
                       [](ModifierKind k) { return k == ModifierKind::Other; });

    // \x1f between the fields so an artist ending in the next field's opening
    // text can't forge a match ("A" + "BC" vs "AB" + "C"). The release type
    // leads, so a group can never straddle two of the grid's tabs.
    std::string out;
    out.reserve(a.artist.size() + base.size() + kept.size() + 8);
    out += (char)('0' + (int)a.releaseType);
    out += '\x1f';
    foldInto(a.artist, out);
    out += '\x1f';
    foldInto(base, out);
    out += '\x1f';
    out += kept;
    return out;
}

// ── Track identity ──────────────────────────────────────────────────────────

// Folds `s` and drops only its EDITION modifier segments, appending the result
// to `out`. Both halves of trackKey() run through this, so "Song (Remastered
// 2011)" and "Song" reduce alike while "Song (X VIP Remix)" keeps the segment
// that makes it different music.
static void appendEditionStripped(const std::string& s, std::string& out) {
    std::string base, mod;
    splitNameModifier(s, base, mod);   // on no split: base = trimmed s, mod = ""
    foldInto(base, out);
    appendKeptSegments(mod, out, /*separateFirst=*/true, [](ModifierKind k) {
        return k != ModifierKind::Edition && k != ModifierKind::None;
    });
}

// Filename without directory or extension. Only ever reached for a track whose
// TITLE tag is empty, where it is the last stable thing left to key on.
static std::string_view fileStem(std::string_view path) {
    const size_t slash = path.find_last_of("/\\");
    std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const size_t dot = name.find_last_of('.');
    if (dot != std::string_view::npos && dot > 0) name = name.substr(0, dot);
    return name;
}

// Decimal append, replacing a snprintf("%d") that dragged the whole formatted
// -output machinery in for two small integers.
static void appendInt(std::string& out, int v) {
    char buf[12];
    size_t n = 0;
    const bool neg = v < 0;
    unsigned u = neg ? (unsigned)-(long long)v : (unsigned)v;
    do { buf[n++] = char('0' + (u % 10)); u /= 10; } while (u);
    if (neg) buf[n++] = '-';
    while (n--) out += buf[n];
}

std::string trackKey(const Track& t) {
    // ALBUMARTIST first: on a compilation every track carries a different
    // ARTIST, and keying on that would file the same track under a new
    // identity the day a tagger fills the field in differently.
    const std::string& artist = t.albumArtist.empty() ? t.artist : t.albumArtist;
    const std::string  title  = t.title.empty() ? std::string(fileStem(t.filePath)) : t.title;

    // \x1f between the fields, same reason as variantKey(): so text spilling
    // from one field into the next can't forge a match.
    std::string out;
    out.reserve(artist.size() + t.album.size() + title.size() + 16);
    foldInto(artist, out);
    out += '\x1f';
    appendEditionStripped(t.album, out);
    out += '\x1f';
    appendEditionStripped(title, out);
    out += '\x1f';
    appendInt(out, t.discNumber);
    out += '\x1f';
    appendInt(out, t.trackNumber);
    return out;
}

// The extension of `path`, without the dot ("" if none) — a view, so the
// comparisons below cost no allocation. Deliberately byte-wise and ASCII-only:
// extensions are ASCII by definition, and this must not depend on the C locale.
// A dot inside a directory name does not count.
static std::string_view extOf(std::string_view path) {
    const size_t dot = path.find_last_of('.');
    if (dot == std::string_view::npos) return {};
    const size_t slash = path.find_last_of("/\\");
    if (slash != std::string_view::npos && dot < slash) return {};   // dot in a dir name
    return path.substr(dot + 1);
}

std::string variantFormatLabel(const Album& a) {
    if (a.hasDsd) return "DSD";
    // One label per album, so a mixed folder answers with the first thing it
    // finds worth saying. Mixed-format albums are pathological; silently
    // labelling them by their first flagged track beats inventing a rule
    // nobody asked for.
    for (const Track& t : a.tracks) {
        const std::string_view ext = extOf(t.filePath);
        if (icaseEquals(ext, "dsf") || icaseEquals(ext, "dff")) return "DSD";
        if (icaseEquals(ext, "mp3"))                            return "MP3";
    }
    return {};
}

// Highest bit depth across the album's tracks. Album carries no averaged
// depth (unlike avgSampleRate) — the album view computes the same max inline
// for its quality badge.
static int maxBitDepth(const Album& a) {
    int m = 0;
    for (const Track& t : a.tracks) m = std::max(m, t.bitDepth);
    return m;
}

bool variantOutranks(const Album& a, const Album& b) {
    if (a.hasDsd != b.hasDsd)               return a.hasDsd;
    if (a.avgSampleRate != b.avgSampleRate) return a.avgSampleRate > b.avgSampleRate;
    const int da = maxBitDepth(a), db = maxBitDepth(b);
    if (da != db)                           return da > db;
    if (a.tracks.size() != b.tracks.size()) return a.tracks.size() > b.tracks.size();
    // Last resort, so the order never depends on how the scan happened to
    // enumerate the folders. Albums identical in every input above compare
    // equal here too when their folder names match, which keeps this a
    // strict weak ordering.
    return a.name < b.name;
}

std::vector<AlbumGroup> groupAlbumVariants(const std::vector<Album>& albums) {
    // Keyed map, in first-appearance order via the index it stores. Unordered:
    // this is only ever probed by key, never iterated, so the red-black tree's
    // ordering was paid for and never used.
    std::unordered_map<std::string, size_t> byKey;
    byKey.reserve(albums.size());
    std::vector<AlbumGroup> groups;
    groups.reserve(albums.size());

    for (int i = 0; i < (int)albums.size(); i++) {
        const auto [it, inserted] = byKey.emplace(variantKey(albums[i]), groups.size());
        if (inserted) {
            AlbumGroup g;
            g.primary = i;
            g.members.push_back(i);
            groups.push_back(std::move(g));
        } else {
            groups[it->second].members.push_back(i);
        }
    }

    for (AlbumGroup& g : groups) {
        std::stable_sort(g.members.begin(), g.members.end(),
                         [&albums](int x, int y) {
                             return variantOutranks(albums[x], albums[y]);
                         });
        g.primary = g.members[0];
    }

    // Order groups by the tile that represents them, so adding a variant to
    // an existing album never moves other albums around the grid.
    std::stable_sort(groups.begin(), groups.end(),
                     [](const AlbumGroup& x, const AlbumGroup& y) {
                         return x.primary < y.primary;
                     });
    return groups;
}

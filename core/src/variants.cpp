#include "core/variants.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <regex>
#include <set>

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
    char close = s.back();
    if (close != ')' && close != ']') return std::string::npos;
    char open = (close == ')') ? '(' : '[';
    int depth = 0;
    for (size_t i = s.size(); i-- > 0; ) {
        if (s[i] == close) depth++;
        else if (s[i] == open) {
            depth--;
            if (depth == 0) {
                // Must be a *suffix* group, not the whole string
                // ("(What's The Story) Morning Glory?" keeps its parens).
                if (i == 0) return std::string::npos;
                inner = s.substr(i + 1, s.size() - i - 2);
                if (s[i - 1] != ' ') {
                    // No space before the bracket: only accept groups that
                    // are unmistakably credits ("Name(feat. X)" happens in
                    // sloppy tags), never bare ones — "R(A)W" stays whole.
                    std::string low = inner.substr(0, 5);
                    for (auto& ch : low) ch = (char)tolower((unsigned char)ch);
                    bool credit = low.rfind("feat", 0) == 0 || low.rfind("ft.", 0) == 0 ||
                                  low.rfind("ft ", 0) == 0  || low.rfind("with ", 0) == 0;
                    if (!credit) { inner.clear(); return std::string::npos; }
                }
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
        bool spaceBefore = i > 0 && s[i - 1] == ' ';
        bool spaceAfter  = i + len < s.size() && s[i + len] == ' ';
        if (spaceBefore || spaceAfter) { best = i; bestLen = len; }
        i += len;
    }
    if (best == std::string::npos) return std::string::npos;

    size_t t = best + bestLen;
    while (t < s.size() && s[t] == ' ') t++;
    if (t >= s.size()) return std::string::npos;          // trailing dash, no tail
    std::string head = s.substr(0, best);
    while (!head.empty() && head.back() == ' ') head.pop_back();
    if (head.size() < 2) return std::string::npos;        // "- Title" IS the title

    // The tail must START CAPITALISED. Edition tags are, by convention, in
    // every script that has cases: "Edición Especial", "Remastered 2011",
    // "Deluxe". Titles that merely contain a dash usually are not — this is
    // what keeps "v.i.p. - very important pony" whole, which the whitespace
    // rule alone would have chopped in half. A non-ASCII first byte is taken
    // on trust: telling case there needs Unicode tables this app has no reason
    // to carry, and an accented edition tag ("Édition Collector") is
    // capitalised anyway.
    unsigned char f = (unsigned char)s[t];
    if (f < 0x80 && !(isupper(f) || isdigit(f))) return std::string::npos;

    tail = s.substr(t);
    return best;
}

// '·' (U+00B7), the separator stacked modifiers are joined with.
static const char* kModSep = " \xC2\xB7 ";

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
    for (int pass = 0; pass < 3; pass++) {
        std::string inner;
        size_t at = trailingGroup(base, inner);
        if (at == std::string::npos || inner.empty()) break;
        std::string rest = base.substr(0, at);
        while (!rest.empty() && rest.back() == ' ') rest.pop_back();
        if (rest.empty()) break;
        base = rest;
        mod = mod.empty() ? inner : inner + kModSep + mod;
    }
    // Then at most one dash-separated tail, after the brackets are off — so
    // "Album - Deluxe (feat. X)" reads base "Album", mod "Deluxe · feat. X".
    {
        std::string tail;
        size_t at = trailingDashModifier(base, tail);
        if (at != std::string::npos) {
            base.resize(at);
            while (!base.empty() && base.back() == ' ') base.pop_back();
            mod = mod.empty() ? tail : tail + kModSep + mod;
        }
    }
    // No group found: keep the (whitespace-trimmed) name as the base.
    return !mod.empty();
}

// ── Release-type classification ─────────────────────────────────────────────
// Ported verbatim from the sibling Android player's AlbumDao.java
// (isRemixTrack/isRemixAlbum/classifyRelease) — see the design spec at
// docs/superpowers/specs/2026-07-27-release-type-and-quality-color-design.md.
// Moved here from library.cpp so the test can link it: it is pure string and
// Track logic, and it belongs beside the grouping that reads its answer.

bool isRemixTrackTitle(const std::string& title) {
    if (title.empty()) return false;
    std::string lower = title;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                    [](unsigned char c) { return std::tolower(c); });
    size_t b = lower.find_first_not_of(" \t");
    size_t e = lower.find_last_not_of(" \t");
    std::string trimmed = (b == std::string::npos) ? "" : lower.substr(b, e - b + 1);
    if (trimmed == "remix" || trimmed == "mix" ||
        trimmed == "the remix" || trimmed == "the mix") return false;
    if (lower.find("remix") != std::string::npos) return true;
    if (lower.find("rmx") != std::string::npos) return true;
    static const std::regex kWordMix(R"(\b\w+\s+mix\b)", std::regex::icase);
    static const std::regex kParenMix(R"(\(.*mix.*\))", std::regex::icase);
    static const std::regex kBracketMix(R"(\[.*mix.*\])", std::regex::icase);
    if (std::regex_search(title, kWordMix))    return true;
    if (std::regex_search(title, kParenMix))   return true;
    if (std::regex_search(title, kBracketMix)) return true;
    return false;
}

namespace {

// "Most of this is remixes" over one flat list of titles — the Android
// reference's rule, unchanged: all of them, or a strict majority of at least
// two.
bool remixMajority(const std::vector<const Track*>& tracks) {
    int trackCount = (int)tracks.size();
    if (trackCount == 0) return false;
    int remixCount = 0;
    for (const Track* t : tracks)
        if (isRemixTrackTitle(t->title)) remixCount++;
    return remixCount == trackCount || (remixCount >= 2 && remixCount * 2 > trackCount);
}

bool isRemixAlbum(const std::string& albumName, const std::vector<Track>& tracks) {
    static const std::regex kRemixName(R"(\b(remix|remixes|remixed|rmx)\b)", std::regex::icase);
    if (!albumName.empty() && std::regex_search(albumName, kRemixName)) return true;
    if (tracks.empty()) return false;

    // Group by tagged disc. discNumber is 0 for every track of a single-disc
    // release (see core/library.h), so a library with no DISCNUMBER tags lands
    // in a single bucket and behaves exactly as before.
    std::map<int, std::vector<const Track*>> byDisc;
    for (const Track& t : tracks) byDisc[t.discNumber].push_back(&t);

    if (byDisc.size() < 2) {
        std::vector<const Track*> all;
        all.reserve(tracks.size());
        for (const Track& t : tracks) all.push_back(&t);
        return remixMajority(all);
    }

    // MULTI-DISC: every disc must be a remix disc. One disc of remixes bolted
    // onto a disc of new material is a bonus disc, and the release is still an
    // album — but counted flat, those remixes can outnumber the originals and
    // drag the whole thing into the Remixes tab. Anyma's *Genesys II* is the
    // case that exposed this: 10 original tracks on disc 1, 11 remixes on disc
    // 2, so 11 of 21 titles matched and a sequel album was filed as a remix
    // set. Per disc, disc 1 answers no, and the album stays an album.
    for (auto& [disc, discTracks] : byDisc)
        if (!remixMajority(discTracks)) return false;
    return true;
}

} // namespace

Album::ReleaseType classifyReleaseType(const std::string& albumName,
                                       const std::vector<Track>& tracks) {
    if (isRemixAlbum(albumName, tracks)) return Album::ReleaseType::Remix;
    int trackCount = (int)tracks.size();
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
static std::string fold(const std::string& s) {
    static const char* kFolded =
        // U+00C0..U+00FF, one ASCII letter each ('?' where there is no
        // sensible single-letter fold — those compare against themselves).
        "aaaaaaaceeeeiiiidnooooo?ouuuuy??"   // C0-DF
        "aaaaaaaceeeeiiiidnooooo?ouuuuy?y";  // E0-FF
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) {
            out += (char)tolower(c);
            i++;
        } else if ((c == 0xC3) && i + 1 < s.size()) {
            unsigned char lo = (unsigned char)s[i + 1];
            // C3 80..BF is U+00C0..U+00FF.
            if (lo >= 0x80 && lo <= 0xBF) {
                char f = kFolded[lo - 0x80];
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
static const char* kEditionTerms[] = {
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
static const char* kRemixTerms[] = {
    "remix", "remixes", "remixed", "remixe", "remixes", "rmx",
    "bootleg", "rework", "reworked", "flip", "vip", "edits",
    "remezcla", "remezclas",
};

static bool isWordIn(const std::string& word, const char* const* table, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (word == table[i]) return true;
    return false;
}

ModifierKind classifyModifier(const std::string& mod) {
    std::string f = fold(mod);
    // Trim, then reject an all-whitespace modifier the same as an empty one.
    size_t b = f.find_first_not_of(" \t");
    if (b == std::string::npos) return ModifierKind::None;

    // Word-boundary matching, not substring: "Superstition" must not match
    // "super", and "Reversion" must not match "version". Any non-alphanumeric
    // byte separates words, which also splits "20th Anniversary Edition" and
    // "Remastered 2011" the way you'd expect.
    //
    // Remix wins over Edition when a modifier somehow says both ("Deluxe
    // Remixes"): the reworked music is the more surprising fact, and it is the
    // one that changes how the group is drawn.
    bool edition = false, remix = false;
    std::string word;
    auto flush = [&]() {
        if (word.empty()) return;
        if (isWordIn(word, kEditionTerms, sizeof(kEditionTerms) / sizeof(*kEditionTerms)))
            edition = true;
        if (isWordIn(word, kRemixTerms, sizeof(kRemixTerms) / sizeof(*kRemixTerms)))
            remix = true;
        word.clear();
    };
    for (size_t i = b; i < f.size(); i++) {
        unsigned char c = (unsigned char)f[i];
        if (c < 0x80 && (isalnum(c) != 0)) word += (char)c;
        else if (c >= 0x80)                word += (char)c;   // keep non-ASCII in-word
        else                               flush();
    }
    flush();
    if (remix)   return ModifierKind::Remix;
    if (edition) return ModifierKind::Edition;
    return ModifierKind::Other;
}

// ── Grouping ────────────────────────────────────────────────────────────────

std::string variantKey(const Album& a) {
    std::string base, mod;
    splitNameModifier(a.displayName, base, mod);

    // Drop the Edition and Remix segments, keep everything else.
    // "Album (Deluxe)" collapses onto "Album"; "Obsessed (Remixes)" and
    // "Obsessed (X & Y VIP Remix)" collapse onto each other. But
    // "Album (feat. X)" stays apart, and "Album (Deluxe) [feat. X]" lands with
    // the feat. version — the release it is actually another pressing of.
    std::string kept;
    const std::string sep = kModSep;
    size_t pos = 0;
    while (pos <= mod.size()) {
        size_t at  = mod.find(sep, pos);
        std::string seg = mod.substr(pos, at == std::string::npos ? std::string::npos
                                                                 : at - pos);
        if (classifyModifier(seg) == ModifierKind::Other) {
            if (!kept.empty()) kept += '\x1e';
            kept += fold(seg);
        }
        if (at == std::string::npos) break;
        pos = at + sep.size();
    }

    // \x1f between the fields so an artist ending in the next field's opening
    // text can't forge a match ("A" + "BC" vs "AB" + "C"). The release type
    // leads, so a group can never straddle two of the grid's tabs.
    return std::string(1, (char)('0' + (int)a.releaseType)) + '\x1f' +
           fold(a.artist) + '\x1f' + fold(base) + '\x1f' + kept;
}

// ── Track identity ──────────────────────────────────────────────────────────

// Folds `s` and drops only its EDITION modifier segments, keeping every other
// segment joined by \x1e. Both halves of trackKey() run through this, so
// "Song (Remastered 2011)" and "Song" reduce alike while "Song (X VIP Remix)"
// keeps the segment that makes it different music.
static std::string stripEditionMods(const std::string& s) {
    std::string base, mod;
    splitNameModifier(s, base, mod);   // on no split: base = trimmed s, mod = ""
    std::string out = fold(base);

    const std::string sep = kModSep;
    size_t pos = 0;
    while (pos <= mod.size()) {
        size_t at  = mod.find(sep, pos);
        std::string seg = mod.substr(pos, at == std::string::npos ? std::string::npos
                                                                  : at - pos);
        ModifierKind kind = classifyModifier(seg);
        if (kind != ModifierKind::Edition && kind != ModifierKind::None) {
            out += '\x1e';
            out += fold(seg);
        }
        if (at == std::string::npos) break;
        pos = at + sep.size();
    }
    return out;
}

// Filename without directory or extension. Only ever reached for a track whose
// TITLE tag is empty, where it is the last stable thing left to key on.
static std::string fileStem(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0) name = name.substr(0, dot);
    return name;
}

std::string trackKey(const Track& t) {
    // ALBUMARTIST first: on a compilation every track carries a different
    // ARTIST, and keying on that would file the same track under a new
    // identity the day a tagger fills the field in differently.
    const std::string& artist = t.albumArtist.empty() ? t.artist : t.albumArtist;
    const std::string  title  = t.title.empty() ? fileStem(t.filePath) : t.title;

    // \x1f between the fields, same reason as variantKey(): so text spilling
    // from one field into the next can't forge a match.
    char nums[32];
    snprintf(nums, sizeof(nums), "%d\x1f%d", t.discNumber, t.trackNumber);
    return fold(artist) + '\x1f' + stripEditionMods(t.album) + '\x1f' +
           stripEditionMods(title) + '\x1f' + nums;
}

// Lowercase extension of `path`, without the dot ("" if none). Deliberately
// byte-wise and ASCII-only: extensions are ASCII by definition, and this must
// not depend on the C locale.
static std::string lowerExt(const std::string& path) {
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return {};
    size_t slash = path.find_last_of("/\\");
    if (slash != std::string::npos && dot < slash) return {};   // dot in a dir name
    std::string ext = path.substr(dot + 1);
    for (char& c : ext) c = (char)tolower((unsigned char)c);
    return ext;
}

std::string variantFormatLabel(const Album& a) {
    if (a.hasDsd) return "DSD";
    // One label per album, so a mixed folder answers with the first thing it
    // finds worth saying. Mixed-format albums are pathological; silently
    // labelling them by their first flagged track beats inventing a rule
    // nobody asked for.
    for (const Track& t : a.tracks) {
        std::string ext = lowerExt(t.filePath);
        if (ext == "dsf" || ext == "dff") return "DSD";
        if (ext == "mp3")                 return "MP3";
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
    int da = maxBitDepth(a), db = maxBitDepth(b);
    if (da != db)                           return da > db;
    if (a.tracks.size() != b.tracks.size()) return a.tracks.size() > b.tracks.size();
    // Last resort, so the order never depends on how the scan happened to
    // enumerate the folders. Albums identical in every input above compare
    // equal here too when their folder names match, which keeps this a
    // strict weak ordering.
    return a.name < b.name;
}

std::vector<AlbumGroup> groupAlbumVariants(const std::vector<Album>& albums) {
    // Keyed map, in first-appearance order via the index it stores.
    std::map<std::string, size_t> byKey;
    std::vector<AlbumGroup> groups;

    for (int i = 0; i < (int)albums.size(); i++) {
        std::string key = variantKey(albums[i]);
        auto it = byKey.find(key);
        if (it == byKey.end()) {
            byKey.emplace(key, groups.size());
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

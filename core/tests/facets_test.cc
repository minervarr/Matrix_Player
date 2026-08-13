// Asserts must stay live even though the app builds Release (NDEBUG).
#undef NDEBUG
#include <cassert>
#include <chrono>   // the scale case's tripwire, see the bottom of main()
#include <cstdio>
#include <string>
#include <vector>

// The REAL guided-search logic, linked from src/facets.cpp — not restated
// here. facets.cpp is pure (no sqlite, no Canvas, no OS), so this test links
// that one translation unit and nothing else. If it ever needs another
// source, something leaked into the search engine.
#include "core/facets.h"

using namespace facets;

// ── Fixtures ────────────────────────────────────────────────────────────────

static Album mk(const std::string& artist, const std::string& name, int year,
                int depth, Album::ReleaseType type, const std::string& genre,
                int nTracks = 5) {
    Album a;
    a.artist      = artist;
    a.displayName = name;
    a.name        = name;
    a.releaseType = type;
    for (int i = 0; i < nTracks; i++) {
        Track t;
        t.title      = name + " part " + std::to_string(i + 1);
        t.artist     = artist;
        t.year       = year;
        t.bitDepth   = depth;
        t.sampleRate = 44100;
        t.genre      = genre;
        a.tracks.push_back(t);
    }
    return a;
}

// Björk's two 16-bit records sit in the nineties; the 24-bit ones are all
// later. That gap is what makes the "exists, but not HERE" case real.
static std::vector<Album> lib() {
    return {
        mk("Björk",            "Debut",          1993, 16, Album::ReleaseType::Album, "Electronic"),
        mk("Björk",            "Homogenic",      1997, 16, Album::ReleaseType::Album, "Electronic"),
        mk("Radiohead",        "I Might Be Wrong", 2001, 16, Album::ReleaseType::Live,  "Rock"),
        mk("Boards of Canada", "Geogaddi",       2002, 24, Album::ReleaseType::Album, "Electronic"),
        mk("Radiohead",        "In Rainbows",    2007, 24, Album::ReleaseType::Album, "Rock"),
    };
}

static const Suggestion* find(const std::vector<Suggestion>& ss, const std::string& label) {
    for (const Suggestion& s : ss)
        if (s.label == label) return &s;
    return nullptr;
}

static Chip decade(int from) {
    Chip c; c.kind = Kind::Decade; c.value = std::to_string(from) + "s";
    c.from = from; c.to = from + 9; return c;
}

static bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

int main() {
    // ── albumYear: the most common tagged year; 0 when nothing is tagged ────
    // Album has no year field of its own, so it is derived from the tracks.
    // A reissue often carries one stray remaster year on a bonus track; the
    // majority is the release, not the outlier.
    {
        Album a = mk("X", "Mixed", 1993, 16, Album::ReleaseType::Album, "");
        a.tracks[4].year = 2011;              // one stray year
        assert(albumYear(a) == 1993);

        Album untagged = mk("X", "Untagged", 0, 16, Album::ReleaseType::Album, "");
        assert(albumYear(untagged) == 0);     // renders as "Unknown", never 1970
    }

    // ── normalize: accents and case stop mattering ─────────────────────────
    {
        assert(normalize("Björk") == normalize("bjork"));
        assert(normalize("Edición Especial") == normalize("edicion especial"));
        assert(normalize("  A.B.C!  ") == normalize("abc"));
        // Non-Latin scripts pass through: equality must still work.
        assert(normalize("Ветер") == normalize("Ветер"));
        assert(!normalize("Ветер").empty());
    }

    // ── editDistance is bounded ────────────────────────────────────────────
    {
        assert(editDistance("kitten", "sitting", 10) == 3);
        assert(editDistance("abc", "abc", 5) == 0);
        assert(editDistance("abc", "xyzuvw", 2) > 2);   // abandoned past max
    }

    // ── matches: OR inside a kind, AND across kinds ────────────────────────
    {
        std::vector<Album> l = lib();
        std::vector<Chip> chips = { decade(1990) };
        assert(matches(l[0], chips));    // Debut, 1993
        assert(!matches(l[4], chips));   // In Rainbows, 2007

        // Two decades = either of them, NOT neither.
        chips.push_back(decade(2000));
        assert(matches(l[0], chips));
        assert(matches(l[4], chips));

        // A different kind narrows it again.
        Chip live; live.kind = Kind::Type; live.value = "Live";
        chips.push_back(live);
        assert(!matches(l[0], chips));   // Debut is an Album, not Live
        assert(matches(l[2], chips));    // I Might Be Wrong is Live, 2001
    }

    // ── The connective is DERIVED, never chosen by the listener ────────────
    // Two values of one group are alternatives (OR); different groups narrow
    // each other (AND). A year and a decade are the same group — both answer
    // "when" — so they must read as OR, not as an impossible AND.
    {
        Chip q; q.kind = Kind::Quality; q.value = "24-bit";
        Chip y; y.kind = Kind::Year; y.value = "1993"; y.from = y.to = 1993;
        assert(sameGroup(decade(1990), decade(2000)));
        assert(sameGroup(decade(1990), y));
        assert(!sameGroup(decade(1990), q));
    }

    // ── A name is fuzzy-matched: "bjork" finds "Björk" ─────────────────────
    {
        std::vector<Suggestion> ss = suggest(lib(), "bjork", {});
        const Suggestion* s = find(ss, "Björk");
        assert(s != nullptr);
        assert(s->chip.kind == Kind::Name);
        assert(s->count == 2);           // Debut + Homogenic
        assert(s->enabled);
    }

    // ── A year and a decade are offered SEPARATELY, never guessed ──────────
    {
        std::vector<Suggestion> ss = suggest(lib(), "1993", {});
        const Suggestion* exact  = find(ss, "1993");
        const Suggestion* tenner = find(ss, "1990s");
        assert(exact  != nullptr && exact->chip.kind  == Kind::Year);
        assert(tenner != nullptr && tenner->chip.kind == Kind::Decade);
        assert(exact->count  == 1);      // Debut alone
        assert(tenner->count == 2);      // Debut + Homogenic
    }

    // ── What does not exist ANYWHERE is never suggested ────────────────────
    // No dead "(0)" row for a value the library has never held.
    {
        std::vector<Suggestion> ss = suggest(lib(), "", {});
        assert(find(ss, "32-bit") == nullptr);
        assert(find(ss, "1980s")  == nullptr);
        assert(find(ss, "Remix")  == nullptr);   // no remix release in this library
    }

    // ── What exists but not HERE is suggested, disabled, and explains ──────
    // The whole reason "24-bit (0)" was not good enough: it could not tell
    // "you own no 24-bit at all" from "none in the nineties".
    {
        std::vector<Chip> chips = { decade(1990) };
        std::vector<Suggestion> ss = suggest(lib(), "", chips);
        const Suggestion* hires = find(ss, "24-bit");
        assert(hires != nullptr);        // offered, because it DOES exist
        assert(!hires->enabled);         // but greyed out
        assert(hires->count == 0);

        const Suggestion* cd = find(ss, "16-bit");
        assert(cd != nullptr && cd->enabled && cd->count == 2);
    }

    // ── A suggestion's count ignores its OWN kind ──────────────────────────
    // With 1990s already placed, offering 2000s must count the 2000s records,
    // not zero — sibling values are alternatives (OR), not extra filters.
    // Getting this wrong makes picking one option grey out all its siblings,
    // the classic faceted-search bug.
    {
        std::vector<Chip> chips = { decade(1990) };
        std::vector<Suggestion> ss = suggest(lib(), "", chips);
        const Suggestion* s = find(ss, "2000s");
        assert(s != nullptr);
        assert(s->enabled);
        assert(s->count == 3);           // I Might Be Wrong, Geogaddi, In Rainbows
    }

    // ── Chips already placed are not offered again ─────────────────────────
    {
        std::vector<Chip> chips = { decade(1990) };
        std::vector<Suggestion> ss = suggest(lib(), "", chips);
        assert(find(ss, "1990s") == nullptr);
    }

    // ── explainEmpty names the chip that killed the result ─────────────────
    {
        Chip hires; hires.kind = Kind::Quality; hires.value = "24-bit";
        std::vector<Chip> chips = { decade(1990), hires };

        EmptyReason r = explainEmpty(lib(), chips);
        assert(r.empty);
        assert(r.culpritIndex == 0);                 // the decade, not the depth
        assert(contains(r.message, "24-bit"));
        assert(contains(r.message, "1990"));

        // A chip set that DOES match explains nothing.
        std::vector<Chip> ok = { decade(1990) };
        EmptyReason none = explainEmpty(lib(), ok);
        assert(!none.empty);
        assert(none.culpritIndex == -1);
        assert(none.message.empty());
    }

    // ── A digit run longer than a year must not abort the process ──────────
    // typedMatches() used to convert the typed text to an int BEFORE checking
    // that it was four digits long, and std::stoi answers anything past
    // INT_MAX by throwing — so holding a number key down killed the app. A
    // search box accepts whatever the listener types; reaching the assertions
    // below at all is the regression check.
    {
        std::vector<Suggestion> ss = suggest(lib(), "1234567890123", {});
        for (const Suggestion& s : ss)
            assert(s.chip.kind != Kind::Year && s.chip.kind != Kind::Decade);

        // Five digits name no year either — but four still do, and a partial
        // year still offers BOTH readings of what was typed.
        assert(find(suggest(lib(), "19931", {}), "1990s") == nullptr);
        assert(find(suggest(lib(), "199",   {}), "1990s") != nullptr);
        assert(find(suggest(lib(), "199",   {}), "1993")  != nullptr);
    }

    // ── An untagged name is never offered as a chip ────────────────────────
    // nameHit() treats an empty needle as "no name constraint" and so matches
    // EVERY album. A Name candidate built from an untagged artist therefore
    // used to offer a chip with no text on it whose count was the whole
    // library — and since the ranking is by count descending, it sorted FIRST,
    // which is the row Tab and Enter accept.
    {
        std::vector<Album> l = lib();
        l.push_back(mk("", "", 1995, 16, Album::ReleaseType::Album, ""));

        std::vector<Suggestion> ss = suggest(l, "a", {});
        assert(find(ss, "") == nullptr);
        for (const Suggestion& s : ss) {
            assert(!s.label.empty());
            assert(!(s.chip.kind == Kind::Name && s.chip.value.empty()));
            assert(s.count <= (int)l.size());
        }

        // The real rows are untouched by the guard.
        const Suggestion* bj = find(suggest(l, "bjork", {}), "Björk");
        assert(bj != nullptr && bj->count == 2 && bj->enabled);
    }

    // ── Diacritics fold through the PUBLIC path, not just in normalize() ───
    // normalize() is asserted in isolation above, but what matters is that
    // suggest() actually goes through it: an accented query must find an
    // unaccented record, which edit distance alone would only manage by luck
    // on short words.
    {
        std::vector<Album> l = { mk("Bjork", "Vespertine", 2001, 24,
                                    Album::ReleaseType::Album, "Electronic") };
        const Suggestion* s = find(suggest(l, "björk", {}), "Bjork");
        assert(s != nullptr && s->count == 1 && s->enabled);
    }

    // ── Counts stay right at scale ─────────────────────────────────────────
    // 4000 albums over 50 distinct years: every decade holds exactly 800 and
    // every year exactly 80. suggest() counts by bucketing one pass per chip
    // GROUP rather than by walking the library once per candidate, and this is
    // what stops a rewrite of that from being fast and quietly WRONG.
    //
    // There is deliberately NO wall-clock assertion here, though the cost is
    // the reason the counting was rewritten (one keystroke over 4000 albums
    // measured 1443 ms before, 47 ms after). Two things make a time limit
    // untrustworthy in exactly this file: these tests are Debug-only targets,
    // so they run unoptimized, and this machine measured a 4.5x spread across
    // runs of one unchanged binary. A test that fails for the wrong reason is
    // worse than no test. The timing is printed instead, so a regression is
    // visible to anyone reading the output.
    {
        std::vector<Album> big;
        for (int i = 0; i < 4000; i++)
            big.push_back(mk("Artist " + std::to_string(i),
                             "Record " + std::to_string(i),
                             1970 + (i % 50), (i % 2) ? 16 : 24,
                             Album::ReleaseType::Album,
                             (i % 3) ? "Rock" : "Electronic",
                             1));

        std::vector<Suggestion> ss = suggest(big, "", {});
        const Suggestion* d = find(ss, "1990s");
        assert(d != nullptr && d->count == 800);
        const Suggestion* y = find(suggest(big, "1993", {}), "1993");
        assert(y != nullptr && y->count == 80);

        auto t0 = std::chrono::steady_clock::now();
        std::vector<Suggestion> typed = suggest(big, "a", {});
        double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        assert(!typed.empty());   // a measurement of nothing is worthless

        // One common letter typed into a library this size matches most
        // artists AND most record titles, so the name field is capped — see
        // kMaxNameCands in facets.cpp. That cap is the reason this number is
        // bounded at all, so assert it holds rather than only printing it.
        int names = 0;
        for (const Suggestion& s : typed)
            if (s.chip.kind == Kind::Name) names++;
        assert(names <= 64);
        printf("facets_test: suggest() over %zu albums, one letter typed: "
               "%.0f ms, %zu rows (%d names)\n",
               big.size(), ms, typed.size(), names);
    }

    printf("facets_test: all assertions passed\n");
    return 0;
}

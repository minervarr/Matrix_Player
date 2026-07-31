// Asserts must stay live even though the app builds Release (NDEBUG).
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

// The REAL grouping logic, linked from src/variants.cpp — not restated here.
// variants.cpp is pure (no OS, no Canvas), so this test links that one
// translation unit and nothing else.
#include "core/variants.h"

// ── Fixtures ────────────────────────────────────────────────────────────────

// An album with `n` identical tracks at the given format. displayName is what
// the grouping reads; name (the raw folder) only breaks final ties, so it is
// given the same text plus the quality suffix the folder convention uses.
static Album mk(const std::string& artist, const std::string& displayName,
                int rate, int depth, int n, bool dsd = false,
                Album::ReleaseType type = Album::ReleaseType::Album) {
    Album a;
    a.artist      = artist;
    a.displayName = displayName;
    a.releaseType = type;
    a.name        = displayName + " (" + std::to_string(depth) + "-" +
                    std::to_string(rate / 1000) + ")";
    a.avgSampleRate = rate;
    a.hasDsd        = dsd;
    for (int i = 0; i < n; i++) {
        Track t;
        t.title       = "Track " + std::to_string(i + 1);
        t.trackNumber = i + 1;
        t.sampleRate  = rate;
        t.bitDepth    = depth;
        a.tracks.push_back(t);
    }
    return a;
}

// Index of the group containing album `idx`, or -1.
static int groupOf(const std::vector<AlbumGroup>& gs, int idx) {
    for (size_t g = 0; g < gs.size(); g++)
        for (int m : gs[g].members)
            if (m == idx) return (int)g;
    return -1;
}

int main() {
    // ── splitNameModifier survived the move out of the GUI ─────────────────
    // Not an exhaustive re-test of the splitter; a guard that the behaviour
    // the grid depends on came across intact.
    {
        std::string base, mod;
        assert(splitNameModifier("Senderos De Traición - Edición Especial", base, mod));
        assert(base == "Senderos De Traición");
        assert(mod  == "Edición Especial");

        assert(splitNameModifier("Rush (Are You Coming)", base, mod));
        assert(base == "Rush" && mod == "Are You Coming");

        // Stacked groups fold into one " · "-joined modifier.
        assert(splitNameModifier("Tears (with X) [feat. Y]", base, mod));
        assert(base == "Tears" && mod == "with X \xC2\xB7 feat. Y");

        // In-word dashes are not modifiers.
        assert(!splitNameModifier("Jay-Z", base, mod));
        assert(base == "Jay-Z" && mod.empty());

        // Nothing to split: base is the (trimmed) name.
        assert(!splitNameModifier("Avalancha ", base, mod));
        assert(base == "Avalancha" && mod.empty());
    }

    // ── classifyModifier reads meaning, not punctuation ─────────────────────
    {
        assert(classifyModifier("Deluxe")          == ModifierKind::Edition);
        assert(classifyModifier("Edición Especial") == ModifierKind::Edition);
        assert(classifyModifier("Edicion Especial") == ModifierKind::Edition);  // unaccented
        assert(classifyModifier("DELUXE EDITION")   == ModifierKind::Edition);
        assert(classifyModifier("Remastered 2011")  == ModifierKind::Edition);
        assert(classifyModifier("20th Anniversary Edition") == ModifierKind::Edition);

        assert(classifyModifier("feat. Sleepnet & Joker") == ModifierKind::Other);
        assert(classifyModifier("with X")                 == ModifierKind::Other);
        assert(classifyModifier("from the Netflix Series Arcane") == ModifierKind::Other);
        assert(classifyModifier("Live at Wembley")        == ModifierKind::Other);
        assert(classifyModifier("")                       == ModifierKind::None);

        // Word-boundary matching: "Superstition" is not "super", and
        // "Reversion" is not "version".
        assert(classifyModifier("Superstition") == ModifierKind::Other);
        assert(classifyModifier("Reversion")    == ModifierKind::Other);

        // Remix is its own kind — the real library's two cases, plus the
        // spellings around them.
        assert(classifyModifier("Remixes") == ModifierKind::Remix);
        assert(classifyModifier("horsegiirL & Luvhunter VIP Remix") == ModifierKind::Remix);
        assert(classifyModifier("SoFTT Remix")  == ModifierKind::Remix);
        assert(classifyModifier("Bootleg")      == ModifierKind::Remix);
        assert(classifyModifier("Kaytranada Rework") == ModifierKind::Remix);
        // "mix" alone is NOT a remix term — too many ordinary names carry it.
        assert(classifyModifier("Original Mix") == ModifierKind::Other);
        assert(classifyModifier("Final Mix")    == ModifierKind::Other);
        // Remix wins when a modifier claims both.
        assert(classifyModifier("Deluxe Remixes") == ModifierKind::Remix);
    }

    // ── Two qualities of the same disc: one group, higher rate wins ─────────
    {
        std::vector<Album> albums = {
            mk("Héroes Del Silencio", "Avalancha", 44100, 16, 12),
            mk("Héroes Del Silencio", "Avalancha", 96000, 24, 12),
        };
        auto gs = groupAlbumVariants(albums);
        assert(gs.size() == 1);
        assert(gs[0].members.size() == 2);
        assert(gs[0].primary == 1);            // the 24/96
        assert(gs[0].members[0] == 1);         // members are best-first
        assert(gs[0].members[1] == 0);
    }

    // ── An edition suffix groups with the plain album ──────────────────────
    {
        std::vector<Album> albums = {
            mk("Héroes Del Silencio", "Senderos De Traición", 44100, 16, 12),
            mk("Héroes Del Silencio", "Senderos De Traición - Edición Especial",
               44100, 16, 18),
        };
        auto gs = groupAlbumVariants(albums);
        assert(gs.size() == 1);
        // Same quality on both, so track count decides: the 18-track edition.
        assert(gs[0].primary == 1);
    }

    // ── Accent spelling does not split a group ─────────────────────────────
    {
        std::vector<Album> albums = {
            mk("Artist", "Album (Edición Especial)", 44100, 16, 10),
            mk("Artist", "Album (Edicion Especial)", 96000, 24, 10),
            mk("Artist", "Album",                    44100, 16, 10),
        };
        auto gs = groupAlbumVariants(albums);
        assert(gs.size() == 1);
        assert(gs[0].members.size() == 3);
        assert(gs[0].primary == 1);
    }

    // ── A non-edition modifier is a DIFFERENT release ──────────────────────
    {
        std::vector<Album> albums = {
            mk("Artist", "Album",             44100, 16, 10),
            mk("Artist", "Album (feat. X)",   44100, 16, 10),
            mk("Artist", "Album (Live at Wembley)", 44100, 16, 10),
        };
        auto gs = groupAlbumVariants(albums);
        assert(gs.size() == 3);
    }

    // ── An edition stacked on a "feat." credit still groups by the credit ──
    {
        std::vector<Album> albums = {
            mk("Artist", "Album (feat. X)",          44100, 16, 10),
            mk("Artist", "Album (Deluxe) [feat. X]", 96000, 24, 12),
            mk("Artist", "Album",                    44100, 16, 10),
        };
        auto gs = groupAlbumVariants(albums);
        assert(gs.size() == 2);
        assert(groupOf(gs, 0) == groupOf(gs, 1));   // both carry "feat. X"
        assert(groupOf(gs, 2) != groupOf(gs, 0));   // the plain one stands apart
    }

    // ── Same album name, different artists: never one group ────────────────
    {
        std::vector<Album> albums = {
            mk("Artist One", "Greatest Hits", 44100, 16, 10),
            mk("Artist Two", "Greatest Hits", 44100, 16, 10),
        };
        auto gs = groupAlbumVariants(albums);
        assert(gs.size() == 2);
    }

    // ── Quality beats completeness: DSD standard over a PCM deluxe ─────────
    // TODO.md's decided tie-break. hasDsd is hardcoded false in the scanner
    // today, so this case is the rule's only live exercise until DSD decoding
    // lands — which is exactly why it is pinned here.
    {
        std::vector<Album> albums = {
            mk("Artist", "Album (Deluxe)", 96000,  24, 20, /*dsd=*/false),
            mk("Artist", "Album",          44100,  1,   9, /*dsd=*/true),
        };
        auto gs = groupAlbumVariants(albums);
        assert(gs.size() == 1);
        assert(gs[0].primary == 1);          // the DSD, despite 9 tracks vs 20
        assert(gs[0].members[1] == 0);
    }

    // ── Ranking falls through rate → depth → track count ───────────────────
    {
        // Same rate, different depth.
        Album a = mk("A", "X", 44100, 24, 10);
        Album b = mk("A", "X", 44100, 16, 10);
        assert(variantOutranks(a, b));
        assert(!variantOutranks(b, a));

        // Same rate and depth, more tracks wins.
        Album c = mk("A", "X", 44100, 16, 18);
        assert(variantOutranks(c, b));
        assert(!variantOutranks(b, c));

        // Identical in every ranking input: the ordering is still strict, so
        // sorting cannot depend on the input order.
        Album d = mk("A", "X", 44100, 16, 10);
        assert(variantOutranks(b, d) == false && variantOutranks(d, b) == false);
    }

    // ── Release-type classification ────────────────────────────────────────
    {
        auto tracksOf = [](std::vector<std::pair<int, std::string>> spec) {
            std::vector<Track> ts;
            for (auto& [disc, title] : spec) {
                Track t; t.discNumber = disc; t.title = title;
                ts.push_back(t);
            }
            return ts;
        };
        using RT = Album::ReleaseType;

        // Plain track-count thresholds.
        assert(classifyReleaseType("X", tracksOf({{0,"a"}}))                 == RT::Single);
        assert(classifyReleaseType("X", tracksOf({{0,"a"},{0,"b"}}))         == RT::Ep);
        assert(classifyReleaseType("X", tracksOf({{0,"a"},{0,"b"},{0,"c"},
                                                  {0,"d"},{0,"e"}}))         == RT::Album);

        // The album NAME saying "remix" is enough on its own.
        assert(classifyReleaseType("Obsessed (Remixes)", tracksOf({{0,"a"}})) == RT::Remix);

        // A single-disc majority of remix titles, the Android rule, unchanged.
        assert(classifyReleaseType("X", tracksOf({{0,"One (Foo Remix)"},
                                                  {0,"Two (Bar Remix)"},
                                                  {0,"Three"}}))            == RT::Remix);
        // ...and a minority is not enough.
        assert(classifyReleaseType("X", tracksOf({{0,"One (Foo Remix)"},
                                                  {0,"Two"},
                                                  {0,"Three"}}))            != RT::Remix);

        // THE GENESYS II CASE. Disc 1 is ten originals, disc 2 is eleven
        // remixes. Counted flat that is 11 of 21 — a majority — and the album
        // used to land in the Remixes tab. Per disc, it is an album with a
        // bonus remix disc.
        {
            std::vector<std::pair<int, std::string>> spec = {
                {1,"Sacrifice"}, {1,"Now Or Never"}, {1,"The Light"},
                {1,"Simulation"}, {1,"Pictures Of You"}, {1,"Higher Power"},
                {1,"F.T.L."}, {1,"Hear Me Now"}, {1,"Exodus"}, {1,"After Love"},
                {2,"Eternity (Massano Remix)"},
                {2,"Pictures Of You (Cassian Remix)"},
                {2,"The Sign (Kevin de Vries Remix)"},
                {2,"Welcome To The Opera (Adriatique Remix)"},
                {2,"Syren (Adam Sellouk Remix)"},
                {2,"Save Me (Goom Gum & Stylo Remix)"},
                {2,"Consciousness (Eric Prydz Remix)"},
                {2,"Save Me (Kölsch Remix)"},
                {2,"Syren (Amelie Lens Remix)"},
                {2,"Welcome To The Opera (Kobosil 44 Symbiont Mix)"},
                {2,"Explore Your Future (Daniel Avery Remix)"},
            };
            auto ts = tracksOf(spec);
            // The flat majority that used to decide it is still a majority —
            // this asserts the OLD rule would have fired, so the test fails
            // loudly if someone reverts the per-disc split.
            int remixes = 0;
            for (auto& t : ts) if (isRemixTrackTitle(t.title)) remixes++;
            assert(remixes * 2 > (int)ts.size());
            assert(classifyReleaseType("Genesys II", ts) == RT::Album);
        }

        // A genuine multi-disc remix set: EVERY disc is remixes, so it counts.
        assert(classifyReleaseType("X", tracksOf({{1,"A (Foo Remix)"},
                                                  {1,"B (Bar Remix)"},
                                                  {2,"C (Baz Remix)"},
                                                  {2,"D (Qux Remix)"}}))     == RT::Remix);
        // One clean disc is enough to make it not a remix release.
        assert(classifyReleaseType("X", tracksOf({{1,"A"}, {1,"B"},
                                                  {2,"C (Baz Remix)"},
                                                  {2,"D (Qux Remix)"}}))     != RT::Remix);
    }

    // ── Remix sets of the same track group together ────────────────────────
    // The real case from the library: two remix releases of "Obsessed", same
    // artist, differing only in which remixers are named.
    {
        const auto RX = Album::ReleaseType::Remix;
        std::vector<Album> albums = {
            mk("horsegiirL", "Obsessed (Remixes)", 44100, 16, 5, false, RX),
            mk("horsegiirL", "Obsessed (horsegiirL & Luvhunter VIP Remix)",
               44100, 16, 2, false, RX),
        };
        auto gs = groupAlbumVariants(albums);
        assert(gs.size() == 1);
        assert(gs[0].members.size() == 2);
        assert(gs[0].primary == 0);   // same quality, so 5 tracks beats 2
    }

    // ── A group never straddles two release-type tabs ──────────────────────
    // "Obsessed" the single and "Obsessed (Remixes)" the remix set are
    // different KINDS of release. Folding them would make the remix vanish
    // from the Remixes tab, since the grid filters on the group's primary.
    {
        std::vector<Album> albums = {
            mk("horsegiirL", "Obsessed", 44100, 16, 1, false,
               Album::ReleaseType::Single),
            mk("horsegiirL", "Obsessed (Remixes)", 44100, 16, 5, false,
               Album::ReleaseType::Remix),
        };
        auto gs = groupAlbumVariants(albums);
        assert(gs.size() == 2);
        assert(groupOf(gs, 0) != groupOf(gs, 1));
    }

    // ── Same-name releases of different types stay apart ───────────────────
    {
        std::vector<Album> albums = {
            mk("A", "Thing", 44100, 16, 12, false, Album::ReleaseType::Album),
            mk("A", "Thing", 44100, 16,  3, false, Album::ReleaseType::Ep),
        };
        assert(groupAlbumVariants(albums).size() == 2);
    }

    // ── Format label: only what changes the meaning of the numbers ─────────
    {
        // FLAC and WAV say nothing — lossless PCM is the library's baseline.
        Album flac = mk("A", "X", 44100, 16, 2);
        flac.tracks[0].filePath = "/music/A/X/01 - One.flac";
        flac.tracks[1].filePath = "/music/A/X/02 - Two.flac";
        assert(variantFormatLabel(flac).empty());

        Album wav = mk("A", "X", 44100, 16, 1);
        wav.tracks[0].filePath = "/music/A/X/01.WAV";
        assert(variantFormatLabel(wav).empty());

        // MP3 is lossy: its 16/44.1 is reconstructed, so it must be said.
        Album mp3 = mk("A", "X", 44100, 16, 1);
        mp3.tracks[0].filePath = "/music/A/X (mp3)/01 - One.mp3";
        assert(variantFormatLabel(mp3) == "MP3");
        // Case and path punctuation must not matter.
        mp3.tracks[0].filePath = "/music/A/X.hires/01.MP3";
        assert(variantFormatLabel(mp3) == "MP3");

        // DSD, both by extension and by the (currently dead) hasDsd flag.
        Album dsf = mk("A", "X", 2822400, 1, 1);
        dsf.tracks[0].filePath = "/music/A/X/01 - One.dsf";
        assert(variantFormatLabel(dsf) == "DSD");
        dsf.tracks[0].filePath = "/music/A/X/01 - One.dff";
        assert(variantFormatLabel(dsf) == "DSD");

        Album flagged = mk("A", "X", 2822400, 1, 1, /*dsd=*/true);
        flagged.tracks[0].filePath = "/music/A/X/01 - One.flac";
        assert(variantFormatLabel(flagged) == "DSD");   // the flag wins

        // A dot in a directory name is not an extension.
        Album noext = mk("A", "X", 44100, 16, 1);
        noext.tracks[0].filePath = "/music/A/X v2.0/track01";
        assert(variantFormatLabel(noext).empty());

        // No tracks at all: no label, no crash.
        Album empty;
        assert(variantFormatLabel(empty).empty());
    }

    // ── A lone album is a group of one, primary itself ─────────────────────
    {
        std::vector<Album> albums = { mk("Artist", "Only Album", 44100, 16, 11) };
        auto gs = groupAlbumVariants(albums);
        assert(gs.size() == 1);
        assert(gs[0].primary == 0);
        assert(gs[0].members.size() == 1 && gs[0].members[0] == 0);
    }

    // ── Empty input yields no groups (and does not crash) ──────────────────
    {
        std::vector<Album> albums;
        assert(groupAlbumVariants(albums).empty());
    }

    // ── Every album lands in exactly one group ─────────────────────────────
    {
        std::vector<Album> albums = {
            mk("A", "One",            44100, 16, 10),
            mk("A", "One (Deluxe)",   96000, 24, 12),
            mk("B", "Two",            44100, 16,  4),
            mk("A", "Three (feat. Q)", 44100, 16, 1),
            mk("A", "One",           192000, 24, 10),
        };
        auto gs = groupAlbumVariants(albums);
        size_t total = 0;
        for (auto& g : gs) {
            total += g.members.size();
            assert(!g.members.empty());
            assert(g.primary == g.members[0]);   // primary is always the best
        }
        assert(total == albums.size());
        for (int i = 0; i < (int)albums.size(); i++) assert(groupOf(gs, i) >= 0);

        // Group order follows the primaries' indices, so the grid does not
        // reshuffle between scans.
        for (size_t g = 1; g < gs.size(); g++)
            assert(gs[g - 1].primary < gs[g].primary);
    }

    // ── The same input always produces the same output ─────────────────────
    {
        std::vector<Album> albums = {
            mk("A", "Same", 44100, 16, 10),
            mk("A", "Same", 44100, 16, 10),
            mk("A", "Same", 44100, 16, 10),
        };
        auto first = groupAlbumVariants(albums);
        for (int pass = 0; pass < 5; pass++) {
            auto again = groupAlbumVariants(albums);
            assert(again.size() == first.size());
            for (size_t g = 0; g < again.size(); g++) {
                assert(again[g].primary == first[g].primary);
                assert(again[g].members == first[g].members);
            }
        }
    }

    // ── trackKey(): the identity listening history is recorded against ─────
    //
    // Everything below is a statement about what MUST and MUST NOT change a
    // track's identity. A key that moves when it shouldn't orphans history
    // silently; a key that collides merges two different recordings.
    {
        auto tk = [](const std::string& artist, const std::string& album,
                     const std::string& title, int disc = 1, int num = 1,
                     const std::string& path = "/m/x.flac") {
            Track t;
            t.artist      = artist;
            t.album       = album;
            t.title       = title;
            t.discNumber  = disc;
            t.trackNumber = num;
            t.filePath    = path;
            return trackKey(t);
        };

        // An Edition modifier is another pressing, not other music — on the
        // album name and on the title alike.
        assert(tk("A", "Rec",           "Song") == tk("A", "Rec (Deluxe)",  "Song"));
        assert(tk("A", "Rec",           "Song") == tk("A", "Rec - Edición Especial", "Song"));
        assert(tk("A", "Rec", "Song") == tk("A", "Rec", "Song (Remastered 2011)"));

        // A remix is different music and keeps its own history. This is the
        // one place trackKey deliberately diverges from variantKey.
        assert(tk("A", "Rec", "Song") != tk("A", "Rec", "Song (X VIP Remix)"));
        assert(tk("A", "Rec", "Song") != tk("A", "Rec (Remixes)", "Song"));

        // Neither does a "feat." credit fold away.
        assert(tk("A", "Rec", "Song") != tk("A", "Rec", "Song (feat. B)"));

        // Quality is not identity: upgrading a file must not split its
        // history. Nothing about the file — path, rate, depth, size — enters
        // the key, so two copies of one track answer alike.
        {
            Track lo, hi;
            lo.artist = hi.artist = "A";
            lo.album  = hi.album  = "Rec";
            lo.title  = hi.title  = "Song";
            lo.discNumber = hi.discNumber = 1;
            lo.trackNumber = hi.trackNumber = 3;
            lo.filePath = "/m/A/Rec (16-44)/03 Song.flac";
            lo.sampleRate = 44100; lo.bitDepth = 16; lo.fileSize = 111;
            hi.filePath = "/m/A/Rec (24-96)/03 Song.flac";
            hi.sampleRate = 96000; hi.bitDepth = 24; hi.fileSize = 999;
            assert(trackKey(lo) == trackKey(hi));
        }

        // Disc and track number disambiguate: an album that really does carry
        // the same title twice stays two identities.
        assert(tk("A", "Rec", "Song", 1, 3) != tk("A", "Rec", "Song", 1, 9));
        assert(tk("A", "Rec", "Song", 1, 3) != tk("A", "Rec", "Song", 2, 3));

        // Case and Latin-1 accents fold — whether an accent survived tagging
        // is not information about the music.
        assert(tk("Héroes", "Senderos", "Maldito") ==
               tk("HEROES", "SENDEROS", "MALDITO"));

        // ALBUMARTIST wins over ARTIST, so a compilation does not hand every
        // track a new identity the day a tagger fills the fields differently.
        {
            Track a, b;
            a.artist = "Guest One"; a.albumArtist = "Various Artists";
            b.artist = "Guest Two"; b.albumArtist = "Various Artists";
            a.album = b.album = "Comp";
            a.title = b.title = "Same Title";
            a.trackNumber = b.trackNumber = 4;
            assert(trackKey(a) == trackKey(b));
        }

        // An untitled file falls back to its filename stem rather than
        // colliding with every other untitled file by the same artist.
        assert(tk("A", "Rec", "", 1, 0, "/m/first.flac") !=
               tk("A", "Rec", "", 1, 0, "/m/second.flac"));
        // ...and the stem is the name only: directory and extension are not
        // part of it, so moving the file keeps the key.
        assert(tk("A", "Rec", "", 1, 0, "/one/dir/song.flac") ==
               tk("A", "Rec", "", 1, 0, "/other/place/song.flac"));

        // The \x1f field separator does its job: text spilling from one field
        // into the next cannot forge a match.
        assert(tk("A", "BC", "Song") != tk("AB", "C", "Song"));

        // Same input, same answer, every time.
        for (int pass = 0; pass < 5; pass++)
            assert(tk("A", "Rec (Deluxe)", "Song") == tk("A", "Rec", "Song"));
    }

    printf("variants_test: all assertions passed\n");
    return 0;
}

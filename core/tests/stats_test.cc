// Asserts must stay live even though the app builds Release (NDEBUG).
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

// The REAL data layer, linked from src/db.cpp + src/db_stats.cpp +
// src/variants.cpp — not restated here. Nothing else links in: no matrix_core,
// no ae_core, no library.cpp. See the note on the stats_test target in
// core/CMakeLists.txt for why that restriction is the point.
#include "core/db.h"
#include "core/variants.h"
#include "db_schema.h"      // localUtcOffsetMinutes() — src/-private, hence the flat name
#include "sqlite3.h"

// ── Fixtures ────────────────────────────────────────────────────────────────

static Track mkTrack(const std::string& artist, const std::string& album,
                     const std::string& title, int num,
                     const std::string& genre = "", int year = 0,
                     int durationMs = 200000) {
    Track t;
    t.albumArtist = artist;
    t.artist      = artist;
    t.album       = album;
    t.title       = title;
    t.discNumber  = 1;
    t.trackNumber = num;
    t.durationMs  = durationMs;
    t.genre       = genre;
    t.year        = year;
    t.filePath    = "/m/" + artist + "/" + album + "/" +
                    std::to_string(num) + " " + title + ".flac";
    // The scan computes this in buildAlbums(); here the fixture stands in for
    // it, using the same function, so the test keys on exactly what ships.
    t.trackKey    = trackKey(t);
    return t;
}

// A day in unix seconds, and a fixed base instant to build timelines from.
static const int64_t kDay  = 86400;
static const int64_t kBase = 1700000000;   // 2023-11-14 UTC, arbitrary but fixed

// Opens a throwaway in-memory database with `tracks` populated.
static void openWith(Db& db, const std::vector<Track>& tracks) {
    assert(db.open(":memory:"));
    db.saveTracks(tracks);
}

// Records one closed event, returning its id.
static int64_t logPlay(Db& db, const Track& t, int64_t at, int64_t msHeard,
                       bool completed, StartCause sc, EndCause ec) {
    int64_t id = db.beginPlayEvent(t.trackKey, t.filePath, t.durationMs, sc, at);
    assert(id > 0);
    db.endPlayEvent(id, msHeard, completed, ec);
    return id;
}

// Raw SQL escape hatch, used only to build pre-migration databases by hand and
// to check things the public API deliberately does not expose.
static int64_t scalar(const std::string& path, const char* sql) {
    sqlite3* raw = nullptr;
    assert(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
    sqlite3_stmt* st = nullptr;
    assert(sqlite3_prepare_v2(raw, sql, -1, &st, nullptr) == SQLITE_OK);
    int64_t v = -1;
    if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    sqlite3_close(raw);
    return v;
}

static void execRaw(const std::string& path, const char* sql) {
    sqlite3* raw = nullptr;
    assert(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
    assert(sqlite3_exec(raw, sql, nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(raw);
}

int main() {
    // ── An event round-trips exactly as written ────────────────────────────
    {
        Track t = mkTrack("Anyma", "Genesys", "Explore", 1);
        Db db;
        openWith(db, { t });

        int64_t id = db.beginPlayEvent(t.trackKey, t.filePath, t.durationMs,
                                       StartCause::Manual, kBase);
        assert(id > 0);
        db.endPlayEvent(id, 195000, /*completed=*/true, EndCause::Natural);

        TrackStats s = db.trackTotals(t.trackKey);
        assert(s.playCount    == 1);
        assert(s.skipCount    == 0);
        assert(s.listenTimeMs == 195000);

        auto recent = db.recentlyPlayed(10);
        assert(recent.size() == 1);
        assert(recent[0].trackKey   == t.trackKey);
        assert(recent[0].filePath   == t.filePath);
        assert(recent[0].startedAt  == kBase);
        assert(recent[0].msHeard    == 195000);
        assert(recent[0].durationMs == t.durationMs);
        assert(recent[0].completed);
        assert(recent[0].startCause == StartCause::Manual);
        assert(recent[0].endCause   == EndCause::Natural);
        assert(recent[0].endedAt > 0);   // closed
    }

    // ── An empty key is not logged ─────────────────────────────────────────
    // A track with no identity would produce a row nothing can ever attribute
    // or group. Refusing it is what keeps every stored row meaningful.
    {
        Db db;
        openWith(db, {});
        assert(db.beginPlayEvent("", "/m/x.flac", 1000, StartCause::Manual, kBase) == 0);
        assert(db.totals(StatsRange{}).plays == 0);
    }

    // ── Closing twice cannot rewrite a settled verdict ─────────────────────
    {
        Track t = mkTrack("A", "Rec", "Song", 1);
        Db db;
        openWith(db, { t });
        int64_t id = db.beginPlayEvent(t.trackKey, t.filePath, t.durationMs,
                                       StartCause::Manual, kBase);
        db.endPlayEvent(id, 190000, true, EndCause::Natural);
        db.endPlayEvent(id, 5, false, EndCause::Next);      // must be ignored
        auto recent = db.recentlyPlayed(1);
        assert(recent[0].msHeard  == 190000);
        assert(recent[0].completed);
        assert(recent[0].endCause == EndCause::Natural);
    }

    // ── Totals, over a hand-counted set ────────────────────────────────────
    {
        Track a = mkTrack("Anyma", "Genesys", "Explore",   1, "Melodic Techno", 2023);
        Track b = mkTrack("Anyma", "Genesys", "Sentience", 2, "Melodic Techno", 2023);
        Track c = mkTrack("Héroes", "Senderos", "Maldito", 1, "Rock", 1991);
        Db db;
        openWith(db, { a, b, c });

        // 3 plays of a, 2 of b, 1 of c. Two skips (both on b, by hand).
        logPlay(db, a, kBase + 0 * kDay, 200000, true,  StartCause::Manual,  EndCause::Natural);
        logPlay(db, a, kBase + 0 * kDay, 200000, true,  StartCause::Gapless, EndCause::Natural);
        logPlay(db, a, kBase + 1 * kDay, 200000, true,  StartCause::Manual,  EndCause::Natural);
        logPlay(db, b, kBase + 1 * kDay,  10000, false, StartCause::Manual,  EndCause::Next);
        logPlay(db, b, kBase + 2 * kDay,  10000, false, StartCause::Manual,  EndCause::Next);
        logPlay(db, c, kBase + 2 * kDay, 150000, true,  StartCause::Shuffle, EndCause::Natural);

        Totals tot = db.totals(StatsRange{});
        assert(tot.plays           == 6);
        assert(tot.skips           == 2);
        assert(tot.msHeard         == 200000 * 3 + 10000 * 2 + 150000);
        assert(tot.distinctTracks  == 3);
        assert(tot.distinctAlbums  == 2);   // Genesys, Senderos
        assert(tot.distinctArtists == 2);   // Anyma, Héroes
        assert(tot.activeDays      == 3);

        // Every play is judgeable here, so the rate is exactly 2/6.
        const double rate = db.skipRate(StatsRange{});
        assert(rate > 0.333 && rate < 0.334);

        // ── Rankings ──────────────────────────────────────────────────────
        auto tracks = db.topTracks(StatsRange{}, 10);
        assert(tracks.size() == 3);
        assert(tracks[0].key   == a.trackKey);   // 3 plays, the most
        assert(tracks[0].plays == 3);
        assert(tracks[0].label == "Explore");
        assert(tracks[0].subLabel == "Anyma");
        assert(tracks[1].plays == 2);            // b
        assert(tracks[2].plays == 1);            // c

        auto albums = db.topAlbums(StatsRange{}, 10);
        assert(albums.size() == 2);
        assert(albums[0].key   == "Genesys");
        assert(albums[0].plays == 5);            // a's 3 + b's 2
        assert(albums[1].key   == "Senderos");
        assert(albums[1].plays == 1);

        auto artists = db.topArtists(StatsRange{}, 10);
        assert(artists.size() == 2);
        assert(artists[0].key   == "Anyma");
        assert(artists[0].plays == 5);

        auto genres = db.topGenres(StatsRange{}, 10);
        assert(genres.size() == 2);
        assert(genres[0].key   == "Melodic Techno");
        assert(genres[0].plays == 5);
        assert(genres[1].key   == "Rock");

        // limit is honoured, and a non-positive limit returns nothing rather
        // than everything.
        assert(db.topTracks(StatsRange{}, 1).size() == 1);
        assert(db.topTracks(StatsRange{}, 0).empty());

        // ── Range filtering is half-open: from <= t < to ───────────────────
        StatsRange dayOne{ kBase, kBase + kDay };
        Totals one = db.totals(dayOne);
        assert(one.plays      == 2);            // both of day 0's plays
        assert(one.activeDays == 1);

        StatsRange openEnd{ kBase + kDay, 0 };  // day 1 onward
        assert(db.totals(openEnd).plays == 4);

        StatsRange openStart{ 0, kBase + kDay };
        assert(db.totals(openStart).plays == 2);

        // ── Daily buckets ─────────────────────────────────────────────────
        auto days = db.dailyListening(StatsRange{});
        assert(days.size() == 3);               // only days with plays
        assert(days[0].dayUnix < days[1].dayUnix);   // oldest first
        assert(days[1].dayUnix < days[2].dayUnix);
        int64_t summed = 0;
        for (const auto& d : days) summed += d.plays;
        assert(summed == 6);
        // Each bucket is a real local midnight. Whole minutes, not whole
        // hours: some zones are offset by :30 or :45.
        for (const auto& d : days)
            assert(d.dayUnix % 60 == 0);
    }

    // ── The same track held twice must not count twice ─────────────────────
    // trackKey() merges a standard edition with its deluxe, and a 16-44 copy
    // with its 24-96 — deliberately, because they are the same music. The
    // direct consequence is that `tracks` holds SEVERAL rows per key, and a
    // plain join from play_events fans out and counts one listen once per copy
    // on disk. On a real 43-play library that read as 52.
    //
    // Every ranking has to go through one representative row per key. This is
    // the case the first version of this test missed, because its fixture only
    // ever put one tracks row behind each key.
    {
        Track std_ = mkTrack("A", "Rec",          "Song", 1, "Rock", 1991);
        Track dlx  = mkTrack("A", "Rec (Deluxe)", "Song", 1, "Rock", 1991);
        // Different files, different album tags — but ONE identity, which is
        // the whole point of the key.
        assert(std_.trackKey == dlx.trackKey);
        assert(std_.filePath != dlx.filePath);

        // A third copy at another quality, to prove this is not a two-row
        // special case.
        Track hi = mkTrack("A", "Rec", "Song", 1, "Rock", 1991);
        hi.filePath = "/m/A/Rec (24-96)/1 Song.flac";
        assert(hi.trackKey == std_.trackKey);

        Db db;
        openWith(db, { std_, dlx, hi });

        // Exactly three listens, all of the same music.
        logPlay(db, std_, kBase + 1, 200000, true, StartCause::Manual, EndCause::Natural);
        logPlay(db, dlx,  kBase + 2, 200000, true, StartCause::Manual, EndCause::Natural);
        logPlay(db, hi,   kBase + 3, 200000, true, StartCause::Manual, EndCause::Natural);

        Totals tot = db.totals(StatsRange{});
        assert(tot.plays          == 3);
        assert(tot.distinctTracks == 1);
        assert(tot.distinctAlbums == 1);   // not 2, and not 3
        assert(tot.distinctArtists == 1);

        auto tracks = db.topTracks(StatsRange{}, 10);
        assert(tracks.size()   == 1);
        assert(tracks[0].plays == 3);      // fan-out would say 9

        auto albums = db.topAlbums(StatsRange{}, 10);
        assert(albums.size()   == 1);      // one release, not one row per copy
        assert(albums[0].plays == 3);

        auto artists = db.topArtists(StatsRange{}, 10);
        assert(artists.size()   == 1);
        assert(artists[0].plays == 3);

        auto genres = db.topGenres(StatsRange{}, 10);
        assert(genres.size()   == 1);
        assert(genres[0].plays == 3);

        // No ranking may ever total more listens than the log holds.
        int64_t summed = 0;
        for (const auto& e : db.topAlbums(StatsRange{}, 100)) summed += e.plays;
        assert(summed == tot.plays);
    }

    // ── What counts as a skip, and what is not a verdict at all ────────────
    // The denominator matters as much as the numerator here: a listener who
    // closes the app mid-song has not rejected the song, and letting that
    // count as "not skipped" would quietly deflate everyone's skip rate.
    {
        Track t = mkTrack("A", "Rec", "Song", 1);
        Db db;
        openWith(db, { t });

        // Judged, and skipped: left early by the listener's own hand. All
        // three buttons are the same act.
        logPlay(db, t, kBase + 1, 5000, false, StartCause::Manual, EndCause::Next);
        logPlay(db, t, kBase + 2, 5000, false, StartCause::Manual, EndCause::Prev);
        logPlay(db, t, kBase + 3, 5000, false, StartCause::Manual, EndCause::Replaced);
        // Judged, not skipped.
        logPlay(db, t, kBase + 4, 200000, true, StartCause::Manual, EndCause::Natural);
        // Not judged at all: facts about the listener's evening, not the track.
        logPlay(db, t, kBase + 5, 5000, false, StartCause::Manual, EndCause::Stop);
        logPlay(db, t, kBase + 6, 5000, false, StartCause::Manual, EndCause::AppExit);

        Totals tot = db.totals(StatsRange{});
        assert(tot.plays == 6);      // every listen is still recorded
        assert(tot.skips == 3);
        // 3 skips out of 4 judgeable — the Stop and the AppExit are in neither
        // half of the fraction. Counted as "not skipped" it would read 3/6.
        const double rate = db.skipRate(StatsRange{});
        assert(rate == 0.75);

        // A completed play is never a skip, whatever ended it.
        assert(db.trackTotals(t.trackKey).skipCount == 3);
    }

    // ── Deleting a track keeps its plays, loses only its label ─────────────
    {
        Track t = mkTrack("A", "Rec", "Song", 1);
        Db db;
        openWith(db, { t });
        logPlay(db, t, kBase, 200000, true, StartCause::Manual, EndCause::Natural);
        db.removeTracksByPaths({ t.filePath });

        assert(db.totals(StatsRange{}).plays == 1);
        assert(db.trackTotals(t.trackKey).playCount == 1);
        auto top = db.topTracks(StatsRange{}, 10);
        assert(top.size() == 1);
        assert(top[0].key   == t.trackKey);   // still ranked
        assert(top[0].plays == 1);
        assert(top[0].label.empty());         // but no longer named
        // The album ranking needs the join, so it can no longer see this play.
        // Stated here so the limitation is pinned rather than discovered.
        assert(db.topAlbums(StatsRange{}, 10).empty());
    }

    // ── The hour histogram is LOCAL, and the offset is what makes it so ────
    // Two plays at the SAME UTC instant, with different stored offsets. If the
    // local hour were recomputed from UTC instead of read from the event, both
    // would land in one bucket. This is the assertion that proves storing the
    // offset per event was worth doing — and it is the case daylight saving
    // and a change of timezone both reduce to.
    {
        const std::string path = "stats_test_hours.db";
        remove(path.c_str());

        Track t = mkTrack("A", "Rec", "Song", 1);
        const int64_t midnightUtc = kBase - (kBase % kDay);   // exactly 00:00 UTC
        {
            Db db;
            assert(db.open(path));
            db.saveTracks({ t });
            logPlay(db, t, midnightUtc, 200000, true,
                    StartCause::Manual, EndCause::Natural);
            logPlay(db, t, midnightUtc, 200000, true,
                    StartCause::Manual, EndCause::Natural);
        }

        // beginPlayEvent always stamps THIS machine's offset, and the point
        // here is to vary it — so the two rows are rewritten directly: one in
        // UTC, one five hours east.
        execRaw(path, "UPDATE play_events SET utc_offset_min = 0   WHERE id = 1;"
                      "UPDATE play_events SET utc_offset_min = 300 WHERE id = 2;");

        {
            Db db;
            assert(db.open(path));
            auto hours = db.hourHistogram(StatsRange{});
            assert(hours.size() == 24);        // every bucket, gaps included
            for (int h = 0; h < 24; h++) assert(hours[(size_t)h].hour == h);
            assert(hours[0].plays == 1);       // midnight for the UTC listener
            assert(hours[5].plays == 1);       // 05:00 for the one at UTC+5
            int64_t summed = 0;
            for (const auto& b : hours) summed += b.plays;
            assert(summed == 2);

            // Same split shows up in the daily buckets: the UTC+5 listener's
            // local midnight is five hours earlier in absolute terms.
            auto days = db.dailyListening(StatsRange{});
            assert(days.size() == 2);
            assert(days[1].dayUnix - days[0].dayUnix == 5 * 3600);
        }

        remove(path.c_str());
        remove((path + "-wal").c_str());
        remove((path + "-shm").c_str());
    }

    // ── Migration from the old play_history log ────────────────────────────
    // Built by hand in the shape the schema had BEFORE this work: tracks and
    // play_history, user_version 0, no play_events. Then opened through the
    // real Db, which must migrate it exactly once.
    {
        const std::string path = "stats_test_migration.db";
        remove(path.c_str());

        {
            sqlite3* raw = nullptr;
            assert(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
            const char* v1 =
                "CREATE TABLE tracks ("
                "  id INTEGER PRIMARY KEY AUTOINCREMENT, title TEXT, artist TEXT,"
                "  album_artist TEXT, album TEXT, file_path TEXT UNIQUE,"
                "  track_number INTEGER DEFAULT 0, disc_number INTEGER DEFAULT 0,"
                "  duration_ms INTEGER, sample_rate INTEGER, channels INTEGER,"
                "  bit_depth INTEGER, file_size INTEGER, file_mtime INTEGER DEFAULT 0);"
                "CREATE TABLE play_history ("
                "  id INTEGER PRIMARY KEY AUTOINCREMENT, file_path TEXT NOT NULL,"
                "  played_at INTEGER NOT NULL);"
                "INSERT INTO tracks (title, artist, album_artist, album, file_path,"
                "                    track_number, disc_number, duration_ms, file_mtime)"
                "  VALUES ('Song','A','A','Rec','/m/A/Rec/1 Song.flac',1,1,200000,999);"
                "INSERT INTO play_history (file_path, played_at) VALUES"
                "  ('/m/A/Rec/1 Song.flac', 1690000000),"
                "  ('/m/A/Rec/1 Song.flac', 1690001000),"
                "  ('/m/gone/vanished.flac', 1690002000);";
            assert(sqlite3_exec(raw, v1, nullptr, nullptr, nullptr) == SQLITE_OK);
            sqlite3_close(raw);
        }

        {
            Db db;
            assert(db.open(path));

            // Every history row came across — including the orphan, which has
            // no track to join to and must NOT have been silently dropped.
            Totals tot = db.totals(StatsRange{});
            assert(tot.plays == 3);

            // The two attributable ones found their track's key.
            auto tracks = db.loadTracks();
            assert(tracks.size() == 1);
            assert(!tracks[0].trackKey.empty());
            assert(db.trackTotals(tracks[0].trackKey).playCount == 2);

            // Migrated rows record no verdict, so nothing may be inferred
            // from them: they are neither skips nor completions.
            assert(tot.skips == 0);
            assert(db.skipRate(StatsRange{}) == 0.0);

            // The genre/year backfill zeroed the scan cache, buying exactly
            // one full re-parse on the next scan.
            assert(tracks[0].fileMtime == 0);
        }

        // The orphan kept its place rather than being destroyed.
        assert(scalar(path, "SELECT COUNT(*) FROM play_events WHERE track_key = '';") == 1);
        // The old log is still on disk as the only checkable record of what
        // was migrated.
        assert(scalar(path, "SELECT COUNT(*) FROM play_history;") == 3);
        // Tracks kSchemaVersion in db.cpp deliberately: a stale stamp here is
        // how you notice you added a step without deciding what re-running it
        // would do.
        assert(scalar(path, "PRAGMA user_version;") == 4);

        // ── Re-opening must NOT migrate again ──────────────────────────────
        // This is the whole reason user_version exists: a second pass would
        // duplicate the entire listening history on every launch.
        for (int pass = 0; pass < 3; pass++) {
            Db db;
            assert(db.open(path));
            assert(db.totals(StatsRange{}).plays == 3);
        }
        assert(scalar(path, "SELECT COUNT(*) FROM play_events;") == 3);

        remove(path.c_str());
        remove((path + "-wal").c_str());
        remove((path + "-shm").c_str());
    }

    // ── A fresh database is stamped current and migrates nothing ───────────
    {
        const std::string path = "stats_test_fresh.db";
        remove(path.c_str());
        {
            Db db;
            assert(db.open(path));
            assert(db.totals(StatsRange{}).plays == 0);
        }
        assert(scalar(path, "PRAGMA user_version;") == 4);
        remove(path.c_str());
        remove((path + "-wal").c_str());
        remove((path + "-shm").c_str());
    }

    // ── An event left open by a crash is closed on the next open ───────────
    {
        const std::string path = "stats_test_crash.db";
        remove(path.c_str());

        Track t = mkTrack("A", "Rec", "Song", 1);
        {
            Db db;
            assert(db.open(path));
            db.saveTracks({ t });
            // Opened and never closed — the app died here.
            assert(db.beginPlayEvent(t.trackKey, t.filePath, t.durationMs,
                                     StartCause::Manual, kBase) > 0);
        }
        assert(scalar(path, "SELECT COUNT(*) FROM play_events WHERE ended_at IS NULL;") == 1);

        {
            Db db;
            assert(db.open(path));
            auto recent = db.recentlyPlayed(1);
            assert(recent.size() == 1);
            assert(recent[0].endedAt > 0);
            assert(recent[0].endCause == EndCause::AppExit);
            // An app exit is not a verdict on the music: it counts as neither
            // a skip nor a completion, so it cannot deflate the skip rate.
            assert(db.totals(StatsRange{}).skips == 0);
            assert(db.skipRate(StatsRange{}) == 0.0);
        }
        assert(scalar(path, "SELECT COUNT(*) FROM play_events WHERE ended_at IS NULL;") == 0);

        remove(path.c_str());
        remove((path + "-wal").c_str());
        remove((path + "-shm").c_str());
    }

    // ── Local offset: the helper agrees with the C library ─────────────────
    {
        // Whatever this machine's zone is, applying the reported offset to UTC
        // must reproduce localtime's own breakdown of the same instant.
        const int off = localUtcOffsetMinutes(kBase);
        time_t at = (time_t)kBase;
        std::tm lo{};
#ifdef _WIN32
        localtime_s(&lo, &at);
#else
        localtime_r(&at, &lo);
#endif
        const int64_t shifted = kBase + (int64_t)off * 60;
        std::tm gm{};
        time_t sh = (time_t)shifted;
#ifdef _WIN32
        gmtime_s(&gm, &sh);
#else
        gmtime_r(&sh, &gm);
#endif
        assert(gm.tm_hour == lo.tm_hour);
        assert(gm.tm_min  == lo.tm_min);
        assert(gm.tm_mday == lo.tm_mday);
    }

    // ── Queries are safe on an empty database ──────────────────────────────
    {
        Db db;
        openWith(db, {});
        assert(db.totals(StatsRange{}).plays == 0);
        assert(db.topTracks(StatsRange{}, 10).empty());
        assert(db.topAlbums(StatsRange{}, 10).empty());
        assert(db.topArtists(StatsRange{}, 10).empty());
        assert(db.topGenres(StatsRange{}, 10).empty());
        assert(db.dailyListening(StatsRange{}).empty());
        assert(db.recentlyPlayed(10).empty());
        assert(db.hourHistogram(StatsRange{}).size() == 24);
        assert(db.skipRate(StatsRange{}) == 0.0);
        assert(db.trackTotals("nothing").playCount == 0);
        assert(db.trackTotals("").playCount == 0);
    }

    // ── An existing install's assigned profile seeds the inventory ─────────
    // Without step_seedEqHeadphones an upgrading listener meets an empty
    // switcher and has to re-pick the pair they were already using. Seeded
    // PINNED, because the prune has no evidence of when they last used it and
    // must not be free to evict a deliberate choice.
    {
        const std::string path = "stats_test_eqseed.db";
        remove(path.c_str());
        {
            // A pre-analytics database: tracks exists (so open() treats it as
            // pre-existing and runs the steps), user_version 0.
            sqlite3* raw = nullptr;
            assert(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
            const char* v1 =
                "CREATE TABLE tracks (id INTEGER PRIMARY KEY AUTOINCREMENT,"
                " file_path TEXT UNIQUE, title TEXT, artist TEXT, album TEXT,"
                " duration_ms INTEGER, sample_rate INTEGER, bit_depth INTEGER,"
                " channels INTEGER, format TEXT, file_size INTEGER, disc_number INTEGER);"
                "CREATE TABLE eq_assignments (device_key TEXT PRIMARY KEY,"
                " profile_name TEXT NOT NULL, profile_source TEXT DEFAULT '',"
                " profile_form TEXT DEFAULT '');"
                "INSERT INTO eq_assignments VALUES ('32BB:0004','HD 650','oratory1990','over-ear');";
            assert(sqlite3_exec(raw, v1, nullptr, nullptr, nullptr) == SQLITE_OK);
            sqlite3_close(raw);
        }
        {
            Db db;
            assert(db.open(path));
            auto hp = db.loadEqHeadphones(10);
            assert(hp.size() == 1);
            assert(hp[0].name   == "HD 650");
            assert(hp[0].source == "oratory1990");
            assert(hp[0].form   == "over-ear");
            assert(hp[0].pinned);
        }
        // Re-opening must not duplicate it — the version guard, not the
        // INSERT OR IGNORE, is what has to be doing the work here.
        for (int pass = 0; pass < 3; pass++) {
            Db db;
            assert(db.open(path));
        }
        assert(scalar(path, "SELECT COUNT(*) FROM eq_headphones;") == 1);
        remove(path.c_str());
        remove((path + "-wal").c_str());
        remove((path + "-shm").c_str());
    }

    // ── Headphone inventory: a credit must never clobber pinned/use_count ──
    // creditEqHeadphone() is two statements rather than INSERT OR REPLACE for
    // exactly this reason. A replace would silently unpin a pair every time it
    // played, and the listener would only notice when the prune ate it.
    {
        Db db;
        openWith(db, {});
        assert(db.loadEqHeadphones(10).empty());

        db.creditEqHeadphone("HD 600", "oratory1990", "over-ear", kBase);
        auto hp = db.loadEqHeadphones(10);
        assert(hp.size() == 1);
        assert(hp[0].name     == "HD 600");
        assert(hp[0].useCount == 1);
        assert(hp[0].lastUsed == kBase);
        assert(!hp[0].pinned);

        db.setEqHeadphonePinned("HD 600", "oratory1990", "over-ear", true);
        db.creditEqHeadphone("HD 600", "oratory1990", "over-ear", kBase + kDay);
        hp = db.loadEqHeadphones(10);
        assert(hp.size() == 1);               // still one row, not a duplicate
        assert(hp[0].useCount == 2);
        assert(hp[0].lastUsed == kBase + kDay);
        assert(hp[0].pinned);                 // survived the credit

        // Same name, different source/form is a DIFFERENT pair: the primary
        // key is all three, because two measurements of one model are two
        // profiles and picking between them is the whole point of the list.
        db.creditEqHeadphone("HD 600", "crinacle", "over-ear", kBase);
        assert(db.loadEqHeadphones(10).size() == 2);

        db.removeEqHeadphone("HD 600", "crinacle", "over-ear");
        assert(db.loadEqHeadphones(10).size() == 1);
    }

    // ── The prune keeps the list readable, and pinned rows are exempt ──────
    {
        Db db;
        openWith(db, {});
        // Two pinned pairs plus 20 casual ones. The prune budget is 12
        // unpinned rows; pinned rows must not be counted against it, or
        // pinning a set could evict the pair playing right now.
        db.creditEqHeadphone("Keeper A", "", "", kBase);
        db.creditEqHeadphone("Keeper B", "", "", kBase);
        db.setEqHeadphonePinned("Keeper A", "", "", true);
        db.setEqHeadphonePinned("Keeper B", "", "", true);
        for (int i = 0; i < 20; i++)
            db.creditEqHeadphone("Casual " + std::to_string(i), "", "",
                                 kBase + kDay + i);

        auto hp = db.loadEqHeadphones(100);
        assert(hp.size() == 14);              // 2 pinned + 12 unpinned survivors

        // Pinned first, then most-recent-first — the sidebar reads this order
        // straight off the query.
        assert(hp[0].pinned && hp[1].pinned);
        assert(hp[2].name == "Casual 19");
        assert(hp[13].name == "Casual 8");

        // The oldest casual entries are the ones that went.
        for (const auto& h : hp) assert(h.name != "Casual 0");
    }

    printf("stats_test: all assertions passed\n");
    return 0;
}

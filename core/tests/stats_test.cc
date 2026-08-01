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
        assert(s.firstPlayed  == kBase);
        assert(s.lastPlayed   == kBase);

        auto recent = db.recentlyPlayed(StatsRange{}, 10);
        assert(recent.size() == 1);
        assert(recent[0].trackKey   == t.trackKey);
        assert(recent[0].filePath   == t.filePath);
        assert(recent[0].title      == "Explore");
        assert(recent[0].artist     == "Anyma");
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
        auto recent = db.recentlyPlayed(StatsRange{}, 1);
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
        // An album's identity is title AND artist, joined by U+001F. The two
        // halves come back split for display as well.
        assert(albums[0].key      == std::string("Genesys\x1f") + "Anyma");
        assert(albums[0].label    == "Genesys");
        assert(albums[0].subLabel == "Anyma");
        assert(albums[0].plays    == 5);         // a's 3 + b's 2
        assert(albums[1].label    == "Senderos");
        assert(albums[1].plays    == 1);

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
        assert(days[0].dayLocal < days[1].dayLocal);   // oldest first
        assert(days[1].dayLocal < days[2].dayLocal);
        int64_t summed = 0;
        for (const auto& d : days) summed += d.plays;
        assert(summed == 6);
        // Each bucket is a local midnight expressed on the LOCAL clock, so it
        // is a whole number of days — not a whole number of minutes, which is
        // all a UTC instant of local midnight could promise in a :30 or :45
        // zone. That exactness is what makes the calendar helpers division.
        for (const auto& d : days)
            assert(d.dayLocal % kDay == 0);

        // ── Sessions ──────────────────────────────────────────────────────
        // Six plays spread over three days, each day a run of its own: with a
        // 30-minute gap there is no way for one day's listening to join the
        // next, so this must read as three sessions.
        SessionStats sess = db.sessions(StatsRange{});
        assert(sess.count   == 3);
        assert(sess.plays   == 6);
        assert(sess.msHeard == tot.msHeard);
        // A range narrows sessions the same way it narrows everything else.
        assert(db.sessions(dayOne).count == 1);
        assert(db.sessions(dayOne).plays == 2);
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

        // The history says the same thing, for the same reason: the row is
        // still there, unnamed. recentlyPlayed() resolving its labels through
        // a LEFT JOIN and not a plain one is what keeps this list agreeing
        // with the ranking above it.
        auto recent = db.recentlyPlayed(StatsRange{}, 10);
        assert(recent.size() == 1);
        assert(recent[0].trackKey == t.trackKey);
        assert(recent[0].title.empty());
        assert(recent[0].artist.empty());
    }

    // ── Two albums may share a title; they are not the same album ──────────
    // Grouping a ranking on the title alone merged them into one row, and the
    // artist column then read as whichever of the two MAX() happened to pick —
    // a wrong count under a misleading name. The identity is title AND artist.
    {
        Track x = mkTrack("Anyma",  "Live", "One", 1);
        Track y = mkTrack("Héroes", "Live", "Two", 1);
        assert(x.trackKey != y.trackKey);

        Db db;
        openWith(db, { x, y });
        logPlay(db, x, kBase + 1, 200000, true, StartCause::Manual, EndCause::Natural);
        logPlay(db, x, kBase + 2, 200000, true, StartCause::Manual, EndCause::Natural);
        logPlay(db, y, kBase + 3, 200000, true, StartCause::Manual, EndCause::Natural);

        auto albums = db.topAlbums(StatsRange{}, 10);
        assert(albums.size() == 2);                     // not one merged row
        assert(albums[0].label == "Live" && albums[0].subLabel == "Anyma");
        assert(albums[0].plays == 2);
        assert(albums[1].label == "Live" && albums[1].subLabel == "Héroes");
        assert(albums[1].plays == 1);
        assert(albums[0].key != albums[1].key);         // and they are distinct

        // The same invariant the fan-out case asserts, from the other side: a
        // ranking may never total more listens than the log holds.
        int64_t summed = 0;
        for (const auto& e : albums) summed += e.plays;
        assert(summed == db.totals(StatsRange{}).plays);
    }

    // ── "Most played" and "most listened to" are different questions ───────
    // A track heard once from end to end can outweigh one started three times
    // and abandoned. Ordering only by play count cannot say so.
    {
        Track longT  = mkTrack("A", "Rec", "Long",  1, "", 0, 600000);
        Track shortT = mkTrack("A", "Rec", "Short", 2, "", 0,  60000);
        Db db;
        openWith(db, { longT, shortT });

        logPlay(db, longT,  kBase + 1, 600000, true, StartCause::Manual, EndCause::Natural);
        for (int i = 0; i < 3; i++)
            logPlay(db, shortT, kBase + 10 + i, 60000, true,
                    StartCause::Manual, EndCause::Natural);

        auto byPlays = db.topTracks(StatsRange{}, 10, TopSort::Plays);
        assert(byPlays.size() == 2);
        assert(byPlays[0].label == "Short" && byPlays[0].plays == 3);
        assert(byPlays[1].label == "Long");

        auto byTime = db.topTracks(StatsRange{}, 10, TopSort::TimeHeard);
        assert(byTime.size() == 2);
        assert(byTime[0].label   == "Long");            // 10 min beats 3 min
        assert(byTime[0].msHeard == 600000);
        assert(byTime[1].label   == "Short");
        // Both measures come back either way, so a caller can flip the sort
        // without a second query.
        assert(byPlays[0].msHeard == 180000);

        // Plays is the default, and the other rankings honour the flag too.
        assert(db.topTracks(StatsRange{}, 10)[0].label == "Short");
        assert(db.topGenres(StatsRange{}, 10, TopSort::TimeHeard).empty());  // no genres set
    }

    // ── Sessions split on a gap and only on a gap ──────────────────────────
    {
        Track t = mkTrack("A", "Rec", "Song", 1);
        Db db;
        openWith(db, { t });

        // Three plays back to back (each 200 s heard, starting 210 s apart:
        // a 10-second gap, far inside the 30-minute threshold), then a two-
        // hour silence, then two more.
        for (int i = 0; i < 3; i++)
            logPlay(db, t, kBase + i * 210, 200000, true,
                    StartCause::Manual, EndCause::Natural);
        const int64_t after = kBase + 2 * 3600;
        for (int i = 0; i < 2; i++)
            logPlay(db, t, after + i * 210, 200000, true,
                    StartCause::Manual, EndCause::Natural);

        SessionStats s = db.sessions(StatsRange{});
        assert(s.count   == 2);
        assert(s.plays   == 5);
        assert(s.msHeard == 200000 * 5);
        // First run: starts at kBase, last play ends 2*210 + 200 s later.
        assert(s.longestMs == (2 * 210 + 200) * 1000);
        assert(s.spanMs    == (2 * 210 + 200) * 1000 + (210 + 200) * 1000);

        // The threshold is the knob, and it is the only thing separating one
        // reading from the other: widen it past two hours and the whole
        // evening is one session.
        assert(db.sessions(StatsRange{}, 3 * 3600).count == 1);
        // Narrow it below the 10-second gap and every play stands alone.
        assert(db.sessions(StatsRange{}, 0).count == 5);
    }

    // ── First and last play come out of the counter scan ───────────────────
    {
        Track t = mkTrack("A", "Rec", "Song", 1);
        Db db;
        openWith(db, { t });
        logPlay(db, t, kBase + 5 * kDay, 200000, true, StartCause::Manual, EndCause::Natural);
        logPlay(db, t, kBase + 1 * kDay, 200000, true, StartCause::Manual, EndCause::Natural);
        logPlay(db, t, kBase + 3 * kDay, 200000, true, StartCause::Manual, EndCause::Natural);

        TrackStats s = db.trackTotals(t.trackKey);
        assert(s.playCount   == 3);
        assert(s.firstPlayed == kBase + 1 * kDay);   // earliest, not first logged
        assert(s.lastPlayed  == kBase + 5 * kDay);
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

            // The DAILY buckets must NOT split the same way. Both plays fall
            // on the same date of the listener's calendar — one at 00:00, one
            // at 05:00 — so they are one day, whatever the offset was.
            //
            // This is the assertion an earlier version of this test had
            // backwards: grouping on the UTC instant of local midnight moved
            // that instant with the offset and produced two buckets five hours
            // apart, which is what a daylight-saving Sunday would have looked
            // like — one day counted twice, in dailyListening and in
            // activeDays alike.
            auto days = db.dailyListening(StatsRange{});
            assert(days.size() == 1);
            assert(days[0].plays == 2);
            assert(days[0].dayLocal % kDay == 0);
            assert(db.totals(StatsRange{}).activeDays == 1);
        }

        remove(path.c_str());
        remove((path + "-wal").c_str());
        remove((path + "-shm").c_str());
    }

    // ── ...but a genuine change of local DATE still splits ─────────────────
    // The fix above must not have flattened every offset difference into one
    // bucket. Same trick, one instant late in the UTC day: for the listener at
    // UTC the date is still today, for the one five hours east it is already
    // tomorrow. Two calendar days, two buckets.
    {
        const std::string path = "stats_test_days.db";
        remove(path.c_str());

        Track t = mkTrack("A", "Rec", "Song", 1);
        const int64_t lateUtc = kBase - (kBase % kDay) + 22 * 3600;   // 22:00 UTC
        {
            Db db;
            assert(db.open(path));
            db.saveTracks({ t });
            logPlay(db, t, lateUtc, 200000, true, StartCause::Manual, EndCause::Natural);
            logPlay(db, t, lateUtc, 200000, true, StartCause::Manual, EndCause::Natural);
        }
        execRaw(path, "UPDATE play_events SET utc_offset_min = 0   WHERE id = 1;"
                      "UPDATE play_events SET utc_offset_min = 300 WHERE id = 2;");
        {
            Db db;
            assert(db.open(path));
            auto days = db.dailyListening(StatsRange{});
            assert(days.size() == 2);
            assert(days[1].dayLocal - days[0].dayLocal == kDay);   // consecutive
            assert(days[0].plays == 1 && days[1].plays == 1);
            assert(db.totals(StatsRange{}).activeDays == 2);
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

            // And they generate no playlist. IS_AFFINITY needs completed = 1,
            // which the old play_history never recorded, so migrated history
            // is excluded here with no clause of its own — worth pinning,
            // because the alternative (inferring completion) would invent a
            // taste profile out of rows that never claimed one.
            assert(db.heavyRotation(StatsRange{}, 10).empty());
            assert(db.forgottenFavourites(10, 1, 2000000000).empty());

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
            auto recent = db.recentlyPlayed(StatsRange{}, 1);
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

    // ── A generated playlist must never feed itself ────────────────────────
    // THE assertion this whole feature rests on. A play that came OUT of a
    // playlist may not count toward the ranking that built it: otherwise the
    // top track earns another count every time it plays BECAUSE it is at the
    // top, and nothing below it can ever overtake it. A track earns its place
    // by being chosen elsewhere — the grid, an album — or it does not earn it.
    {
        Track top    = mkTrack("A", "Rec", "Top",    1);
        Track second = mkTrack("A", "Rec", "Second", 2);
        Db db;
        openWith(db, { top, second });

        logPlay(db, top,    kBase + 1, 200000, true, StartCause::Manual, EndCause::Natural);
        logPlay(db, top,    kBase + 2, 200000, true, StartCause::Manual, EndCause::Natural);
        logPlay(db, top,    kBase + 3, 200000, true, StartCause::Manual, EndCause::Natural);
        logPlay(db, second, kBase + 4, 200000, true, StartCause::Manual, EndCause::Natural);
        logPlay(db, second, kBase + 5, 200000, true, StartCause::Manual, EndCause::Natural);

        auto before = db.heavyRotation(StatsRange{}, 10);
        assert(before.size()   == 2);
        assert(before[0].label == "Top"    && before[0].plays == 3);
        assert(before[1].label == "Second" && before[1].plays == 2);

        // Now play the list itself, hard. Ten complete listens of the leader —
        // exactly what happens when someone leaves Heavy Rotation running.
        for (int i = 0; i < 10; i++)
            logPlay(db, top, kBase + 100 + i, 200000, true,
                    StartCause::Playlist, EndCause::Natural);

        auto after = db.heavyRotation(StatsRange{}, 10);
        assert(after.size()   == 2);
        assert(after[0].label == "Top"    && after[0].plays == 3);   // NOT 13
        assert(after[1].label == "Second" && after[1].plays == 2);

        // Second could still overtake by being chosen deliberately — which is
        // the whole point of excluding the playlist plays.
        for (int i = 0; i < 2; i++)
            logPlay(db, second, kBase + 200 + i, 200000, true,
                    StartCause::Manual, EndCause::Natural);
        auto flipped = db.heavyRotation(StatsRange{}, 10);
        assert(flipped[0].label == "Second" && flipped[0].plays == 4);
        assert(flipped[1].label == "Top"    && flipped[1].plays == 3);

        // The rows were still WRITTEN. Not counted is not discarded — the log
        // keeps every listen, and reconsidering this rule later costs nothing.
        assert(db.totals(StatsRange{}).plays == 17);
        assert(db.trackTotals(top.trackKey).playCount == 13);

        // And skipRate() is untouched by the new cause: skipping inside a
        // playlist is still skipping. IS_SKIP did not change, and this is what
        // keeps it that way.
        const double rate = db.skipRate(StatsRange{});
        assert(rate == 0.0);
    }

    // ── Half a track is not liking it ──────────────────────────────────────
    {
        Track half = mkTrack("A", "Rec", "Half", 1);
        Track full = mkTrack("A", "Rec", "Full", 2);
        Db db;
        openWith(db, { half, full });

        // Twenty listens that stopped at the midpoint. Enthusiasm, maybe;
        // completion, no.
        for (int i = 0; i < 20; i++)
            logPlay(db, half, kBase + i, 100000, false,
                    StartCause::Manual, EndCause::Next);
        assert(db.heavyRotation(StatsRange{}, 10).empty());

        // One that ran to the end outranks all twenty.
        logPlay(db, full, kBase + 50, 200000, true, StartCause::Manual, EndCause::Natural);
        auto top = db.heavyRotation(StatsRange{}, 10);
        assert(top.size()   == 1);
        assert(top[0].label == "Full");
        assert(top[0].plays == 1);
    }

    // ── Forgotten favourites: loved once, untouched since ──────────────────
    {
        const int64_t cutoff = kBase + 100 * kDay;
        Track old_  = mkTrack("A", "Rec", "Old",   1);
        Track fresh = mkTrack("A", "Rec", "Fresh", 2);
        Track thin  = mkTrack("A", "Rec", "Thin",  3);
        Db db;
        openWith(db, { old_, fresh, thin });

        for (int i = 0; i < 5; i++)   // loved, long ago
            logPlay(db, old_,  kBase + i * kDay, 200000, true,
                    StartCause::Manual, EndCause::Natural);
        for (int i = 0; i < 5; i++)   // loved, and played last week
            logPlay(db, fresh, kBase + (150 + i) * kDay, 200000, true,
                    StartCause::Manual, EndCause::Natural);
        for (int i = 0; i < 2; i++)   // long ago, but never really loved
            logPlay(db, thin,  kBase + i * kDay, 200000, true,
                    StartCause::Manual, EndCause::Natural);

        auto f = db.forgottenFavourites(10, /*minPlays=*/3, cutoff);
        assert(f.size()   == 1);
        assert(f[0].label == "Old");
        assert(f[0].plays == 5);

        // Lower the bar and the thin one qualifies too; raise the cutoff past
        // everything and nothing is forgotten yet.
        assert(db.forgottenFavourites(10, 2, cutoff).size() == 2);
        assert(db.forgottenFavourites(10, 3, kBase).empty());

        // A play from a playlist does not make a track "loved" again — but it
        // DOES make it recently touched, so it stops being forgotten. The
        // cutoff runs on every event; only the count is filtered.
        logPlay(db, old_, kBase + 160 * kDay, 200000, true,
                StartCause::Playlist, EndCause::Natural);
        assert(db.forgottenFavourites(10, 3, cutoff).empty());
    }

    // ── Never heard: what the log cannot answer ────────────────────────────
    {
        Track a   = mkTrack("A", "Rec", "Alpha", 1);
        Track b   = mkTrack("A", "Rec", "Bravo", 2);
        Track dup = mkTrack("A", "Rec (Deluxe)", "Alpha", 1);   // same identity
        Track hi  = mkTrack("A", "Rec", "Alpha", 1);
        hi.filePath = "/m/A/Rec (24-96)/1 Alpha.flac";
        assert(a.trackKey == dup.trackKey && a.trackKey == hi.trackKey);

        Db db;
        openWith(db, { a, b, dup, hi });

        // Nothing played: two identities, not four rows.
        auto never = db.neverHeard(0);
        assert(never.size() == 2);
        assert(never[0].label == "Alpha");
        assert(never[1].label == "Bravo");

        // A single partial listen is enough to disqualify: "I have never heard
        // this" stops being true the first time it plays at all.
        logPlay(db, a, kBase, 5000, false, StartCause::Manual, EndCause::Next);
        never = db.neverHeard(0);
        assert(never.size()   == 1);
        assert(never[0].label == "Bravo");

        // limit <= 0 is unlimited; a positive limit caps.
        assert(db.neverHeard(0).size() == 1);
        assert(db.neverHeard(1).size() == 1);
        logPlay(db, b, kBase + 1, 5000, false, StartCause::Manual, EndCause::Next);
        assert(db.neverHeard(0).empty());
    }

    // ── Calendar arithmetic (core/src/stats.cpp) ───────────────────────────
    // Pure integer math, so these answers are the same in every zone and on
    // every platform — which is the reason it does not go through <ctime>.
    {
        auto civil = [](int64_t dayLocal) {
            int y = 0, m = 0, d = 0;
            statsCivilFromLocalDay(dayLocal, y, m, d);
            return y * 10000 + m * 100 + d;
        };
        assert(civil(0)           == 19700101);
        assert(statsWeekdayFromLocalDay(0) == 4);        // 1970-01-01 was a Thursday
        assert(civil(19675 * kDay) == 20231114);
        assert(statsWeekdayFromLocalDay(19675 * kDay) == 2);   // a Tuesday
        // The leap day, and the day after it — the case a naive 365-day
        // calculation gets wrong.
        assert(civil(19782 * kDay) == 20240229);
        assert(civil(19783 * kDay) == 20240301);
        assert(statsWeekdayFromLocalDay(19782 * kDay) == 4);
        // A leap SECOND year's end, and a Saturday, to pin the week's phase
        // from the other side of the fold.
        assert(civil(17166 * kDay) == 20161231);
        assert(statsWeekdayFromLocalDay(17166 * kDay) == 6);
        // Before the epoch the floor must round DOWN, not toward zero.
        assert(civil(-1 * kDay) == 19691231);
        assert(statsWeekdayFromLocalDay(-1 * kDay) == 3);      // a Wednesday
    }

    // ── Range presets land on LOCAL midnight ───────────────────────────────
    // Not "now minus 7*86400": the answer to "the last seven days" must not
    // shift by an hour because the clocks changed inside the window, and it
    // must not start mid-afternoon.
    {
        const int off = -300;                 // UTC-5, this library's own zone
        // kBase is 2023-11-14 22:13:20 UTC, which is 17:13 local at UTC-5.
        StatsRange r7 = rangeFor(RangePreset::Last7Days, kBase, off);
        assert(r7.fromUnix == 1699419600);    // local midnight, 2023-11-08
        assert(r7.toUnix   == 0);             // open: what plays next counts too
        // Seven calendar days INCLUDING today, so six day-lengths back from
        // this morning's midnight.
        assert((r7.fromUnix + (int64_t)off * 60) % kDay == 0);

        StatsRange r30 = rangeFor(RangePreset::Last30Days, kBase, off);
        assert(r30.fromUnix == 1697432400);
        assert(r7.fromUnix - r30.fromUnix == 23 * kDay);

        // What "On Repeat" means: recent taste, not lifetime taste.
        StatsRange r90 = rangeFor(RangePreset::Last90Days, kBase, off);
        assert(r30.fromUnix - r90.fromUnix == 60 * kDay);
        assert((r90.fromUnix + (int64_t)off * 60) % kDay == 0);
        // East of Greenwich it must still land on a local midnight, which is
        // the whole reason this is not now - 90*86400.
        assert((rangeFor(RangePreset::Last90Days, kBase, 780).fromUnix + 780 * 60)
               % kDay == 0);

        StatsRange ry = rangeFor(RangePreset::ThisYear, kBase, off);
        assert(ry.fromUnix == 1672549200);    // local midnight, 2023-01-01
        int y = 0, m = 0, d = 0;
        statsCivilFromLocalDay(ry.fromUnix + (int64_t)off * 60, y, m, d);
        assert(y == 2023 && m == 1 && d == 1);

        StatsRange all = rangeFor(RangePreset::AllTime, kBase, off);
        assert(all.fromUnix == 0 && all.toUnix == 0);   // both sides open

        // The same instant east of Greenwich is a different local date, and
        // the preset has to follow the listener, not the server.
        StatsRange east = rangeFor(RangePreset::Last7Days, kBase, 780);  // UTC+13
        assert((east.fromUnix + 780 * 60) % kDay == 0);
        assert(east.fromUnix != r7.fromUnix);

        // And a preset actually filters: the last 7 days of a 2023 timeline
        // contain nothing when asked at kBase + a year.
        Track t = mkTrack("A", "Rec", "Song", 1);
        Db db;
        openWith(db, { t });
        logPlay(db, t, kBase, 200000, true, StartCause::Manual, EndCause::Natural);
        assert(db.totals(rangeFor(RangePreset::Last7Days, kBase, off)).plays == 1);
        assert(db.totals(rangeFor(RangePreset::Last7Days, kBase + 365 * kDay, off)).plays == 0);
        assert(db.totals(rangeFor(RangePreset::AllTime,   kBase + 365 * kDay, off)).plays == 1);
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
        assert(db.recentlyPlayed(StatsRange{}, 10).empty());
        assert(db.hourHistogram(StatsRange{}).size() == 24);
        assert(db.skipRate(StatsRange{}) == 0.0);
        assert(db.trackTotals("nothing").playCount == 0);
        assert(db.trackTotals("").playCount == 0);
        assert(db.trackTotals("nothing").lastPlayed == 0);

        SessionStats s = db.sessions(StatsRange{});
        assert(s.count == 0 && s.plays == 0 && s.spanMs == 0 && s.longestMs == 0);

        // The playlist generators too: an empty library must produce four
        // empty lists, not a crash and not a stale one.
        assert(db.heavyRotation(StatsRange{}, 10).empty());
        assert(db.forgottenFavourites(10, 3, kBase).empty());
        assert(db.neverHeard(0).empty());
        assert(db.heavyRotation(StatsRange{}, 0).empty());   // non-positive limit
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

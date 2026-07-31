// Listening analytics: the play_events log and every query derived from it.
//
// Split out of db.cpp because that file was already carrying the library, the
// settings, the roots and the EQ assignments, and this doubles its size. The
// seam is core/src/db_schema.h; both halves define methods on the same Db and
// share one sqlite3 connection, so there is no second handle to keep in step.
//
// The design rule this file exists to enforce: play_events is the ONLY stored
// truth about listening. Nothing here writes a derived counter anywhere, so
// nothing can drift out of step with the log. Every count, ranking and
// histogram below is computed on demand.

#include "db_schema.h"

#include <cstdio>
#include <ctime>

// ── Schema ──────────────────────────────────────────────────────────────────
// Keyed on track_key (core/variants.h), NOT on file_path: a path is rewritten
// whenever a folder is renamed, and history keyed on it orphans silently and
// unrecoverably. file_path is kept alongside as provenance — WHICH copy of the
// track actually played, which is how a 24-96 listen can be told from a 16-44
// one even though both share an identity.
static const char* STATS_SCHEMA = R"(
CREATE TABLE IF NOT EXISTS play_events (
    id             INTEGER PRIMARY KEY AUTOINCREMENT,
    track_key      TEXT    NOT NULL,
    file_path      TEXT    NOT NULL,
    started_at     INTEGER NOT NULL,   -- unix seconds, UTC
    -- Minutes east of UTC in force AT THAT MOMENT. Stored rather than derived
    -- because "what hour do you listen at" is a question about local clocks,
    -- and deriving it later smears every answer across a daylight-saving
    -- change or a move to another timezone.
    utc_offset_min INTEGER NOT NULL,
    ended_at       INTEGER,            -- NULL while the event is open
    ms_heard       INTEGER DEFAULT 0,
    -- Snapshot of the track's duration, so a later retag can't rewrite what
    -- fraction of it was heard.
    duration_ms    INTEGER DEFAULT 0,
    completed      INTEGER DEFAULT 0,
    start_cause    INTEGER DEFAULT 0,  -- StartCause, core/stats.h
    end_cause      INTEGER DEFAULT 0   -- EndCause,   core/stats.h
);
CREATE INDEX IF NOT EXISTS idx_play_events_key  ON play_events(track_key);
CREATE INDEX IF NOT EXISTS idx_play_events_time ON play_events(started_at);
)";

void stats_createSchema(sqlite3* db) {
    if (!db) return;
    sqlite3_exec(db, STATS_SCHEMA, nullptr, nullptr, nullptr);
}

// ── Local time ──────────────────────────────────────────────────────────────

int localUtcOffsetMinutes(int64_t whenUnixSec) {
    time_t t = (time_t)whenUnixSec;
    std::tm gm{}, lo{};
#ifdef _WIN32
    if (gmtime_s(&gm, &t) != 0 || localtime_s(&lo, &t) != 0) return 0;
#else
    if (!gmtime_r(&t, &gm) || !localtime_r(&t, &lo)) return 0;
#endif
    // mktime() reads its argument as LOCAL time. Handing it the UTC breakdown
    // yields the instant at which local clocks would read what UTC clocks read
    // now, and the gap from the real instant is the offset. Copying the real
    // tm_isdst across is what makes this the offset actually in force at `t`
    // rather than the zone's standard one.
    gm.tm_isdst = lo.tm_isdst;
    const int64_t asLocal = (int64_t)mktime(&gm);
    if (asLocal == -1) return 0;
    return (int)((whenUnixSec - asLocal) / 60);
}

// ── Shared SQL fragments ────────────────────────────────────────────────────
// Every query takes the range as parameters 1 and 2, where 0 means "open on
// that side". Half-open: from <= started_at < to.
#define RANGE_WHERE \
    "(?1 = 0 OR started_at >= ?1) AND (?2 = 0 OR started_at < ?2)"

// The instant shifted into the listener's own clock. Everything local-time
// below is expressed on top of this.
#define LOCAL_SEC "(started_at + utc_offset_min * 60)"

// Local hour, 0..23. The double modulo keeps it correct for a pre-1970
// timestamp, where SQLite's % would otherwise go negative.
#define LOCAL_HOUR "((((" LOCAL_SEC " % 86400) + 86400) % 86400) / 3600)"

// Unix second of the listener's local midnight for that event. Written as a
// floor rather than a truncating divide, same reason.
#define LOCAL_DAY_START \
    "(" LOCAL_SEC " - (((" LOCAL_SEC " % 86400) + 86400) % 86400) " \
    "- utc_offset_min * 60)"

// A skip: the track was left early and the listener is the one who left it —
// Next, Prev, or starting something else over it (Replaced). All three are the
// same act with a different button, so counting only the first two would let
// "picked a different song instead" hide from the number.
//
// Migrated rows (StartCause::Unknown) never recorded how they ended and are
// excluded — see the note on Db::skipRate.
#define IS_SKIP "(completed = 0 AND end_cause IN (1,2,4) AND start_cause <> 4)"

// Plays that say anything about the MUSIC at all, and so form the denominator
// the skip rate is a fraction of. Natural, Next, Prev and Replaced qualify.
//
// Stop and AppExit do not: stopping the transport or closing the app is a fact
// about the listener's evening, not about the track that happened to be
// playing. Counting either as "not skipped" would quietly deflate the rate for
// anyone who closes the app mid-song, which is most people.
#define IS_JUDGEABLE "(start_cause <> 4 AND end_cause IN (0,1,2,4))"

// ONE representative tracks row per identity. Every join from play_events to
// tracks MUST go through this.
//
// trackKey() deliberately gives the 16-44 and the 24-96 copy of a track — and
// a standard edition and its deluxe — the SAME key, because they are the same
// music and splitting their history would be wrong. The direct consequence is
// that `tracks` legitimately holds SEVERAL rows per key, so a plain
// `JOIN tracks ON track_key` fans out and counts one listen once per copy on
// disk. On a real library that was 52 counted plays against 43 real ones.
//
// MIN(id) is an arbitrary but stable choice among the copies. It only decides
// which edition's name gets printed; the counts are the same either way, and
// merging them under one name is what the variant system already does in the
// grid.
#define TRACK_REP \
    "JOIN (SELECT MIN(id) AS id, track_key FROM tracks " \
    "      WHERE track_key <> '' GROUP BY track_key) r ON r.track_key = e.track_key " \
    "JOIN tracks t ON t.id = r.id "

static void bindRange(sqlite3_stmt* stmt, const StatsRange& r) {
    sqlite3_bind_int64(stmt, 1, r.fromUnix);
    sqlite3_bind_int64(stmt, 2, r.toUnix);
}

static std::string colText(sqlite3_stmt* stmt, int i) {
    auto* s = (const char*)sqlite3_column_text(stmt, i);
    return s ? s : "";
}

// ── Writing the log ─────────────────────────────────────────────────────────

int64_t Db::beginPlayEvent(const std::string& trackKey,
                           const std::string& filePath,
                           int64_t durationMs,
                           StartCause cause,
                           int64_t whenUnixSec) {
    if (!impl_ || !impl_->db) return 0;
    // An empty key means the caller has no identity to record against — a
    // track outside the library, say. Logging it would create a row that can
    // never be attributed or grouped, so the honest answer is not to log.
    if (trackKey.empty()) return 0;

    const char* sql =
        "INSERT INTO play_events "
        "(track_key, file_path, started_at, utc_offset_min, ended_at, "
        " ms_heard, duration_ms, completed, start_cause, end_cause) "
        "VALUES (?,?,?,?,NULL,0,?,0,?,?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0;
    sqlite3_bind_text (stmt, 1, trackKey.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 2, filePath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, whenUnixSec);
    sqlite3_bind_int  (stmt, 4, localUtcOffsetMinutes(whenUnixSec));
    sqlite3_bind_int64(stmt, 5, durationMs);
    sqlite3_bind_int  (stmt, 6, (int)cause);
    // Open events carry EndCause::Unknown until they are closed. If the app
    // dies first, stats_closeOpenEvents() rewrites it to AppExit on next open.
    sqlite3_bind_int  (stmt, 7, (int)EndCause::Unknown);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok ? (int64_t)sqlite3_last_insert_rowid(impl_->db) : 0;
}

void Db::endPlayEvent(int64_t eventId, int64_t msHeard, bool completed,
                      EndCause cause) {
    if (!impl_ || !impl_->db || eventId <= 0) return;
    // Guarded on ended_at IS NULL so a double close — the outgoing track being
    // banked twice by two paths — cannot overwrite a settled verdict.
    const char* sql =
        "UPDATE play_events SET ended_at = strftime('%s','now'), ms_heard = ?, "
        "completed = ?, end_cause = ? WHERE id = ? AND ended_at IS NULL;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_int64(stmt, 1, msHeard < 0 ? 0 : msHeard);
    sqlite3_bind_int  (stmt, 2, completed ? 1 : 0);
    sqlite3_bind_int  (stmt, 3, (int)cause);
    sqlite3_bind_int64(stmt, 4, eventId);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void stats_closeOpenEvents(sqlite3* db) {
    if (!db) return;
    // ms_heard is left at whatever was written, which for a killed process is
    // 0. That is the truthful answer — nobody banked the figure — and marking
    // the row AppExit is what lets the completion and skip queries leave it
    // out instead of reading it as "heard nothing, therefore skipped".
    char sql[192];
    snprintf(sql, sizeof(sql),
             "UPDATE play_events SET ended_at = started_at + (ms_heard / 1000), "
             "end_cause = %d WHERE ended_at IS NULL;", (int)EndCause::AppExit);
    sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
}

// ── Migration from the old play_history log ─────────────────────────────────

void stats_backfillFromPlayHistory(sqlite3* db) {
    if (!db) return;

    // The old log recorded only (file_path, played_at) — no duration heard, no
    // completion, no reason. Those fields cannot be invented, so they come
    // across as zero and the row is stamped StartCause::Unknown, which every
    // completion- and skip-based query filters out. The PLAY COUNT, which is
    // the one thing the old log did record faithfully, carries over intact.
    //
    // The local offset is likewise unrecorded. The current machine's offset is
    // the best available guess — these plays happened on this machine — so it
    // is used rather than a misleading zero, which would file every historical
    // play under UTC and skew the hour histogram for anyone east or west of it.
    const int offsetGuess = localUtcOffsetMinutes((int64_t)time(nullptr));

    char sql[768];
    snprintf(sql, sizeof(sql),
        // A history row whose file has since left the library keeps its place
        // with an empty track_key. It can no longer be attributed to a track,
        // but destroying it outright would be worse: the totals would silently
        // shrink and nothing would say why.
        "INSERT INTO play_events "
        "(track_key, file_path, started_at, utc_offset_min, ended_at, "
        " ms_heard, duration_ms, completed, start_cause, end_cause) "
        "SELECT IFNULL(t.track_key, ''), h.file_path, h.played_at, %d, "
        "       h.played_at, 0, IFNULL(t.duration_ms, 0), 0, %d, %d "
        "FROM play_history h LEFT JOIN tracks t ON t.file_path = h.file_path;",
        offsetGuess, (int)StartCause::Unknown, (int)EndCause::Unknown);
    sqlite3_exec(db, sql, nullptr, nullptr, nullptr);

    // play_history and track_stats are deliberately left on disk untouched.
    // They have no writers left, but they are the only remaining copy of the
    // pre-migration aggregates, and a migration that cannot be checked against
    // anything is a migration nobody can trust.
}

// ── Aggregate queries ───────────────────────────────────────────────────────

Totals Db::totals(const StatsRange& range) {
    Totals out;
    if (!impl_ || !impl_->db) return out;

    {
        const char* sql =
            "SELECT COUNT(*), "
            "       SUM(CASE WHEN " IS_SKIP " THEN 1 ELSE 0 END), "
            "       SUM(ms_heard), "
            "       COUNT(DISTINCT NULLIF(track_key,'')), "
            "       COUNT(DISTINCT " LOCAL_DAY_START ") "
            "FROM play_events WHERE " RANGE_WHERE ";";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            bindRange(stmt, range);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                out.plays          = sqlite3_column_int64(stmt, 0);
                out.skips          = sqlite3_column_int64(stmt, 1);
                out.msHeard        = sqlite3_column_int64(stmt, 2);
                out.distinctTracks = sqlite3_column_int64(stmt, 3);
                out.activeDays     = sqlite3_column_int64(stmt, 4);
            }
        }
        sqlite3_finalize(stmt);
    }
    {
        // Albums and artists need the join, so they can only count plays whose
        // track is still in the library. Kept a separate statement so that
        // limitation stays visible instead of hiding inside a wider query.
        const char* sql =
            "SELECT COUNT(DISTINCT t.album), "
            "       COUNT(DISTINCT CASE WHEN IFNULL(t.album_artist,'') <> '' "
            "                           THEN t.album_artist ELSE t.artist END) "
            "FROM play_events e " TRACK_REP
            "WHERE " RANGE_WHERE ";";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            bindRange(stmt, range);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                out.distinctAlbums  = sqlite3_column_int64(stmt, 0);
                out.distinctArtists = sqlite3_column_int64(stmt, 1);
            }
        }
        sqlite3_finalize(stmt);
    }
    return out;
}

// Runs one ranking query: four columns (key, label, subLabel, then plays and
// ms), ordered and limited by the caller's SQL.
static std::vector<TopEntry> runTop(sqlite3* db, const char* sql,
                                    const StatsRange& range, int limit) {
    std::vector<TopEntry> out;
    if (!db || limit <= 0) return out;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
    bindRange(stmt, range);
    sqlite3_bind_int(stmt, 3, limit);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        TopEntry e;
        e.key      = colText(stmt, 0);
        e.label    = colText(stmt, 1);
        e.subLabel = colText(stmt, 2);
        e.plays    = sqlite3_column_int64(stmt, 3);
        e.msHeard  = sqlite3_column_int64(stmt, 4);
        out.push_back(std::move(e));
    }
    sqlite3_finalize(stmt);
    return out;
}

std::vector<TopEntry> Db::topTracks(const StatsRange& range, int limit) {
    // Grouped by the event's own key and joined via a LEFT JOIN through the
    // representative, so a track deleted from the library still ranks — it
    // just ranks without a name. MAX() picks one label per group; every row in
    // a group is the same track, so any will do, and MAX is what lets the
    // aggregate coexist with the GROUP BY.
    return runTop(impl_ ? impl_->db : nullptr,
        "SELECT e.track_key, IFNULL(MAX(t.title),''), "
        "       IFNULL(MAX(CASE WHEN IFNULL(t.album_artist,'') <> '' "
        "                       THEN t.album_artist ELSE t.artist END),''), "
        "       COUNT(*), SUM(e.ms_heard) "
        "FROM play_events e "
        "LEFT JOIN (SELECT MIN(id) AS id, track_key FROM tracks "
        "           WHERE track_key <> '' GROUP BY track_key) r "
        "       ON r.track_key = e.track_key "
        "LEFT JOIN tracks t ON t.id = r.id "
        "WHERE " RANGE_WHERE " AND e.track_key <> '' "
        "GROUP BY e.track_key "
        "ORDER BY COUNT(*) DESC, SUM(e.ms_heard) DESC, e.track_key ASC "
        "LIMIT ?3;", range, limit);
}

std::vector<TopEntry> Db::topAlbums(const StatsRange& range, int limit) {
    return runTop(impl_ ? impl_->db : nullptr,
        "SELECT t.album, t.album, "
        "       IFNULL(MAX(CASE WHEN IFNULL(t.album_artist,'') <> '' "
        "                       THEN t.album_artist ELSE t.artist END),''), "
        "       COUNT(*), SUM(e.ms_heard) "
        "FROM play_events e " TRACK_REP
        "WHERE " RANGE_WHERE " AND IFNULL(t.album,'') <> '' "
        "GROUP BY t.album "
        "ORDER BY COUNT(*) DESC, SUM(e.ms_heard) DESC, t.album ASC "
        "LIMIT ?3;", range, limit);
}

std::vector<TopEntry> Db::topArtists(const StatsRange& range, int limit) {
    // ALBUMARTIST first, same rule trackKey() follows, so a compilation does
    // not scatter one listener's plays across a dozen guest credits.
    //
    // The CASE is spelled out at every use rather than aliased once: a SELECT
    // alias is not in scope in WHERE, and leaning on SQLite tolerating it in
    // some positions and not others is how this silently returned nothing.
#define ARTIST_EXPR \
    "CASE WHEN IFNULL(t.album_artist,'') <> '' THEN t.album_artist ELSE t.artist END"
    return runTop(impl_ ? impl_->db : nullptr,
        "SELECT " ARTIST_EXPR ", " ARTIST_EXPR ", '', COUNT(*), SUM(e.ms_heard) "
        "FROM play_events e " TRACK_REP
        "WHERE " RANGE_WHERE " AND IFNULL(" ARTIST_EXPR ",'') <> '' "
        "GROUP BY " ARTIST_EXPR " "
        "ORDER BY COUNT(*) DESC, SUM(e.ms_heard) DESC, " ARTIST_EXPR " ASC "
        "LIMIT ?3;", range, limit);
#undef ARTIST_EXPR
}

std::vector<TopEntry> Db::topGenres(const StatsRange& range, int limit) {
    // Empty genres are excluded rather than bucketed as "Unknown": nothing
    // reads ID3 yet, so every MP3 in the library would pile into that bucket
    // and it would top the chart while meaning nothing. See TODO.md.
    return runTop(impl_ ? impl_->db : nullptr,
        "SELECT t.genre, t.genre, '', COUNT(*), SUM(e.ms_heard) "
        "FROM play_events e " TRACK_REP
        "WHERE " RANGE_WHERE " AND IFNULL(t.genre,'') <> '' "
        "GROUP BY t.genre "
        "ORDER BY COUNT(*) DESC, SUM(e.ms_heard) DESC, t.genre ASC "
        "LIMIT ?3;", range, limit);
}

std::vector<HourBucket> Db::hourHistogram(const StatsRange& range) {
    // All 24 buckets are returned, including the empty ones — a caller drawing
    // this as a chart needs the gaps to be gaps, not missing bars.
    std::vector<HourBucket> out(24);
    for (int h = 0; h < 24; h++) out[(size_t)h].hour = h;
    if (!impl_ || !impl_->db) return out;

    const char* sql =
        "SELECT " LOCAL_HOUR " AS h, COUNT(*), SUM(ms_heard) "
        "FROM play_events WHERE " RANGE_WHERE " GROUP BY h;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
    bindRange(stmt, range);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const int h = sqlite3_column_int(stmt, 0);
        if (h < 0 || h > 23) continue;
        out[(size_t)h].plays   = sqlite3_column_int64(stmt, 1);
        out[(size_t)h].msHeard = sqlite3_column_int64(stmt, 2);
    }
    sqlite3_finalize(stmt);
    return out;
}

std::vector<DayBucket> Db::dailyListening(const StatsRange& range) {
    // Only days with plays, oldest first. Silent days are absent rather than
    // zero-filled: the range can be years wide, and a caller drawing a
    // calendar or counting a streak knows its own bounds better than this does.
    std::vector<DayBucket> out;
    if (!impl_ || !impl_->db) return out;
    const char* sql =
        "SELECT " LOCAL_DAY_START " AS d, COUNT(*), SUM(ms_heard) "
        "FROM play_events WHERE " RANGE_WHERE " GROUP BY d ORDER BY d ASC;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
    bindRange(stmt, range);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        DayBucket d;
        d.dayUnix = sqlite3_column_int64(stmt, 0);
        d.plays   = sqlite3_column_int64(stmt, 1);
        d.msHeard = sqlite3_column_int64(stmt, 2);
        out.push_back(d);
    }
    sqlite3_finalize(stmt);
    return out;
}

std::vector<PlayEvent> Db::recentlyPlayed(int limit) {
    std::vector<PlayEvent> out;
    if (!impl_ || !impl_->db || limit <= 0) return out;
    const char* sql =
        "SELECT id, track_key, file_path, started_at, utc_offset_min, "
        "       IFNULL(ended_at,0), ms_heard, duration_ms, completed, "
        "       start_cause, end_cause "
        "FROM play_events ORDER BY started_at DESC, id DESC LIMIT ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_int(stmt, 1, limit);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        PlayEvent e;
        e.id           = sqlite3_column_int64(stmt, 0);
        e.trackKey     = colText(stmt, 1);
        e.filePath     = colText(stmt, 2);
        e.startedAt    = sqlite3_column_int64(stmt, 3);
        e.utcOffsetMin = sqlite3_column_int  (stmt, 4);
        e.endedAt      = sqlite3_column_int64(stmt, 5);
        e.msHeard      = sqlite3_column_int64(stmt, 6);
        e.durationMs   = sqlite3_column_int64(stmt, 7);
        e.completed    = sqlite3_column_int  (stmt, 8) != 0;
        e.startCause   = (StartCause)sqlite3_column_int(stmt, 9);
        e.endCause     = (EndCause)  sqlite3_column_int(stmt, 10);
        out.push_back(std::move(e));
    }
    sqlite3_finalize(stmt);
    return out;
}

TrackStats Db::trackTotals(const std::string& trackKey) {
    TrackStats out;
    if (!impl_ || !impl_->db || trackKey.empty()) return out;
    const char* sql =
        "SELECT COUNT(*), SUM(CASE WHEN " IS_SKIP " THEN 1 ELSE 0 END), "
        "       SUM(ms_heard) FROM play_events WHERE track_key = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_text(stmt, 1, trackKey.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out.playCount    = sqlite3_column_int64(stmt, 0);
        out.skipCount    = sqlite3_column_int64(stmt, 1);
        out.listenTimeMs = sqlite3_column_int64(stmt, 2);
    }
    sqlite3_finalize(stmt);
    return out;
}

double Db::skipRate(const StatsRange& range) {
    if (!impl_ || !impl_->db) return 0.0;
    const char* sql =
        "SELECT SUM(CASE WHEN " IS_SKIP " THEN 1 ELSE 0 END), "
        "       SUM(CASE WHEN " IS_JUDGEABLE " THEN 1 ELSE 0 END) "
        "FROM play_events WHERE " RANGE_WHERE ";";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0.0;
    bindRange(stmt, range);
    double rate = 0.0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const int64_t skips     = sqlite3_column_int64(stmt, 0);
        const int64_t judgeable = sqlite3_column_int64(stmt, 1);
        if (judgeable > 0) rate = (double)skips / (double)judgeable;
    }
    sqlite3_finalize(stmt);
    return rate;
}

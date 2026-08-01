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

// Midnight of the event's own local calendar date, in LOCAL seconds — the
// clock the listener reads, already shifted, not the UTC instant that midnight
// corresponds to. Written as a floor rather than a truncating divide so a
// pre-1970 timestamp lands on the right side of the boundary.
//
// The distinction is load-bearing. The UTC instant of a local midnight MOVES
// WITH THE OFFSET, so grouping on it splits one calendar day into two rows
// whenever the offset changes inside it — a daylight-saving Sunday, or a day
// spent flying — and makes Totals::activeDays count that day twice. Grouping
// on the local date instead keeps the day whole, and still separates two
// events that genuinely fall on different local dates.
#define LOCAL_DAY \
    "(" LOCAL_SEC " - (((" LOCAL_SEC " % 86400) + 86400) % 86400))"

// ALBUMARTIST first, falling back to ARTIST — the same rule trackKey() follows,
// so a compilation does not scatter one listener's plays across a dozen guest
// credits.
//
// Spelled out at every use rather than aliased once: a SELECT alias is not in
// scope in WHERE, and leaning on SQLite tolerating it in some positions and not
// others is how one of these queries silently returned nothing.
#define ARTIST_EXPR \
    "CASE WHEN IFNULL(t.album_artist,'') <> '' THEN t.album_artist ELSE t.artist END"

// The unit separator that joins the two halves of an album's identity in
// TopEntry::key. An album title alone is not an identity — two artists can
// both have a record called "Live" — and U+001F cannot occur in a tag.
#define KEY_SEP "char(31)"

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

// Plays that say something about TASTE, and the only basis a playlist is ever
// generated from. Two conditions, each for its own reason:
//
//   completed = 1     — hearing thirty seconds of something is not liking it.
//                       flushTrackStats() sets this at 90% of the tagged
//                       duration, so encoder padding cannot deny a full listen.
//
//   start_cause <> 5  — a play that CAME OUT of a playlist may not feed the
//                       playlist. Without this the list reinforces itself: the
//                       top track earns another count every time it is played
//                       because it is at the top, and nothing below it can ever
//                       catch up. A track earns its place by being chosen
//                       elsewhere — the grid, an album — or it does not earn it.
//                       The row is still written; it is only not counted here.
//
// Migrated rows exclude themselves: the old play_history recorded no
// completion, so they all carry completed = 0.
#define IS_AFFINITY "(completed = 1 AND start_cause <> 5)"

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

// The same representative, joined OUTWARD: the row survives with NULL labels
// when the track has left the library, instead of vanishing from the result.
//
// Which of the two a query wants is a real decision, not a style choice.
// TRACK_REP is for anything that GROUPS BY a tracks column — an album, an
// artist, a genre — because a row with no track has no such column to group
// under. TRACK_REP_LEFT is for anything keyed on track_key itself, where the
// listen is the fact and the name is only decoration: a deleted track keeps
// its plays, its rank and its place in the history, and loses only its title.
//
// Spelled once because four hand-copies of it is four chances for "every join
// goes through one representative" to quietly stop being true.
#define TRACK_REP_LEFT \
    "LEFT JOIN (SELECT MIN(id) AS id, track_key FROM tracks " \
    "           WHERE track_key <> '' GROUP BY track_key) r " \
    "       ON r.track_key = e.track_key " \
    "LEFT JOIN tracks t ON t.id = r.id "

static void bindRange(sqlite3_stmt* stmt, const StatsRange& r) {
    sqlite3_bind_int64(stmt, 1, r.fromUnix);
    sqlite3_bind_int64(stmt, 2, r.toUnix);
}

static std::string colText(sqlite3_stmt* stmt, int i) {
    auto* s = (const char*)sqlite3_column_text(stmt, i);
    return s ? s : "";
}

// One ranking row, in the column order every ranking query here selects:
// key, label, subLabel, plays, msHeard. Shared so a query that binds its own
// parameters (and so cannot go through runTop) still reads its rows the same
// way — the column ORDER is the contract, and it should be stated once.
static TopEntry readTopRow(sqlite3_stmt* stmt) {
    TopEntry e;
    e.key      = colText(stmt, 0);
    e.label    = colText(stmt, 1);
    e.subLabel = colText(stmt, 2);
    e.plays    = sqlite3_column_int64(stmt, 3);
    e.msHeard  = sqlite3_column_int64(stmt, 4);
    return e;
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
            "       COUNT(DISTINCT " LOCAL_DAY ") "
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

// The two measures, in the order the chosen one implies. The unchosen measure
// is the first tie-break, and the caller appends the group's own name as the
// last one, so the ordering is total — two entries with identical counts come
// back in the same order on every run.
static const char* topOrderBy(TopSort sort) {
    return sort == TopSort::TimeHeard
        ? "SUM(e.ms_heard) DESC, COUNT(*) DESC, "
        : "COUNT(*) DESC, SUM(e.ms_heard) DESC, ";
}

// Runs one ranking query: five columns (key, label, subLabel, plays, ms),
// ordered and limited by the caller's SQL.
static std::vector<TopEntry> runTop(sqlite3* db, const std::string& sql,
                                    const StatsRange& range, int limit) {
    std::vector<TopEntry> out;
    if (!db || limit <= 0) return out;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return out;
    bindRange(stmt, range);
    sqlite3_bind_int(stmt, 3, limit);
    while (sqlite3_step(stmt) == SQLITE_ROW)
        out.push_back(readTopRow(stmt));
    sqlite3_finalize(stmt);
    return out;
}

// Tracks ranked out of the log, grouped on the listener's own identity.
//
// Joined OUTWARD through the representative, so a track deleted from the
// library still ranks and merely loses its name. MAX() picks one label per
// group; every row in a group is the same track, so any will do, and MAX is
// what lets the aggregate coexist with the GROUP BY.
//
// `extraWhere` is an extra AND-ed condition ("" for none) and is the ONLY
// difference between the general ranking and the playlist generator, which
// adds IS_AFFINITY. Everything else — the join, the label picking, the
// tie-breaks — must stay identical between the two, which is exactly why it is
// written once: two copies means a fix to one silently skips the other.
static std::vector<TopEntry> rankTracks(sqlite3* db, const char* extraWhere,
                                        const StatsRange& range, int limit,
                                        TopSort sort) {
    return runTop(db,
        std::string(
        "SELECT e.track_key, IFNULL(MAX(t.title),''), "
        "       IFNULL(MAX(" ARTIST_EXPR "),''), "
        "       COUNT(*), SUM(e.ms_heard) "
        "FROM play_events e "
        TRACK_REP_LEFT
        "WHERE " RANGE_WHERE " AND e.track_key <> '' ") + extraWhere +
        " GROUP BY e.track_key "
        "ORDER BY " + topOrderBy(sort) + "e.track_key ASC "
        "LIMIT ?3;", range, limit);
}

std::vector<TopEntry> Db::topTracks(const StatsRange& range, int limit,
                                    TopSort sort) {
    return rankTracks(impl_ ? impl_->db : nullptr, "", range, limit, sort);
}

std::vector<TopEntry> Db::topAlbums(const StatsRange& range, int limit,
                                    TopSort sort) {
    // Grouped by album AND artist, never by title alone. Two artists can both
    // have a record called "Live", and grouping on the title merges them into
    // one row whose artist column is then whichever of the two MAX() happened
    // to pick — a wrong count under a misleading name. Same class of mistake
    // as the fan-out TRACK_REP exists to prevent, in the other direction.
    return runTop(impl_ ? impl_->db : nullptr,
        std::string(
        "SELECT t.album || " KEY_SEP " || " ARTIST_EXPR ", t.album, " ARTIST_EXPR ", "
        "       COUNT(*), SUM(e.ms_heard) "
        "FROM play_events e " TRACK_REP
        "WHERE " RANGE_WHERE " AND IFNULL(t.album,'') <> '' "
        "GROUP BY t.album, " ARTIST_EXPR " "
        "ORDER BY ") + topOrderBy(sort) + "t.album ASC, " ARTIST_EXPR " ASC "
        "LIMIT ?3;", range, limit);
}

std::vector<TopEntry> Db::topArtists(const StatsRange& range, int limit,
                                     TopSort sort) {
    return runTop(impl_ ? impl_->db : nullptr,
        std::string(
        "SELECT " ARTIST_EXPR ", " ARTIST_EXPR ", '', COUNT(*), SUM(e.ms_heard) "
        "FROM play_events e " TRACK_REP
        "WHERE " RANGE_WHERE " AND IFNULL(" ARTIST_EXPR ",'') <> '' "
        "GROUP BY " ARTIST_EXPR " "
        "ORDER BY ") + topOrderBy(sort) + ARTIST_EXPR " ASC "
        "LIMIT ?3;", range, limit);
}

std::vector<TopEntry> Db::topGenres(const StatsRange& range, int limit,
                                    TopSort sort) {
    // Empty genres are excluded rather than bucketed as "Unknown": nothing
    // reads ID3 yet, so every MP3 in the library would pile into that bucket
    // and it would top the chart while meaning nothing. See TODO.md.
    return runTop(impl_ ? impl_->db : nullptr,
        std::string(
        "SELECT t.genre, t.genre, '', COUNT(*), SUM(e.ms_heard) "
        "FROM play_events e " TRACK_REP
        "WHERE " RANGE_WHERE " AND IFNULL(t.genre,'') <> '' "
        "GROUP BY t.genre "
        "ORDER BY ") + topOrderBy(sort) + "t.genre ASC "
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
        "SELECT " LOCAL_DAY " AS d, COUNT(*), SUM(ms_heard) "
        "FROM play_events WHERE " RANGE_WHERE " GROUP BY d ORDER BY d ASC;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
    bindRange(stmt, range);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        DayBucket d;
        d.dayLocal = sqlite3_column_int64(stmt, 0);
        d.plays    = sqlite3_column_int64(stmt, 1);
        d.msHeard  = sqlite3_column_int64(stmt, 2);
        out.push_back(d);
    }
    sqlite3_finalize(stmt);
    return out;
}

std::vector<PlayEvent> Db::recentlyPlayed(const StatsRange& range, int limit) {
    std::vector<PlayEvent> out;
    if (!impl_ || !impl_->db || limit <= 0) return out;
    // LEFT JOIN through the representative, exactly as topTracks does: a track
    // deleted from the library keeps its place in the history and loses only
    // its name. A plain TRACK_REP here would drop the row entirely, which
    // would make this list disagree with every ranking beside it.
    const char* sql =
        "SELECT e.id, e.track_key, e.file_path, e.started_at, e.utc_offset_min, "
        "       IFNULL(e.ended_at,0), e.ms_heard, e.duration_ms, e.completed, "
        "       e.start_cause, e.end_cause, "
        "       IFNULL(t.title,''), IFNULL(" ARTIST_EXPR ",'') "
        "FROM play_events e "
        TRACK_REP_LEFT
        "WHERE " RANGE_WHERE " "
        "ORDER BY e.started_at DESC, e.id DESC LIMIT ?3;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
    bindRange(stmt, range);
    sqlite3_bind_int(stmt, 3, limit);
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
        e.title        = colText(stmt, 11);
        e.artist       = colText(stmt, 12);
        out.push_back(std::move(e));
    }
    sqlite3_finalize(stmt);
    return out;
}

TrackStats Db::trackTotals(const std::string& trackKey) {
    TrackStats out;
    if (!impl_ || !impl_->db || trackKey.empty()) return out;
    // MIN/MAX ride along in the same scan the counters already do, so "last
    // played three days ago" costs nothing beyond two more columns. They stay
    // 0 when the key has no events — SUM/MIN over an empty set are NULL, and
    // sqlite3_column_int64 reads NULL as 0.
    const char* sql =
        "SELECT COUNT(*), SUM(CASE WHEN " IS_SKIP " THEN 1 ELSE 0 END), "
        "       SUM(ms_heard), MIN(started_at), MAX(started_at) "
        "FROM play_events WHERE track_key = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_text(stmt, 1, trackKey.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out.playCount    = sqlite3_column_int64(stmt, 0);
        out.skipCount    = sqlite3_column_int64(stmt, 1);
        out.listenTimeMs = sqlite3_column_int64(stmt, 2);
        out.firstPlayed  = sqlite3_column_int64(stmt, 3);
        out.lastPlayed   = sqlite3_column_int64(stmt, 4);
    }
    sqlite3_finalize(stmt);
    return out;
}

// ── Generated playlists ─────────────────────────────────────────────────────
// Not stored anywhere. A playlist IS the query, run when the section opens, so
// there is no second copy of it that could drift from the log.

std::vector<TopEntry> Db::heavyRotation(const StatsRange& range, int limit) {
    // topTracks() plus one clause. That clause is the whole feature: it is
    // what makes this "what you have chosen and finished" rather than "what
    // has passed through the transport".
    //
    // Always by play count, never by time heard — a long track is not a
    // favourite for being long, and this list answers "what do you come back
    // to", which is a question about how OFTEN.
    return rankTracks(impl_ ? impl_->db : nullptr, "AND " IS_AFFINITY " ",
                      range, limit, TopSort::Plays);
}

std::vector<TopEntry> Db::forgottenFavourites(int limit, int minPlays,
                                              int64_t notSinceUnix) {
    std::vector<TopEntry> out;
    if (!impl_ || !impl_->db || limit <= 0) return out;
    // HAVING, not WHERE, for both conditions: each is a property of the GROUP
    // (how many completed plays this identity has, and when it was last
    // touched at all), not of a single row.
    //
    // MAX(e.started_at) deliberately spans EVERY event, not just the affinity
    // ones — a track played yesterday from a playlist has not been forgotten,
    // whatever the ranking says about it. So the cutoff runs on an unfiltered
    // MAX while the count runs on a filtered SUM.
    // Counted twice — as the ranking column and as the HAVING threshold — so
    // it is written once. A SELECT alias is not in scope in HAVING, which is
    // the same trap ARTIST_EXPR exists to avoid.
#define AFFINITY_PLAYS "SUM(CASE WHEN " IS_AFFINITY " THEN 1 ELSE 0 END)"
    const char* sql =
        "SELECT e.track_key, IFNULL(MAX(t.title),''), "
        "       IFNULL(MAX(" ARTIST_EXPR "),''), "
        "       " AFFINITY_PLAYS ", "
        "       SUM(CASE WHEN " IS_AFFINITY " THEN e.ms_heard ELSE 0 END) "
        "FROM play_events e "
        TRACK_REP_LEFT
        "WHERE e.track_key <> '' "
        "GROUP BY e.track_key "
        "HAVING " AFFINITY_PLAYS " >= ?1 "
        "   AND MAX(e.started_at) < ?2 "
        "ORDER BY 4 DESC, MAX(e.started_at) ASC, e.track_key ASC "
        "LIMIT ?3;";
#undef AFFINITY_PLAYS
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_int  (stmt, 1, minPlays < 1 ? 1 : minPlays);
    sqlite3_bind_int64(stmt, 2, notSinceUnix);
    sqlite3_bind_int  (stmt, 3, limit);
    while (sqlite3_step(stmt) == SQLITE_ROW)
        out.push_back(readTopRow(stmt));
    sqlite3_finalize(stmt);
    return out;
}

std::vector<TopEntry> Db::neverHeard(int limit) {
    std::vector<TopEntry> out;
    if (!impl_ || !impl_->db) return out;
    // The one query that starts from `tracks`. Everything else here asks the
    // log what happened; this asks the library what never did, and the log
    // cannot answer that — a track with no events has no rows to select.
    //
    // Grouped by track_key so the 16-44 and the 24-96 copy are one entry, the
    // same identity rule the rankings follow. ANY event disqualifies, however
    // brief: "I have never heard this" stops being true the first time it
    // plays at all, completed or not.
    std::string sql =
        "SELECT t.track_key, MIN(t.title), "
        "       MIN(" ARTIST_EXPR "), 0, 0 "
        "FROM tracks t "
        "LEFT JOIN play_events e ON e.track_key = t.track_key "
        "WHERE t.track_key <> '' AND e.id IS NULL "
        "GROUP BY t.track_key "
        "ORDER BY MIN(" ARTIST_EXPR ") ASC, MIN(t.album) ASC, "
        "         MIN(t.disc_number) ASC, MIN(t.track_number) ASC, "
        "         t.track_key ASC";
    // limit <= 0 means no limit: capping this would quietly decide how much of
    // your own library you are allowed to meet.
    if (limit > 0) sql += " LIMIT " + std::to_string(limit);
    sql += ";";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(impl_->db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        return out;
    while (sqlite3_step(stmt) == SQLITE_ROW)
        out.push_back(readTopRow(stmt));
    sqlite3_finalize(stmt);
    return out;
}

SessionStats Db::sessions(const StatsRange& range, int gapSec) {
    SessionStats out;
    if (!impl_ || !impl_->db) return out;
    if (gapSec < 0) gapSec = 0;

    // Folded in C++ rather than with SQL window functions. The fold is a dozen
    // lines and obviously correct to read; the windowed version is neither,
    // and the row count here is one per listen — a decade of heavy listening
    // is a few hundred thousand rows scanned once.
    //
    // A play's end is taken as started_at + ms_heard, NOT as ended_at.
    // ended_at is wall clock: it also counts the time the transport sat paused
    // or waiting for the next pick, so a session built on it swallows an
    // hour-long pause whole instead of splitting there. ms_heard is the
    // audible extent, which is what "still listening" means. Migrated rows
    // carry no ms_heard and so contribute a zero-length play — the honest
    // answer, since nobody recorded one.
    const char* sql =
        "SELECT started_at, ms_heard "
        "FROM play_events WHERE " RANGE_WHERE " ORDER BY started_at ASC, id ASC;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
    bindRange(stmt, range);

    bool    open      = false;
    int64_t sessStart = 0;   // first start in the current run
    int64_t sessEnd   = 0;   // latest end seen in it
    auto closeSession = [&]() {
        if (!open) return;
        const int64_t span = (sessEnd - sessStart) * 1000;
        out.count++;
        out.spanMs += span;
        if (span > out.longestMs) out.longestMs = span;
        open = false;
    };

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const int64_t start   = sqlite3_column_int64(stmt, 0);
        const int64_t msHeard = sqlite3_column_int64(stmt, 1);
        const int64_t end     = start + (msHeard > 0 ? msHeard / 1000 : 0);

        out.plays++;
        out.msHeard += msHeard;

        if (open && start - sessEnd <= (int64_t)gapSec) {
            if (end > sessEnd) sessEnd = end;
        } else {
            closeSession();
            open      = true;
            sessStart = start;
            sessEnd   = end;
        }
    }
    closeSession();
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

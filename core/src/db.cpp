#include "core/db.h"
#include "core/variants.h"      // trackKey() — the identity history is keyed on
#include "db_schema.h"
#include "sqlite3.h"
#include <cstdio>
#include <ctime>

static const char* SCHEMA = R"(
CREATE TABLE IF NOT EXISTS tracks (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    title         TEXT,
    artist        TEXT,
    album_artist  TEXT,
    album         TEXT,
    file_path     TEXT UNIQUE,
    track_number  INTEGER DEFAULT 0,
    disc_number   INTEGER DEFAULT 0,
    duration_ms   INTEGER,
    sample_rate   INTEGER,
    channels      INTEGER,
    bit_depth     INTEGER,
    file_size     INTEGER,
    file_mtime    INTEGER DEFAULT 0,
    -- Stable listening identity (core/variants.h trackKey()). file_path is
    -- this table's only stable *storage* key, but it is not a stable identity:
    -- renaming a folder rewrites every path and would orphan the history.
    track_key     TEXT    DEFAULT '',
    genre         TEXT    DEFAULT '',
    year          INTEGER DEFAULT 0
);
CREATE TABLE IF NOT EXISTS albums (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    name         TEXT,
    artist       TEXT,
    art_path     TEXT,
    display_name TEXT DEFAULT '',
    quality      TEXT DEFAULT '',
    mode         TEXT DEFAULT '',
    country      TEXT DEFAULT '',
    release_type    INTEGER DEFAULT 0,
    avg_sample_rate INTEGER DEFAULT 0,
    has_dsd         INTEGER DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_tracks_title  ON tracks(title);
CREATE INDEX IF NOT EXISTS idx_tracks_artist ON tracks(artist);
CREATE INDEX IF NOT EXISTS idx_tracks_album  ON tracks(album);
CREATE TABLE IF NOT EXISTS settings (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS music_roots (
    path TEXT PRIMARY KEY
);
CREATE TABLE IF NOT EXISTS eq_assignments (
    device_key     TEXT PRIMARY KEY,
    profile_name   TEXT NOT NULL,
    profile_source TEXT DEFAULT '',
    profile_form   TEXT DEFAULT ''
);
-- The headphones actually in use, which is NOT what eq_assignments records.
-- That table is keyed by device, and a DAC has no frequency response —
-- headphones do, and several take turns in one jack. So eq_assignments means
-- "which pair is plugged into this output right now", and this table is the
-- inventory the quick-switcher lists, shared across every output.
--
-- A row is earned, never merely selected: EqManager applies a profile the
-- instant it is picked, but it only lands here after a minute of real
-- listening (see kEqCreditMs in gui/src/player_view.cc). That is what keeps a
-- mis-click from permanently occupying a sidebar row.
CREATE TABLE IF NOT EXISTS eq_headphones (
    profile_name   TEXT    NOT NULL,
    profile_source TEXT    NOT NULL DEFAULT '',
    profile_form   TEXT    NOT NULL DEFAULT '',
    last_used      INTEGER NOT NULL DEFAULT 0,   -- unix seconds
    use_count      INTEGER NOT NULL DEFAULT 0,
    pinned         INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (profile_name, profile_source, profile_form)
);
-- Listening history and per-track counters.
--
-- Keyed by file_path, NOT by tracks.id, even though TODO.md said "track_id".
-- saveTracks() writes INSERT OR REPLACE against the file_path UNIQUE index, and
-- REPLACE deletes the conflicting row before inserting — so a track's
-- AUTOINCREMENT id changes every time its row is rewritten. History keyed on it
-- would silently orphan on a rescan. file_path is the schema's only stable
-- identity, so it is the key; join to tracks on file_path when a query needs
-- title/artist.
CREATE TABLE IF NOT EXISTS play_history (
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    file_path TEXT    NOT NULL,
    played_at INTEGER NOT NULL          -- unix seconds
);
CREATE INDEX IF NOT EXISTS idx_play_history_path ON play_history(file_path);
CREATE TABLE IF NOT EXISTS track_stats (
    file_path      TEXT PRIMARY KEY,
    play_count     INTEGER DEFAULT 0,
    skip_count     INTEGER DEFAULT 0,
    listen_time_ms INTEGER DEFAULT 0
);
-- Resume-on-launch. Exactly one row (the CHECK enforces it), rewritten in
-- place, so there is no history to prune here — play_history is the log.
CREATE TABLE IF NOT EXISTS playback_state (
    id          INTEGER PRIMARY KEY CHECK (id = 1),
    file_path   TEXT    NOT NULL,
    position_ms INTEGER DEFAULT 0,
    volume      REAL    DEFAULT 1.0,
    saved_at    INTEGER DEFAULT 0
);
)";

// ── Column adds ─────────────────────────────────────────────────────────────
// Fired blind, one statement at a time, errors ignored: a re-run fails with
// "duplicate column name", which is exactly the "already applied" signal. That
// is fine for ADDING A COLUMN and for nothing else — it cannot express "do
// this once", which is what SCHEMA_STEPS below is for.
//
// Each statement runs SEPARATELY. Do not fold these into SCHEMA: sqlite3_exec
// stops a multi-statement string at the first error, so one expected failure
// here would silently skip everything after it.
static const char* MIGRATIONS[] = {
    "ALTER TABLE tracks ADD COLUMN album_artist TEXT DEFAULT '';",
    "ALTER TABLE tracks ADD COLUMN track_number INTEGER DEFAULT 0;",
    "ALTER TABLE tracks ADD COLUMN file_mtime INTEGER DEFAULT 0;",
    "ALTER TABLE albums ADD COLUMN display_name TEXT DEFAULT '';",
    "ALTER TABLE albums ADD COLUMN quality TEXT DEFAULT '';",
    "ALTER TABLE albums ADD COLUMN mode TEXT DEFAULT '';",
    "ALTER TABLE albums ADD COLUMN country TEXT DEFAULT '';",
    "ALTER TABLE albums ADD COLUMN release_type INTEGER DEFAULT 0;",
    "ALTER TABLE albums ADD COLUMN avg_sample_rate INTEGER DEFAULT 0;",
    "ALTER TABLE albums ADD COLUMN has_dsd INTEGER DEFAULT 0;",
    // Listening analytics. The index has to live here rather than in SCHEMA
    // for the same reason: on a pre-existing database the column does not
    // exist until the ALTER above it has run.
    "ALTER TABLE tracks ADD COLUMN track_key TEXT DEFAULT '';",
    "ALTER TABLE tracks ADD COLUMN genre TEXT DEFAULT '';",
    "ALTER TABLE tracks ADD COLUMN year INTEGER DEFAULT 0;",
    "CREATE INDEX IF NOT EXISTS idx_tracks_key ON tracks(track_key);",
};

static void runMigrations(sqlite3* db) {
    for (const char* sql : MIGRATIONS) {
        sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
    }

    // disc_number is the one migration that can't just default: the
    // incremental scan skips any file whose (size, mtime) still matches its
    // cached row (see scanLibraryIncremental), so an existing library would
    // keep disc_number = 0 forever and multi-disc albums would stay
    // interleaved. The ALTER only returns SQLITE_OK the one time the column is
    // actually created — on a database that already has it (migrated earlier,
    // or created fresh from SCHEMA above) it fails with "duplicate column
    // name". So a successful ALTER is exactly the signal "the tag has never
    // been read on this machine": zero the cached mtimes to force one full
    // re-parse on the next scan, after which everything is incremental again.
    if (sqlite3_exec(db, "ALTER TABLE tracks ADD COLUMN disc_number INTEGER DEFAULT 0;",
                     nullptr, nullptr, nullptr) == SQLITE_OK) {
        sqlite3_exec(db, "UPDATE tracks SET file_mtime = 0;", nullptr, nullptr, nullptr);
    }
}

// ── Versioned schema steps ──────────────────────────────────────────────────
// The division of labour with MIGRATIONS above: that array ADDS COLUMNS and is
// safe to re-run; these steps do ONE-SHOT DATA WORK and are not. Re-running
// the play_history backfill would duplicate the whole listening log on every
// launch, so "has this already happened" has to be recorded somewhere —
// PRAGMA user_version, checked once and stamped once.
//
// Version 1 is the baseline: everything the schema was before listening
// analytics. New work appends a step and bumps kSchemaVersion. A step never
// runs twice, and it runs inside a transaction, so a crash halfway leaves the
// database at the previous version rather than half-migrated.

// Fills tracks.track_key for rows written before the column existed. Every
// input trackKey() needs — title, artist, album_artist, album, disc, track,
// path — is already in the row, so this reads no files and needs no rescan.
// It must run BEFORE the play_history backfill, which joins on this column.
static void step_backfillTrackKeys(sqlite3* db) {
    std::vector<Track> tracks;
    sqlite3_stmt* sel = nullptr;
    sqlite3_prepare_v2(db,
        "SELECT title, artist, album_artist, album, file_path, "
        "track_number, disc_number FROM tracks WHERE IFNULL(track_key,'') = '';",
        -1, &sel, nullptr);
    while (sel && sqlite3_step(sel) == SQLITE_ROW) {
        auto col = [&](int i) -> std::string {
            auto* s = (const char*)sqlite3_column_text(sel, i);
            return s ? s : "";
        };
        Track t;
        t.title       = col(0);
        t.artist      = col(1);
        t.albumArtist = col(2);
        t.album       = col(3);
        t.filePath    = col(4);
        t.trackNumber = sqlite3_column_int(sel, 5);
        t.discNumber  = sqlite3_column_int(sel, 6);
        tracks.push_back(std::move(t));
    }
    sqlite3_finalize(sel);

    sqlite3_stmt* upd = nullptr;
    sqlite3_prepare_v2(db, "UPDATE tracks SET track_key = ? WHERE file_path = ?;",
                       -1, &upd, nullptr);
    for (const Track& t : tracks) {
        const std::string key = trackKey(t);
        sqlite3_bind_text(upd, 1, key.c_str(),        -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(upd, 2, t.filePath.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(upd);
        sqlite3_reset(upd);
    }
    sqlite3_finalize(upd);

    // GENRE and DATE arrived with the same release, and the incremental scan
    // skips any file whose (size, mtime) still matches its cached row — so an
    // existing library would keep an empty genre forever. Same fix as
    // disc_number above: zero the cached mtimes to buy one full re-parse.
    sqlite3_exec(db, "UPDATE tracks SET file_mtime = 0;", nullptr, nullptr, nullptr);
}

// Seeds eq_headphones from whatever was already assigned per device, so an
// existing install doesn't meet an empty quick-switcher. Pinned, because a
// profile the listener chose by hand must not age out of the prune below —
// there is no other record that they ever picked it.
static void step_seedEqHeadphones(sqlite3* db) {
    sqlite3_exec(db,
        "INSERT OR IGNORE INTO eq_headphones "
        "(profile_name, profile_source, profile_form, last_used, use_count, pinned) "
        "SELECT profile_name, IFNULL(profile_source,''), IFNULL(profile_form,''), "
        "       strftime('%s','now'), 1, 1 "
        "FROM eq_assignments WHERE IFNULL(profile_name,'') <> '';",
        nullptr, nullptr, nullptr);
}

struct SchemaStep {
    int   version;
    void (*apply)(sqlite3* db);
};

static const SchemaStep SCHEMA_STEPS[] = {
    { 2, step_backfillTrackKeys },
    { 3, stats_backfillFromPlayHistory },
    { 4, step_seedEqHeadphones },
};
static const int kSchemaVersion = 4;

static int readUserVersion(sqlite3* db) {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &stmt, nullptr);
    int v = 0;
    if (stmt && sqlite3_step(stmt) == SQLITE_ROW) v = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return v;
}

static void writeUserVersion(sqlite3* db, int v) {
    char sql[64];
    snprintf(sql, sizeof(sql), "PRAGMA user_version = %d;", v);
    sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
}

static void runSchemaSteps(sqlite3* db) {
    int from = readUserVersion(db);
    if (from >= kSchemaVersion) return;
    for (const SchemaStep& s : SCHEMA_STEPS) {
        if (s.version <= from) continue;
        sqlite3_exec(db, "BEGIN;", nullptr, nullptr, nullptr);
        s.apply(db);
        writeUserVersion(db, s.version);
        sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);
    }
}

// True if `table` already exists. Asked BEFORE the schema is created, to tell
// a brand-new database from one being upgraded.
static bool tableExists(sqlite3* db, const char* table) {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db,
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?;",
        -1, &stmt, nullptr);
    if (!stmt) return false;
    sqlite3_bind_text(stmt, 1, table, -1, SQLITE_TRANSIENT);
    bool found = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return found;
}

bool Db::open(const std::string& dbPath) {
    impl_ = new Impl;
    if (sqlite3_open(dbPath.c_str(), &impl_->db) != SQLITE_OK) {
        fprintf(stderr, "[DB][ERROR] open failed: %s\n", sqlite3_errmsg(impl_->db));
        return false;
    }
    // Asked first: SCHEMA is about to create this table if it is missing, and
    // after that a fresh database is indistinguishable from an upgraded one.
    const bool preExisting = tableExists(impl_->db, "tracks");

    sqlite3_exec(impl_->db, SCHEMA, nullptr, nullptr, nullptr);
    stats_createSchema(impl_->db);
    runMigrations(impl_->db);

    if (preExisting) {
        runSchemaSteps(impl_->db);
    } else {
        // Nothing to migrate — SCHEMA just built the current shape, and every
        // step above would be a no-op over zero rows. Stamp it and skip them.
        writeUserVersion(impl_->db, kSchemaVersion);
    }

    // Any event still open belongs to a session that never closed cleanly.
    // Closed here, before a single query can read a half-written log.
    stats_closeOpenEvents(impl_->db);

    sqlite3_exec(impl_->db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(impl_->db,
        "DELETE FROM albums WHERE rowid NOT IN "
        "(SELECT MIN(rowid) FROM albums GROUP BY name, artist);",
        nullptr, nullptr, nullptr);
    return true;
}

void Db::close() {
    if (impl_) {
        if (impl_->db) sqlite3_close(impl_->db);
        delete impl_;
        impl_ = nullptr;
    }
}

void Db::saveTracks(const std::vector<Track>& tracks) {
    if (!impl_->db) return;
    sqlite3_exec(impl_->db, "BEGIN;", nullptr, nullptr, nullptr);
    const char* sql =
        "INSERT OR REPLACE INTO tracks "
        "(title, artist, album_artist, album, file_path, track_number, "
        "duration_ms, sample_rate, channels, bit_depth, file_size, file_mtime, "
        "disc_number, track_key, genre, year) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
    for (auto& t : tracks) {
        sqlite3_bind_text (stmt, 1, t.title.c_str(),       -1, SQLITE_TRANSIENT);
        sqlite3_bind_text (stmt, 2, t.artist.c_str(),      -1, SQLITE_TRANSIENT);
        sqlite3_bind_text (stmt, 3, t.albumArtist.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text (stmt, 4, t.album.c_str(),       -1, SQLITE_TRANSIENT);
        sqlite3_bind_text (stmt, 5, t.filePath.c_str(),    -1, SQLITE_TRANSIENT);
        sqlite3_bind_int  (stmt, 6, t.trackNumber);
        sqlite3_bind_int  (stmt, 7, t.durationMs);
        sqlite3_bind_int  (stmt, 8, t.sampleRate);
        sqlite3_bind_int  (stmt, 9, t.channels);
        sqlite3_bind_int  (stmt, 10, t.bitDepth);
        sqlite3_bind_int64(stmt, 11, t.fileSize);
        sqlite3_bind_int64(stmt, 12, t.fileMtime);
        sqlite3_bind_int  (stmt, 13, t.discNumber);
        sqlite3_bind_text (stmt, 14, t.trackKey.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text (stmt, 15, t.genre.c_str(),    -1, SQLITE_TRANSIENT);
        sqlite3_bind_int  (stmt, 16, t.year);
        sqlite3_step(stmt);
        sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);
    sqlite3_exec(impl_->db, "COMMIT;", nullptr, nullptr, nullptr);
}

std::vector<Track> Db::loadTracks() {
    std::vector<Track> out;
    if (!impl_->db) return out;
    const char* sql =
        "SELECT id, title, artist, album_artist, album, file_path, track_number, "
        "duration_ms, sample_rate, channels, bit_depth, file_size, file_mtime, "
        "disc_number, track_key, genre, year FROM tracks;";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Track t;
        t.id = sqlite3_column_int(stmt, 0);
        auto col = [&](int i) -> std::string {
            auto* s = (const char*)sqlite3_column_text(stmt, i);
            return s ? s : "";
        };
        t.title       = col(1);
        t.artist      = col(2);
        t.albumArtist = col(3);
        t.album       = col(4);
        t.filePath    = col(5);
        t.trackNumber = sqlite3_column_int(stmt, 6);
        t.durationMs  = sqlite3_column_int(stmt, 7);
        t.sampleRate  = sqlite3_column_int(stmt, 8);
        t.channels    = sqlite3_column_int(stmt, 9);
        t.bitDepth    = sqlite3_column_int(stmt, 10);
        t.fileSize    = sqlite3_column_int64(stmt, 11);
        t.fileMtime   = sqlite3_column_int64(stmt, 12);
        t.discNumber  = sqlite3_column_int  (stmt, 13);
        t.trackKey    = col(14);
        t.genre       = col(15);
        t.year        = sqlite3_column_int  (stmt, 16);
        out.push_back(std::move(t));
    }
    sqlite3_finalize(stmt);
    return out;
}

void Db::saveAlbums(const std::vector<Album>& albums) {
    if (!impl_->db) return;
    sqlite3_exec(impl_->db, "BEGIN;", nullptr, nullptr, nullptr);
    sqlite3_exec(impl_->db, "DELETE FROM albums;", nullptr, nullptr, nullptr);
    const char* sql = "INSERT INTO albums (name, artist, art_path, "
                      "display_name, quality, mode, country, "
                      "release_type, avg_sample_rate, has_dsd) VALUES (?,?,?,?,?,?,?,?,?,?);";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
    for (auto& a : albums) {
        sqlite3_bind_text(stmt, 1, a.name.c_str(),        -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, a.artist.c_str(),      -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, a.artPath.c_str(),     -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, a.displayName.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, a.quality.c_str(),     -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 6, a.mode.c_str(),        -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 7, a.country.c_str(),     -1, SQLITE_TRANSIENT);
        sqlite3_bind_int (stmt, 8, (int)a.releaseType);
        sqlite3_bind_int (stmt, 9, a.avgSampleRate);
        sqlite3_bind_int (stmt, 10, a.hasDsd ? 1 : 0);
        sqlite3_step(stmt);
        sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);
    sqlite3_exec(impl_->db, "COMMIT;", nullptr, nullptr, nullptr);
}

void Db::saveSetting(const std::string& key, const std::string& value) {
    if (!impl_->db) return;
    const char* sql = "INSERT OR REPLACE INTO settings (key, value) VALUES (?, ?);";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, key.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

std::string Db::loadSetting(const std::string& key) {
    if (!impl_->db) return {};
    const char* sql = "SELECT value FROM settings WHERE key = ?;";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    std::string result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        auto* s = (const char*)sqlite3_column_text(stmt, 0);
        if (s) result = s;
    }
    sqlite3_finalize(stmt);
    return result;
}

void Db::addMusicRoot(const std::string& path) {
    if (!impl_->db) return;
    const char* sql = "INSERT OR IGNORE INTO music_roots (path) VALUES (?);";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void Db::removeMusicRoot(const std::string& path) {
    if (!impl_->db) return;
    std::string prefix = path;
    if (!prefix.empty() && prefix.back() != '\\') prefix += '\\';
    std::string pattern = prefix + "%";

    sqlite3_exec(impl_->db, "BEGIN;", nullptr, nullptr, nullptr);

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(impl_->db,
        "DELETE FROM music_roots WHERE path = ?;", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    sqlite3_prepare_v2(impl_->db,
        "DELETE FROM tracks WHERE file_path LIKE ?;", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, pattern.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    sqlite3_exec(impl_->db, "COMMIT;", nullptr, nullptr, nullptr);
}

std::vector<std::string> Db::loadMusicRoots() {
    std::vector<std::string> out;
    if (!impl_->db) return out;
    const char* sql = "SELECT path FROM music_roots;";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        auto* s = (const char*)sqlite3_column_text(stmt, 0);
        if (s) out.emplace_back(s);
    }
    sqlite3_finalize(stmt);
    return out;
}

std::vector<Album> Db::loadAlbums() {
    std::vector<Album> out;
    if (!impl_->db) return out;
    const char* sql = "SELECT name, artist, art_path, "
                      "display_name, quality, mode, country, "
                      "release_type, avg_sample_rate, has_dsd FROM albums;";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Album a;
        auto col = [&](int i) -> std::string {
            auto* s = (const char*)sqlite3_column_text(stmt, i);
            return s ? s : "";
        };
        a.name        = col(0);
        a.artist      = col(1);
        a.artPath     = col(2);
        a.displayName = col(3);
        a.quality     = col(4);
        a.mode        = col(5);
        a.country     = col(6);
        a.releaseType   = (Album::ReleaseType)sqlite3_column_int(stmt, 7);
        a.avgSampleRate = sqlite3_column_int(stmt, 8);
        a.hasDsd        = sqlite3_column_int(stmt, 9) != 0;
        // Rows written before the display_name migration: fall back to the
        // raw folder name so the grid never renders empty titles.
        if (a.displayName.empty()) a.displayName = a.name;
        out.push_back(std::move(a));
    }
    sqlite3_finalize(stmt);
    return out;
}

std::map<std::string, FileCache> Db::loadFileCache() {
    std::map<std::string, FileCache> out;
    if (!impl_->db) return out;
    const char* sql = "SELECT file_path, file_size, file_mtime FROM tracks;";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        auto* path = (const char*)sqlite3_column_text(stmt, 0);
        if (!path) continue;
        FileCache fc;
        fc.fileSize  = sqlite3_column_int64(stmt, 1);
        fc.fileMtime = sqlite3_column_int64(stmt, 2);
        out[path] = fc;
    }
    sqlite3_finalize(stmt);
    return out;
}

void Db::removeTracksByPaths(const std::vector<std::string>& paths) {
    if (!impl_->db || paths.empty()) return;
    sqlite3_exec(impl_->db, "BEGIN;", nullptr, nullptr, nullptr);
    const char* sql = "DELETE FROM tracks WHERE file_path = ?;";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
    for (auto& p : paths) {
        sqlite3_bind_text(stmt, 1, p.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);
    sqlite3_exec(impl_->db, "COMMIT;", nullptr, nullptr, nullptr);
}

void Db::saveEqAssignment(const std::string& deviceKey,
                          const std::string& name,
                          const std::string& source,
                          const std::string& form) {
    if (!impl_->db) return;
    const char* sql =
        "INSERT OR REPLACE INTO eq_assignments "
        "(device_key, profile_name, profile_source, profile_form) VALUES (?,?,?,?);";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, deviceKey.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, name.c_str(),      -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, source.c_str(),    -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, form.c_str(),      -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void Db::clearEqAssignment(const std::string& deviceKey) {
    if (!impl_->db) return;
    const char* sql = "DELETE FROM eq_assignments WHERE device_key = ?;";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, deviceKey.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

// ── Headphone inventory (eq_headphones) ─────────────────────────────────────
// Pinned first, then most-recently-used. The two orderings are one list on
// purpose: pinning is how the listener says "this pair is mine regardless of
// when I last reached for it", and recency handles everything else.

// How many unpinned rows survive a prune. The whole point of the earn-your-row
// rule is that the list stays short enough to read at a glance; an unbounded
// table would reintroduce the clutter by the back door, just more slowly.
static const int kEqHeadphoneKeep = 12;

std::vector<EqHeadphone> Db::loadEqHeadphones(int limit) {
    std::vector<EqHeadphone> out;
    if (!impl_->db) return out;
    const char* sql =
        "SELECT profile_name, profile_source, profile_form, last_used, use_count, pinned "
        "FROM eq_headphones "
        "ORDER BY pinned DESC, use_count DESC, last_used DESC, profile_name ASC LIMIT ?;";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
    if (!stmt) return out;
    sqlite3_bind_int(stmt, 1, limit > 0 ? limit : -1);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        auto col = [&](int i) -> std::string {
            auto* s = (const char*)sqlite3_column_text(stmt, i);
            return s ? s : "";
        };
        EqHeadphone h;
        h.name     = col(0);
        h.source   = col(1);
        h.form     = col(2);
        h.lastUsed = sqlite3_column_int64(stmt, 3);
        h.useCount = sqlite3_column_int64(stmt, 4);
        h.pinned   = sqlite3_column_int(stmt, 5) != 0;
        out.push_back(std::move(h));
    }
    sqlite3_finalize(stmt);
    return out;
}

// Two statements rather than INSERT OR REPLACE: replacing the row would wipe
// pinned and reset use_count, silently unpinning a pair every time it played.
void Db::creditEqHeadphone(const std::string& name, const std::string& source,
                           const std::string& form, int64_t whenUnixSec) {
    if (!impl_->db || name.empty()) return;

    sqlite3_stmt* ins = nullptr;
    sqlite3_prepare_v2(impl_->db,
        "INSERT OR IGNORE INTO eq_headphones "
        "(profile_name, profile_source, profile_form) VALUES (?,?,?);",
        -1, &ins, nullptr);
    sqlite3_bind_text(ins, 1, name.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 2, source.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 3, form.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_step(ins);
    sqlite3_finalize(ins);

    sqlite3_stmt* upd = nullptr;
    sqlite3_prepare_v2(impl_->db,
        "UPDATE eq_headphones SET last_used = ?, use_count = use_count + 1 "
        "WHERE profile_name = ? AND profile_source = ? AND profile_form = ?;",
        -1, &upd, nullptr);
    sqlite3_bind_int64(upd, 1, whenUnixSec);
    sqlite3_bind_text (upd, 2, name.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (upd, 3, source.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (upd, 4, form.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_step(upd);
    sqlite3_finalize(upd);

    // Prune the tail. Pinned rows are exempt and are not counted against the
    // budget, so pinning a dozen pairs can never evict the one just played.
    sqlite3_stmt* del = nullptr;
    sqlite3_prepare_v2(impl_->db,
        "DELETE FROM eq_headphones WHERE pinned = 0 AND rowid NOT IN "
        "(SELECT rowid FROM eq_headphones WHERE pinned = 0 "
        " ORDER BY last_used DESC, use_count DESC LIMIT ?);",
        -1, &del, nullptr);
    sqlite3_bind_int(del, 1, kEqHeadphoneKeep);
    sqlite3_step(del);
    sqlite3_finalize(del);
}

void Db::setEqHeadphonePinned(const std::string& name, const std::string& source,
                              const std::string& form, bool pinned) {
    if (!impl_->db) return;
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(impl_->db,
        "UPDATE eq_headphones SET pinned = ? "
        "WHERE profile_name = ? AND profile_source = ? AND profile_form = ?;",
        -1, &stmt, nullptr);
    sqlite3_bind_int (stmt, 1, pinned ? 1 : 0);
    sqlite3_bind_text(stmt, 2, name.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, source.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, form.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void Db::removeEqHeadphone(const std::string& name, const std::string& source,
                           const std::string& form) {
    if (!impl_->db) return;
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(impl_->db,
        "DELETE FROM eq_headphones "
        "WHERE profile_name = ? AND profile_source = ? AND profile_form = ?;",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, name.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, source.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, form.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

// The listening log (play_events) and every query derived from it live in
// db_stats.cpp — see core/src/db_schema.h for the seam between the two.

void Db::savePlaybackState(const PlaybackState& st) {
    if (!impl_->db || st.filePath.empty()) return;
    const char* sql =
        "INSERT OR REPLACE INTO playback_state "
        "(id, file_path, position_ms, volume, saved_at) "
        "VALUES (1, ?, ?, ?, strftime('%s','now'));";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
    sqlite3_bind_text  (stmt, 1, st.filePath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int   (stmt, 2, st.positionMs);
    sqlite3_bind_double(stmt, 3, (double)st.volume);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

bool Db::loadPlaybackState(PlaybackState& out) {
    if (!impl_->db) return false;
    const char* sql =
        "SELECT file_path, position_ms, volume FROM playback_state WHERE id = 1;";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        auto* s = (const char*)sqlite3_column_text(stmt, 0);
        out.filePath   = s ? s : "";
        out.positionMs = sqlite3_column_int(stmt, 1);
        out.volume     = (float)sqlite3_column_double(stmt, 2);
        found = !out.filePath.empty();
    }
    sqlite3_finalize(stmt);
    return found;
}

bool Db::loadEqAssignment(const std::string& deviceKey, EqAssignment& out) {
    if (!impl_->db) return false;
    const char* sql =
        "SELECT profile_name, profile_source, profile_form "
        "FROM eq_assignments WHERE device_key = ?;";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, deviceKey.c_str(), -1, SQLITE_TRANSIENT);
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        auto col = [&](int i) -> std::string {
            auto* s = (const char*)sqlite3_column_text(stmt, i);
            return s ? s : "";
        };
        out.name   = col(0);
        out.source = col(1);
        out.form   = col(2);
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

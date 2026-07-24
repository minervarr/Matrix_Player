#include "core/db.h"
#include "sqlite3.h"
#include <cstdio>

struct Db::Impl {
    sqlite3* db = nullptr;
};

static const char* SCHEMA = R"(
CREATE TABLE IF NOT EXISTS tracks (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    title         TEXT,
    artist        TEXT,
    album_artist  TEXT,
    album         TEXT,
    file_path     TEXT UNIQUE,
    track_number  INTEGER DEFAULT 0,
    duration_ms   INTEGER,
    sample_rate   INTEGER,
    channels      INTEGER,
    bit_depth     INTEGER,
    file_size     INTEGER,
    file_mtime    INTEGER DEFAULT 0
);
CREATE TABLE IF NOT EXISTS albums (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    name         TEXT,
    artist       TEXT,
    art_path     TEXT,
    display_name TEXT DEFAULT '',
    quality      TEXT DEFAULT '',
    mode         TEXT DEFAULT '',
    country      TEXT DEFAULT ''
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
)";

static const char* MIGRATIONS[] = {
    "ALTER TABLE tracks ADD COLUMN album_artist TEXT DEFAULT '';",
    "ALTER TABLE tracks ADD COLUMN track_number INTEGER DEFAULT 0;",
    "ALTER TABLE tracks ADD COLUMN file_mtime INTEGER DEFAULT 0;",
    "ALTER TABLE albums ADD COLUMN display_name TEXT DEFAULT '';",
    "ALTER TABLE albums ADD COLUMN quality TEXT DEFAULT '';",
    "ALTER TABLE albums ADD COLUMN mode TEXT DEFAULT '';",
    "ALTER TABLE albums ADD COLUMN country TEXT DEFAULT '';",
};

static void runMigrations(sqlite3* db) {
    for (const char* sql : MIGRATIONS) {
        sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
    }
}

bool Db::open(const std::string& dbPath) {
    impl_ = new Impl;
    if (sqlite3_open(dbPath.c_str(), &impl_->db) != SQLITE_OK) {
        fprintf(stderr, "[DB][ERROR] open failed: %s\n", sqlite3_errmsg(impl_->db));
        return false;
    }
    sqlite3_exec(impl_->db, SCHEMA, nullptr, nullptr, nullptr);
    runMigrations(impl_->db);
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
        "duration_ms, sample_rate, channels, bit_depth, file_size, file_mtime) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?);";
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
        "duration_ms, sample_rate, channels, bit_depth, file_size, file_mtime FROM tracks;";
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
                      "display_name, quality, mode, country) VALUES (?,?,?,?,?,?,?);";
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
                      "display_name, quality, mode, country FROM albums;";
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

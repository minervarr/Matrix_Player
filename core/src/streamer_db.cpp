#include "core/streamer_db.h"
#include "arc/fs/paths.hh"
#include "sqlite3.h"
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

struct StreamerDb::Impl {
    sqlite3* db = nullptr;
    fs::path assetsRoot;  // parent of the .streamer/ dir; assets.rel_path is relative to this
};

StreamerDb::StreamerDb() = default;

StreamerDb::~StreamerDb() { close(); }

StreamerDb::StreamerDb(StreamerDb&& other) noexcept : impl_(other.impl_) {
    other.impl_ = nullptr;
}

StreamerDb& StreamerDb::operator=(StreamerDb&& other) noexcept {
    if (this != &other) {
        close();
        impl_ = other.impl_;
        other.impl_ = nullptr;
    }
    return *this;
}

// Normalized for comparison and for use as a cache key: lexically cleaned,
// with any trailing separator removed. "/music/", "/music" and "/music/./"
// are one directory and must produce one key, or the same library opens
// twice under two names.
static fs::path normalizeDir(const fs::path& p) {
    fs::path n = p.lexically_normal();
    const std::string s = n.u8string();
    if (s.size() > 1 && (s.back() == '/' || s.back() == '\\'))
        return fs::u8path(s.substr(0, s.size() - 1));
    return n;
}

std::vector<std::string> streamerSearchPath(const std::string& albumDir,
                                            const std::string& rootBound) {
    // The walk itself is arc::fs::ancestorsUpTo() — nearest-first up to the
    // bound and one past it, refusing to climb when the bound is not an
    // ancestor. It moved to archive_engine because it is not about this
    // downloader or about music: "find a companion tree without assuming how
    // deep it sits" is the same question for any app with a sidecar, and it is
    // pure path arithmetic that opens nothing.
    //
    // This name and signature stay because they say what the walk is FOR here,
    // and core/tests/streamer_db_test.cc asserts the three real layouts through
    // it.
    return arc::fs::ancestorsUpTo(albumDir, rootBound);
}

bool StreamerDb::openAt(const std::string& dir) {
    close();

    std::error_code ec;
    const fs::path assetsRoot = normalizeDir(fs::u8path(dir));
    const fs::path dbPath = assetsRoot / ".streamer" / "library.db";
    if (!fs::exists(dbPath, ec)) return false;

    impl_ = new Impl;
    impl_->assetsRoot = assetsRoot;
    if (sqlite3_open_v2(dbPath.u8string().c_str(), &impl_->db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        delete impl_;
        impl_ = nullptr;
        return false;
    }
    return true;
}

bool StreamerDb::isOpen() const { return impl_ && impl_->db; }

void StreamerDb::close() {
    if (impl_) {
        if (impl_->db) sqlite3_close(impl_->db);
        delete impl_;
        impl_ = nullptr;
    }
}

static std::string readWholeFileSmall(const fs::path& p, size_t maxBytes = 64 * 1024) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (s.size() > maxBytes) s.resize(maxBytes);
    return s;
}

// Single-column TEXT lookup: SELECT rel_path FROM assets WHERE kind=? AND artist_id=?
static std::string queryAssetRelPath(sqlite3* db, const char* kind, int artistId) {
    const char* sql = "SELECT rel_path FROM assets WHERE kind = ? AND artist_id = ? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, kind, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, artistId);
    std::string result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        auto* s = (const char*)sqlite3_column_text(stmt, 0);
        if (s) result = s;
    }
    sqlite3_finalize(stmt);
    return result;
}

std::optional<StreamerArtistInfo> StreamerDb::artistInfoForAlbum(const std::string& albumName) const {
    if (!isOpen()) return std::nullopt;

    const char* sql = "SELECT artist_id FROM albums WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, albumName.c_str(), -1, SQLITE_TRANSIENT);
    bool found = false;
    int artistId = 0;
    bool hasArtist = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        found = true;
        if (sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
            artistId = sqlite3_column_int(stmt, 0);
            hasArtist = true;
        }
    }
    sqlite3_finalize(stmt);
    if (!found) return std::nullopt;

    StreamerArtistInfo info;
    if (!hasArtist) return info;

    std::string bioRel = queryAssetRelPath(impl_->db, "artist_bio", artistId);
    if (!bioRel.empty()) info.bioText = readWholeFileSmall(impl_->assetsRoot / fs::u8path(bioRel));

    std::string imgRel = queryAssetRelPath(impl_->db, "artist_image", artistId);
    if (!imgRel.empty()) info.imagePath = (impl_->assetsRoot / fs::u8path(imgRel)).u8string();

    return info;
}

std::optional<StreamerAlbumInfo> StreamerDb::albumInfoForAlbum(const std::string& albumName) const {
    if (!isOpen()) return std::nullopt;

    const char* sql =
        "SELECT g.name, l.name, al.release_date_original, al.release_date_stream, "
        "al.upc, al.copyright, al.product_type, al.release_type, al.hires, "
        "al.maximum_bit_depth, al.maximum_sampling_rate, al.maximum_channel_count, "
        "al.tracks_count, al.duration "
        "FROM albums al "
        "LEFT JOIN genres g ON g.id = al.genre_id "
        "LEFT JOIN labels l ON l.id = al.label_id "
        "WHERE al.id = ?;";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, albumName.c_str(), -1, SQLITE_TRANSIENT);

    std::optional<StreamerAlbumInfo> out;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        auto col = [&](int i) -> std::string {
            auto* s = (const char*)sqlite3_column_text(stmt, i);
            return s ? s : "";
        };
        StreamerAlbumInfo a;
        a.genre               = col(0);
        a.label                = col(1);
        a.releaseDateOriginal  = col(2);
        a.releaseDateStream    = col(3);
        a.upc                  = col(4);
        a.copyright            = col(5);
        a.productType          = col(6);
        a.releaseType          = col(7);
        a.hires                = sqlite3_column_int(stmt, 8) != 0;
        a.maxBitDepth          = sqlite3_column_int(stmt, 9);
        a.maxSamplingRate      = sqlite3_column_double(stmt, 10);
        a.maxChannelCount      = sqlite3_column_int(stmt, 11);
        a.tracksCount          = sqlite3_column_int(stmt, 12);
        a.durationSec          = sqlite3_column_int(stmt, 13);
        out = std::move(a);
    }
    sqlite3_finalize(stmt);
    return out;
}

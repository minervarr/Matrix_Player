#pragma once
#include <string>
#include <optional>
#include <vector>

// Read-only reader for a third-party "streamer" library database. That
// database belongs to an external Qobuz-style download tool, not this app:
// its schema (artists/albums/tracks/labels/genres/files/assets) is foreign
// and is never written to here, never migrated, and never merged into Db's
// own schema. Absence of a match (no bio/image/extra metadata for a given
// album) is the expected common case, not an error — callers should treat
// empty results as "no data available."
//
// WHERE IT LIVES is the downloader's published contract, and it is stated in
// terms of its OWN root, never of ours:
//
//     <download_dir>/<country>/<album_id>/<track_id>.<format_id>.<ext>
//     <download_dir>/<country>/<album_id>/cover.jpg | album_description.txt
//     <download_dir>/.streamer/library.db
//     <download_dir>/.streamer/artists/<artist_id>/{artist.jpg,artist_bio.txt}
//
// Nothing says <download_dir> IS a music root of ours, and on a phone it is
// not: the downloader hardcodes <external>/Music/streamer while this app is
// seeded with <external>/Music, one level above. This class used to probe
// exactly two places — <root>/.streamer and <root>/../.streamer — which is a
// guess about depth, and that guess is what silently cost every artist bio
// and photo on Android while album descriptions (a sidecar INSIDE the album
// folder) kept working and hid it.
//
// So the depth is no longer assumed: streamerSearchPath() walks UP from the
// album's own folder and openAt() opens an already-resolved directory.
struct StreamerArtistInfo {
    std::string bioText;    // contents of artist_bio.txt sidecar, empty if none
    std::string imagePath;  // absolute path to artist.jpg sidecar, empty if none
};

// Recognized-but-not-yet-displayed album metadata, for future features.
// Every field may be empty/zero if the source column was null.
struct StreamerAlbumInfo {
    std::string genre;
    std::string label;
    std::string releaseDateOriginal;
    std::string releaseDateStream;
    std::string upc;
    std::string copyright;
    std::string productType;
    std::string releaseType;
    bool        hires           = false;
    int         maxBitDepth     = 0;
    double      maxSamplingRate = 0.0;
    int         maxChannelCount = 0;
    int         tracksCount     = 0;
    int         durationSec     = 0;
};

class StreamerDb {
public:
    StreamerDb();
    ~StreamerDb();
    StreamerDb(const StreamerDb&) = delete;
    StreamerDb& operator=(const StreamerDb&) = delete;
    StreamerDb(StreamerDb&& other) noexcept;
    StreamerDb& operator=(StreamerDb&& other) noexcept;

    // Opens <dir>/.streamer/library.db and NOTHING ELSE — no probing, no
    // second guess. `dir` is the downloader's <download_dir>, which the
    // caller has already resolved (see streamerSearchPath()); asset paths
    // are then read relative to it, because the db stores them that way
    // (".streamer/artists/<id>/artist.jpg").
    //
    // Returns false and stays closed when there is no such file — the normal
    // case for any library that does not use this layout, not an error.
    bool openAt(const std::string& dir);
    bool isOpen() const;
    void close();

    // Keyed by Album::name (== this DB's albums.id primary key: the
    // FR/<codename> folder name is the same string). Returns std::nullopt
    // only if the album id isn't found in `albums` at all; returns a
    // (possibly all-empty) StreamerArtistInfo if the album's artist simply
    // has no bio/image assets recorded (the common case).
    std::optional<StreamerArtistInfo> artistInfoForAlbum(const std::string& albumName) const;

    std::optional<StreamerAlbumInfo> albumInfoForAlbum(const std::string& albumName) const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

// Directories that may hold a ".streamer/library.db" for an album sitting at
// `albumDir`, NEAREST FIRST: albumDir itself, then each parent, up to and
// INCLUDING one level above `rootBound`.
//
// Pure path arithmetic — nothing is opened, nothing is stat'ed — which is the
// whole reason it can be asserted in core/tests/streamer_db_test.cc without a
// filesystem or a fixture library.
//
// Two ends, and both are deliberate:
//
//   * It starts at the ALBUM, not at the root, so a downloader library nested
//     any distance below a music root is found. That is the phone's case.
//   * It ends one level ABOVE rootBound rather than at it, because that is
//     exactly what the old two-probe open() covered: a listener whose music
//     root is <download_dir>/<country> rather than <download_dir>. Stopping
//     at rootBound would trade one bug for another.
//
// `rootBound` must be an ancestor of `albumDir` (a music root that contains
// it). When it is not — a caller with no roots loaded, a path from another
// volume — the result is albumDir alone, so a mis-call cannot climb to "/"
// and bind some unrelated database.
std::vector<std::string> streamerSearchPath(const std::string& albumDir,
                                            const std::string& rootBound);

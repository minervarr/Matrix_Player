#pragma once
#include <string>
#include <optional>

// Read-only reader for a third-party "streamer" library database
// (<music root>/.streamer/library.db, or its sibling <root>/../.streamer/
// library.db). That database belongs to an external Qobuz-style download
// tool, not this app: its schema (artists/albums/tracks/labels/genres/
// files/assets) is foreign and is never written to here, never migrated,
// and never merged into Db's own schema. Absence of a match (no bio/
// image/extra metadata for a given album) is the expected common case,
// not an error — callers should treat empty results as "no data available."
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

    // Looks for library.db at musicRoot/.streamer/library.db, then at
    // musicRoot/../.streamer/library.db (covers both "root = the folder
    // containing FR/ and .streamer/" and "root = FR/ itself" layouts).
    // Returns false and stays closed if neither exists — that's the normal
    // case for any library that doesn't use this layout, not an error.
    bool open(const std::string& musicRoot);
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

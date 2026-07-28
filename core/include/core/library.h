#pragma once
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>

struct Track {
    int         id = 0;
    std::string title;
    std::string artist;
    std::string albumArtist;
    std::string album;
    std::string filePath;
    int         trackNumber = 0;
    int         durationMs  = 0;
    int         sampleRate  = 0;
    int         channels    = 0;
    int         bitDepth    = 0;
    int64_t     fileSize    = 0;
    int64_t     fileMtime   = 0;
};

struct Album {
    std::string name;         // raw folder name — stable key (track↔album join,
                              // last-played matching, DB dedup); never render it
    std::string artist;
    std::string artPath;
    // Structured fields parsed from the library's fixed folder convention
    // (<root>[/<country>]/<Artist>/<Album> (<bit>-<rate>)/ — see downloader):
    std::string displayName;  // name with the "(24-96)"-style suffix stripped
    std::string quality;      // folder suffix, e.g. "24-96" ("" if none)
    std::string mode;         // "album" | "single" (Singles/ subfolder)
    std::string country;      // optional country dir ("" if absent)
    std::vector<Track> tracks;

    // Release-type classification (Album/EP/Single/Remix) and quality-tier
    // inputs, computed once in buildAlbums() and cached in Db's own albums
    // table (never touches the external streamer db) — see
    // classifyReleaseType()/computeAlbumQualityStats() below.
    enum class ReleaseType { Album = 0, Ep = 1, Single = 2, Remix = 3 };
    ReleaseType releaseType   = ReleaseType::Album;
    int         avgSampleRate = 0;
    bool        hasDsd        = false;

    void sortTracks();
};

// Classifies a release from its track list, mirroring the sibling Android
// player's AlbumDao.classifyRelease() exactly: track-count thresholds
// (1=Single, 2-4=EP, >4=Album), overridden by remix detection (album name
// or a strict majority of track titles matching remix patterns).
Album::ReleaseType classifyReleaseType(const std::string& albumName,
                                       const std::vector<Track>& tracks);

// Mean sample rate across tracks with sampleRate > 0 (0 if none), and
// whether any track is DSD. hasDsd is always false today — this app has no
// DSD/DSF file decoding yet (see root CLAUDE.md's "Not yet wired: DoP") —
// the field exists for forward compatibility with the quality-color palette.
void computeAlbumQualityStats(const std::vector<Track>& tracks,
                              int& avgSampleRate, bool& hasDsd);

std::vector<Album> scanLibrary(const std::string& rootPath);

struct IncrementalScanResult {
    std::vector<Album> albums;
    int filesScanned  = 0;
    int filesSkipped  = 0;
    int filesRemoved  = 0;
};

struct FileCache {
    int64_t fileSize  = 0;
    int64_t fileMtime = 0;
};

IncrementalScanResult scanLibraryIncremental(
    const std::string& rootPath,
    const std::map<std::string, FileCache>& existing);

std::vector<Album> scanLibraryParallel(const std::string& rootPath);

std::string resolveArtPath(const std::string& folderPath);

// Fills album.displayName/quality/mode/country from the folder path per the
// downloader's fixed layout. Non-conforming folders degrade gracefully
// (displayName = name, mode = "album").
void parseAlbumFolder(const std::string& folderPath,
                      const std::string& rootPath, Album& album);

void purgeStaleFiles(std::vector<Album>& albums, int& removedCount);

// Recursive directory-tree watcher with a ~500ms coalescing debounce.
// Platform backend (ReadDirectoryChangesW on Windows, inotify on Linux) is
// hidden behind this opaque Impl so the header stays OS-header-free; see
// core/src/os/windows_folder_watch.cpp / linux_folder_watch.cpp.
class FolderWatcher {
public:
    using Callback = std::function<void(const std::string& root)>;

    FolderWatcher();
    ~FolderWatcher();

    void watchRoot(const std::string& path, Callback cb);
    void unwatchRoot(const std::string& path);
    void unwatchAll();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

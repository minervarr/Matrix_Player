#pragma once
#include <cstdint>
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
    // 0 = the file carried no DISCNUMBER (single-disc release, or untagged).
    // Multi-disc albums number their tracks from 1 on EVERY disc, so this is
    // the primary sort key — without it the discs interleave. See sortTracks().
    int         discNumber  = 0;
    int         durationMs  = 0;
    int         sampleRate  = 0;
    int         channels    = 0;
    int         bitDepth    = 0;
    int64_t     fileSize    = 0;
    int64_t     fileMtime   = 0;

    // Analytics inputs. Both come from Vorbis comments only — nothing reads
    // ID3 yet, so MP3s carry neither (documented in TODO.md, not a blocker).
    std::string genre;
    int         year        = 0;   // 0 = untagged; parsed from DATE/YEAR

    // Stable listening identity — see trackKey() in core/variants.h. Computed
    // during the scan and stored in the tracks table; play_events is keyed on
    // it rather than on filePath, which changes whenever a folder is renamed.
    std::string trackKey;
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
    // Stored as an integer in Db's albums table, so values are APPENDED and
    // never reordered — renumbering would relabel every row already on disk.
    enum class ReleaseType { Album = 0, Ep = 1, Single = 2, Remix = 3,
                             Compilation = 4, Live = 5 };
    ReleaseType releaseType   = ReleaseType::Album;
    int         avgSampleRate = 0;
    bool        hasDsd        = false;

    void sortTracks();
};

// classifyReleaseType() (Album/EP/Single/Remix/Compilation) is declared in
// core/variants.h — it moved there with the rest of the pure name/release
// logic so core/tests/variants_test.cc could link and test it.

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

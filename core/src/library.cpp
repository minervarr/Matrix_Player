#include "core/library.h"
#ifdef _WIN32
#include <windows.h>
#endif
#include <algorithm>
#include <filesystem>
#include <system_error>
#include <map>
#include <cctype>
#include <cstdio>
#include <future>
#include <thread>
#include <regex>
#include "dr_flac.h"
#include "dr_wav.h"

namespace fs = std::filesystem;

// ── Release-type classification (Album/EP/Single/Remix) ─────────────────────
// Ported verbatim from the sibling Android player's AlbumDao.java
// (isRemixTrack/isRemixAlbum/classifyRelease) — see the design spec at
// docs/superpowers/specs/2026-07-27-release-type-and-quality-color-design.md.
namespace {

bool isRemixTrackTitle(const std::string& title) {
    if (title.empty()) return false;
    std::string lower = title;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                    [](unsigned char c) { return std::tolower(c); });
    size_t b = lower.find_first_not_of(" \t");
    size_t e = lower.find_last_not_of(" \t");
    std::string trimmed = (b == std::string::npos) ? "" : lower.substr(b, e - b + 1);
    if (trimmed == "remix" || trimmed == "mix" ||
        trimmed == "the remix" || trimmed == "the mix") return false;
    if (lower.find("remix") != std::string::npos) return true;
    if (lower.find("rmx") != std::string::npos) return true;
    static const std::regex kWordMix(R"(\b\w+\s+mix\b)", std::regex::icase);
    static const std::regex kParenMix(R"(\(.*mix.*\))", std::regex::icase);
    static const std::regex kBracketMix(R"(\[.*mix.*\])", std::regex::icase);
    if (std::regex_search(title, kWordMix))    return true;
    if (std::regex_search(title, kParenMix))   return true;
    if (std::regex_search(title, kBracketMix)) return true;
    return false;
}

bool isRemixAlbum(const std::string& albumName, const std::vector<Track>& tracks) {
    static const std::regex kRemixName(R"(\b(remix|remixes|remixed|rmx)\b)", std::regex::icase);
    if (!albumName.empty() && std::regex_search(albumName, kRemixName)) return true;
    int trackCount = (int)tracks.size();
    if (trackCount == 0) return false;
    int remixCount = 0;
    for (auto& t : tracks)
        if (isRemixTrackTitle(t.title)) remixCount++;
    return remixCount == trackCount || (remixCount >= 2 && remixCount * 2 > trackCount);
}

} // namespace

Album::ReleaseType classifyReleaseType(const std::string& albumName,
                                       const std::vector<Track>& tracks) {
    if (isRemixAlbum(albumName, tracks)) return Album::ReleaseType::Remix;
    int trackCount = (int)tracks.size();
    if (trackCount == 1) return Album::ReleaseType::Single;
    if (trackCount <= 4) return Album::ReleaseType::Ep;
    return Album::ReleaseType::Album;
}

void computeAlbumQualityStats(const std::vector<Track>& tracks,
                              int& avgSampleRate, bool& hasDsd) {
    long long sum = 0;
    int count = 0;
    for (auto& t : tracks) {
        if (t.sampleRate > 0) { sum += t.sampleRate; count++; }
    }
    avgSampleRate = count > 0 ? (int)(sum / count) : 0;
    hasDsd = false;  // no DSD/DSF decode yet — see the declaration's comment
}

static const char* COVER_NAMES[] = { "cover", "folder", "front", "albumart", "album" };
static const char* COVER_EXTS[]  = { ".jpg", ".jpeg", ".png" };

std::string resolveArtPath(const std::string& folderPath) {
    fs::path dir = fs::u8path(folderPath);
    // Pass 1: preferred names in priority order
    for (const char* name : COVER_NAMES) {
        for (const char* ext : COVER_EXTS) {
            fs::path candidate = dir / (std::string(name) + ext);
            if (fs::exists(candidate)) return candidate.u8string();
        }
    }
    // Pass 2: pick the first image found (don't require exactly one)
    std::error_code ec;
    for (auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().u8string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".jpg" || ext == ".jpeg" || ext == ".png")
            return entry.path().u8string();
    }
    return "";
}

static std::string deriveAlbumArtist(const std::vector<Track>& tracks) {
    std::string result;
    // Prefer ALBUMARTIST tags — check all tracks for consistency
    for (auto& t : tracks) {
        if (t.albumArtist.empty()) continue;
        if (result.empty()) result = t.albumArtist;
        else if (result != t.albumArtist) return "Various Artists";
    }
    if (!result.empty()) return result;
    // Fall back to ARTIST — if tracks disagree, it's a various-artists album
    for (auto& t : tracks) {
        if (t.artist.empty()) continue;
        if (result.empty()) result = t.artist;
        else if (result != t.artist) return "Various Artists";
    }
    return result;
}

void Album::sortTracks() {
    std::sort(tracks.begin(), tracks.end(), [](const Track& a, const Track& b) {
        if (a.trackNumber != b.trackNumber) return a.trackNumber < b.trackNumber;
        return a.title < b.title;
    });
}

// ── Folder-convention parsing ────────────────────────────────────────────────
// The library is laid out by the downloader tool in a fixed shape:
//   <root>[/<country>]/<Artist>/<Album> (<bit>-<rate>)/*.flac
//   <root>[/<country>]/<Artist>/Singles/<Title>/*.flac
// e.g. "Motor Y Motivo (24-48)", "Disco de Oro (16-44.1)". The suffix encodes
// the download quality; strip it for display and keep it as a structured
// field so SQL queries can filter on it.

void parseAlbumFolder(const std::string& folderPath,
                      const std::string& rootPath, Album& album) {
    album.displayName = album.name;
    album.mode        = "album";

    // Trailing " (bit-rate)" quality suffix, e.g. " (24-96)" / " (16-44.1)".
    static const std::regex kQualityRe(R"( \((\d{1,2}-\d{1,3}(?:\.\d)?)\)$)");
    std::smatch m;
    if (std::regex_search(album.name, m, kQualityRe)) {
        album.quality     = m[1].str();
        album.displayName = album.name.substr(0, m.position(0));
    }

    // Path components relative to the scan root: [country/]Artist[/Singles]/Album
    if (rootPath.empty()) return;  // no root known — suffix strip is all we can do
    std::error_code ec;
    fs::path rel = fs::relative(fs::u8path(folderPath), fs::u8path(rootPath), ec);
    if (ec || rel.empty()) return;
    std::vector<std::string> parts;
    for (auto& p : rel) parts.push_back(p.u8string());
    if (parts.size() >= 2 && parts[parts.size() - 2] == "Singles")
        album.mode = "single";
    // country/Artist/Album (3 deep) or country/Artist/Singles/Title (4 deep)
    if (parts.size() == 3 || (parts.size() == 4 && album.mode == "single"))
        album.country = parts[0];
}

// Shared tail of every scan flavor: fold the per-folder track lists into
// sorted Album entries. Was duplicated inline across scanLibrary /
// scanLibraryIncremental / scanLibraryParallel.
static std::vector<Album> buildAlbums(
    std::map<std::string, std::vector<Track>>& byFolder,
    const std::string& rootPath)
{
    std::vector<Album> albums;
    for (auto& [folder, tracks] : byFolder) {
        Album album;
        album.name   = fs::u8path(folder).filename().u8string();
        album.tracks = std::move(tracks);
        album.artPath = resolveArtPath(folder);
        album.artist = deriveAlbumArtist(album.tracks);
        parseAlbumFolder(folder, rootPath, album);
        // Prefer the ALBUM tag over the folder-derived name whenever the
        // tracks agree on one: folder names in this library carry encoding
        // damage (mangled UTF-8, substituted characters) that the embedded
        // metadata doesn't have. The folder name stays in `name` as the
        // stable key; only the display string switches.
        std::string metaAlbum;
        for (auto& t : album.tracks) {
            if (t.album.empty()) continue;
            if (metaAlbum.empty()) metaAlbum = t.album;
            else if (metaAlbum != t.album) { metaAlbum.clear(); break; }
        }
        if (!metaAlbum.empty()) album.displayName = metaAlbum;
        album.sortTracks();
        albums.push_back(std::move(album));
    }
    std::sort(albums.begin(), albums.end(),
        [](const Album& a, const Album& b){ return a.displayName < b.displayName; });
    return albums;
}

// ── FLAC metadata parsing ────────────────────────────────────────────────────

struct VorbisCtx {
    std::string title, artist, albumArtist, album;
    int trackNumber = 0;
};

static void onFlacMeta(void* userdata, drflac_metadata* meta) {
    if (meta->type != DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT) return;
    auto* ctx = (VorbisCtx*)userdata;
    drflac_vorbis_comment_iterator iter;
    drflac_init_vorbis_comment_iterator(&iter,
        meta->data.vorbis_comment.commentCount,
        meta->data.vorbis_comment.pComments);
    drflac_uint32 len;
    const char* comment;
    while ((comment = drflac_next_vorbis_comment(&iter, &len)) != nullptr) {
        std::string s(comment, len);
        auto eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string key = s.substr(0, eq);
        std::string val = s.substr(eq + 1);
        for (auto& c : key) c = (char)toupper((unsigned char)c);
        if (key == "TITLE")        ctx->title       = val;
        else if (key == "ARTIST")  ctx->artist      = val;
        else if (key == "ALBUMARTIST" || key == "ALBUM ARTIST")
                                   ctx->albumArtist = val;
        else if (key == "ALBUM")   ctx->album       = val;
        else if (key == "TRACKNUMBER") {
            // Handle "3/12" format
            auto slash = val.find('/');
            ctx->trackNumber = atoi(slash != std::string::npos ? val.substr(0, slash).c_str() : val.c_str());
        }
    }
}

// Windows keeps the exact FILETIME-tick value (100ns since 1601) it always
// has; Linux uses filesystem::last_write_time's native clock tick count.
// Neither is ever compared across platforms — only against a cache value
// this same machine wrote on a prior scan — so the differing epoch/units
// are safe.
static void statSizeAndMtime(const std::string& path, int64_t& outSize, int64_t& outMtime) {
#ifdef _WIN32
    int wl = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring wpath(wl, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), wl);
    if (!wpath.empty() && wpath.back() == L'\0') wpath.pop_back();

    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(wpath.c_str(), GetFileExInfoStandard, &fad)) {
        LARGE_INTEGER sz;
        sz.HighPart = fad.nFileSizeHigh;
        sz.LowPart  = fad.nFileSizeLow;
        outSize     = sz.QuadPart;
        LARGE_INTEGER mt;
        mt.HighPart = fad.ftLastWriteTime.dwHighDateTime;
        mt.LowPart  = fad.ftLastWriteTime.dwLowDateTime;
        outMtime    = mt.QuadPart;
    }
#else
    std::error_code ec;
    fs::path p = fs::u8path(path);
    auto sz = fs::file_size(p, ec);
    outSize = ec ? 0 : static_cast<int64_t>(sz);
    auto ftime = fs::last_write_time(p, ec);
    outMtime = ec ? 0 : ftime.time_since_epoch().count();
#endif
}

static Track quickParseWAV(const std::string& path) {
    Track t;
    t.filePath = path;
    t.title = fs::path(fs::u8path(path)).stem().u8string();

    statSizeAndMtime(path, t.fileSize, t.fileMtime);

    drwav wav;
#ifdef _WIN32
    int wl = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring wpath(wl, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), wl);
    if (!wpath.empty() && wpath.back() == L'\0') wpath.pop_back();
    bool opened = drwav_init_file_w(&wav, wpath.c_str(), nullptr);
#else
    bool opened = drwav_init_file(&wav, path.c_str(), nullptr);
#endif
    if (opened) {
        t.sampleRate = (int)wav.sampleRate;
        t.channels   = (int)wav.channels;
        t.bitDepth   = (int)wav.bitsPerSample;
        if (wav.sampleRate > 0 && wav.totalPCMFrameCount > 0)
            t.durationMs = (int)(wav.totalPCMFrameCount * 1000 / wav.sampleRate);
        drwav_uninit(&wav);
    }
    return t;
}

static Track quickParseFLAC(const std::string& path) {
    Track t;
    t.filePath = path;
    t.title = fs::path(fs::u8path(path)).stem().u8string();

    statSizeAndMtime(path, t.fileSize, t.fileMtime);

    VorbisCtx ctx;
#ifdef _WIN32
    int wl = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring wpath(wl, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), wl);
    if (!wpath.empty() && wpath.back() == L'\0') wpath.pop_back();
    drflac* flac = drflac_open_file_with_metadata_w(wpath.c_str(), onFlacMeta, &ctx, nullptr);
#else
    drflac* flac = drflac_open_file_with_metadata(path.c_str(), onFlacMeta, &ctx, nullptr);
#endif
    if (flac) {
        t.sampleRate = (int)flac->sampleRate;
        t.channels   = (int)flac->channels;
        t.bitDepth   = (int)flac->bitsPerSample;
        if (flac->sampleRate > 0 && flac->totalPCMFrameCount > 0)
            t.durationMs = (int)(flac->totalPCMFrameCount * 1000 / flac->sampleRate);
        if (!ctx.title.empty())       t.title       = ctx.title;
        if (!ctx.artist.empty())      t.artist      = ctx.artist;
        if (!ctx.albumArtist.empty()) t.albumArtist = ctx.albumArtist;
        if (!ctx.album.empty())       t.album       = ctx.album;
        t.trackNumber = ctx.trackNumber;
        drflac_close(flac);
    }
    return t;
}

// ── Full scan (original interface, kept for compatibility) ───────────────────

std::vector<Album> scanLibrary(const std::string& rootPath) {
    std::vector<Album> albums;
    fs::path root = fs::u8path(rootPath);
    if (!fs::exists(root)) return albums;

    std::map<std::string, std::vector<Track>> byFolder;
    for (auto& entry : fs::recursive_directory_iterator(root,
            fs::directory_options::skip_permission_denied)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().u8string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        std::string folder = entry.path().parent_path().u8string();
        std::string filePath = entry.path().u8string();
        if (ext == ".flac")
            byFolder[folder].push_back(quickParseFLAC(filePath));
        else if (ext == ".wav")
            byFolder[folder].push_back(quickParseWAV(filePath));
    }

    albums = buildAlbums(byFolder, rootPath);
    return albums;
}

// ── Incremental scan ─────────────────────────────────────────────────────────

IncrementalScanResult scanLibraryIncremental(
    const std::string& rootPath,
    const std::map<std::string, FileCache>& existing)
{
    IncrementalScanResult result;
    fs::path root = fs::u8path(rootPath);
    if (!fs::exists(root)) return result;

    std::map<std::string, std::vector<Track>> byFolder;

    for (auto& entry : fs::recursive_directory_iterator(root,
            fs::directory_options::skip_permission_denied)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().u8string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext != ".flac" && ext != ".wav") continue;

        std::string filePath = entry.path().u8string();
        std::string folder   = entry.path().parent_path().u8string();

        // Check if file is unchanged
        auto it = existing.find(filePath);
        if (it != existing.end()) {
            int64_t sz = 0, mt = 0;
            statSizeAndMtime(filePath, sz, mt);
            if (sz == it->second.fileSize && mt == it->second.fileMtime) {
                result.filesSkipped++;
                continue;
            }
        }

        result.filesScanned++;
        if (ext == ".flac")
            byFolder[folder].push_back(quickParseFLAC(filePath));
        else if (ext == ".wav")
            byFolder[folder].push_back(quickParseWAV(filePath));
    }

    result.albums = buildAlbums(byFolder, rootPath);
    return result;
}

// ── Parallel scan ────────────────────────────────────────────────────────────

std::vector<Album> scanLibraryParallel(const std::string& rootPath) {
    fs::path root = fs::u8path(rootPath);
    if (!fs::exists(root)) return {};

    // Collect all audio file paths first
    std::vector<std::pair<std::string, std::string>> files; // (path, ext)
    for (auto& entry : fs::recursive_directory_iterator(root,
            fs::directory_options::skip_permission_denied)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().u8string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".flac" || ext == ".wav")
            files.emplace_back(entry.path().u8string(), ext);
    }

    // Parse in parallel using hardware concurrency
    unsigned numThreads = std::thread::hardware_concurrency();
    if (numThreads == 0) numThreads = 4;

    std::vector<std::future<std::vector<Track>>> futures;
    size_t chunkSize = (files.size() + numThreads - 1) / numThreads;

    for (unsigned i = 0; i < numThreads; i++) {
        size_t start = i * chunkSize;
        size_t end   = std::min(start + chunkSize, files.size());
        if (start >= files.size()) break;

        futures.push_back(std::async(std::launch::async, [&files, start, end]() {
            std::vector<Track> tracks;
            for (size_t j = start; j < end; j++) {
                if (files[j].second == ".flac")
                    tracks.push_back(quickParseFLAC(files[j].first));
                else
                    tracks.push_back(quickParseWAV(files[j].first));
            }
            return tracks;
        }));
    }

    // Gather results and group by folder
    std::map<std::string, std::vector<Track>> byFolder;
    for (auto& f : futures) {
        auto tracks = f.get();
        for (auto& t : tracks) {
            std::string folder = fs::u8path(t.filePath).parent_path().u8string();
            byFolder[folder].push_back(std::move(t));
        }
    }

    return buildAlbums(byFolder, rootPath);
}

// ── Stale file cleanup ───────────────────────────────────────────────────────

void purgeStaleFiles(std::vector<Album>& albums, int& removedCount) {
    removedCount = 0;
    for (auto& album : albums) {
        auto it = std::remove_if(album.tracks.begin(), album.tracks.end(),
            [&](const Track& t) {
                if (!fs::exists(fs::u8path(t.filePath))) { removedCount++; return true; }
                return false;
            });
        album.tracks.erase(it, album.tracks.end());
    }
    // Remove empty albums
    albums.erase(
        std::remove_if(albums.begin(), albums.end(),
            [](const Album& a) { return a.tracks.empty(); }),
        albums.end());
}

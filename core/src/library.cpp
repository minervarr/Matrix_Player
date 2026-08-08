#include "core/library.h"
#include "core/variants.h"
#ifdef _WIN32
#include <windows.h>
#endif
#include <algorithm>
#include <filesystem>
#include <system_error>
#include <map>
#include <cstdio>
#include <future>
#include <string_view>
#include <thread>
// Tag/stream-info reading uses libFLAC's metadata API — the same library that
// decodes playback (audio_engine's backends/flac), so the scan and the decoder
// can never disagree about a file. dr_wav stays for WAV, which libFLAC has no
// opinion about.
#include <FLAC/metadata.h>
#include "dr_wav.h"

namespace fs = std::filesystem;

// ASCII character classes, constexpr and locale-independent — same reasoning as
// the block at the top of variants.cpp. <cctype>'s versions read the global
// locale through a table pointer, and this file calls them once per byte of
// every tag in the library.
namespace {
constexpr bool asciiDigit(unsigned char c) { return c >= '0' && c <= '9'; }
constexpr bool asciiSpace(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}
constexpr char asciiLower(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ? char(c + 32) : char(c);
}
constexpr char asciiUpper(unsigned char c) {
    return (c >= 'a' && c <= 'z') ? char(c - 32) : char(c);
}
constexpr bool iequalsUpper(std::string_view s, std::string_view upperLit) {
    if (s.size() != upperLit.size()) return false;
    for (size_t i = 0; i < s.size(); i++)
        if (asciiUpper((unsigned char)s[i]) != upperLit[i]) return false;
    return true;
}
void lowerAscii(std::string& s) {
    for (char& c : s) c = asciiLower((unsigned char)c);
}
} // namespace

// Release-type classification (Album/EP/Single/Remix) moved to
// core/src/variants.cpp — it is pure string/Track logic, and living beside
// the variant grouping that consumes it means core/tests/variants_test.cc
// links it directly. Declared in core/variants.h.

void computeAlbumQualityStats(const std::vector<Track>& tracks,
                              int& avgSampleRate, bool& hasDsd) {
    // Plain arithmetic mean, ported as-is from the Android reference's SQL
    // AVG(). A mixed-rate album (e.g. 44.1kHz + 96kHz tracks) can average
    // into a different tier than any single track actually has — this is a
    // known, deliberate property of the ported logic, not a rounding bug.
    long long sum = 0;
    int count = 0;
    for (auto& t : tracks) {
        if (t.sampleRate > 0) { sum += t.sampleRate; count++; }
    }
    avgSampleRate = count > 0 ? (int)(sum / count) : 0;
    hasDsd = false;  // no DSD/DSF decode yet — see the declaration's comment
}

static constexpr std::string_view COVER_NAMES[] = {
    "cover", "folder", "front", "albumart", "album" };
static constexpr std::string_view COVER_EXTS[] = { ".jpg", ".jpeg", ".png" };

std::string resolveArtPath(const std::string& folderPath) {
    const fs::path dir = fs::u8path(folderPath);
    // Pass 1: preferred names in priority order. One reused buffer and one
    // reused path across the 15 candidates, rather than building both afresh
    // each time — this runs once per album in the library.
    std::string leaf;
    fs::path candidate = dir / "x";
    for (const std::string_view name : COVER_NAMES) {
        for (const std::string_view ext : COVER_EXTS) {
            leaf.assign(name);
            leaf.append(ext);
            candidate.replace_filename(leaf);
            if (fs::exists(candidate)) return candidate.u8string();
        }
    }
    // Pass 2: pick the first image found (don't require exactly one)
    std::error_code ec;
    for (auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        std::string ext = entry.path().extension().u8string();
        lowerAscii(ext);
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
        // Disc first: every disc of a multi-disc release restarts its track
        // numbering at 1, so sorting by trackNumber alone interleaves them
        // (two "1"s, two "2"s, ...). Untagged files (discNumber 0) sort ahead
        // of disc 1, which for a single-disc album is every file — no change.
        if (a.discNumber != b.discNumber)   return a.discNumber < b.discNumber;
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

// The trailing " (bit-rate)" quality suffix, e.g. " (24-96)" / " (16-44.1)".
// Returns the offset of the leading SPACE (so the caller can cut the display
// name there) and fills `quality` with the text between the parens, or npos.
//
// Replaces the regex ` \((\d{1,2}-\d{1,3}(?:\.\d)?)\)$` — the last <regex> use
// in this file, and worth removing on its own: <regex> compiled to hundreds of
// kilobytes of object code here and this is the only shape it ever matched.
//
// Scanning back to the LAST '(' is exact rather than a heuristic: the suffix's
// contents are digits, '-' and '.' only, so the '(' that opens a valid suffix
// is necessarily the final '(' in the string.
static size_t parseQualitySuffix(const std::string& s, std::string& quality) {
    const size_t n = s.size();
    if (n < 6 || s[n - 1] != ')') return std::string::npos;   // shortest is " (0-0)"
    const size_t open = s.rfind('(');
    if (open == std::string::npos || open == 0 || s[open - 1] != ' ')
        return std::string::npos;

    const size_t close = n - 1;
    size_t i = open + 1;
    const size_t bitStart = i;
    while (i < close && asciiDigit((unsigned char)s[i])) i++;
    const size_t bitLen = i - bitStart;                        // \d{1,2}
    if (bitLen < 1 || bitLen > 2) return std::string::npos;
    if (i >= close || s[i] != '-') return std::string::npos;
    i++;
    const size_t rateStart = i;
    while (i < close && asciiDigit((unsigned char)s[i])) i++;
    const size_t rateLen = i - rateStart;                      // \d{1,3}
    if (rateLen < 1 || rateLen > 3) return std::string::npos;
    if (i < close && s[i] == '.') {                            // (?:\.\d)?
        i++;
        if (i >= close || !asciiDigit((unsigned char)s[i])) return std::string::npos;
        i++;
    }
    if (i != close) return std::string::npos;                  // the '$' anchor

    quality.assign(s, open + 1, close - open - 1);
    return open - 1;
}

void parseAlbumFolder(const std::string& folderPath,
                      const std::string& rootPath, Album& album) {
    album.displayName = album.name;
    album.mode        = "album";

    std::string quality;
    const size_t at = parseQualitySuffix(album.name, quality);
    if (at != std::string::npos) {
        album.quality = std::move(quality);
        album.displayName.resize(at);
    }

    // Path components relative to the scan root: [country/]Artist[/Singles]/Album
    if (rootPath.empty()) return;  // no root known — suffix strip is all we can do
    std::error_code ec;
    fs::path rel = fs::relative(fs::u8path(folderPath), fs::u8path(rootPath), ec);
    if (ec || rel.empty()) return;
    std::vector<std::string> parts;
    for (const auto& p : rel) parts.push_back(p.u8string());
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
        album.releaseType = classifyReleaseType(album.displayName, album.tracks);
        computeAlbumQualityStats(album.tracks, album.avgSampleRate, album.hasDsd);

        // The listening identity is computed HERE — the one place every scan
        // flavour funnels through, and the first point at which a thinly
        // tagged track can borrow the album's derived name and artist. Without
        // that fallback a folder of untagged files would hand every track the
        // same key and merge their histories into one.
        //
        // displayName, not name: the folder's "(24-96)" quality suffix has
        // already been stripped out of it, so re-ripping an album at a higher
        // rate does not mint a new identity for every track on it.
        //
        // The fallbacks are applied to the track ITSELF and then undone, rather
        // than to a copy: a Track holds seven std::strings, and copying every
        // one of them per file only to read four fields back was the single
        // largest allocation source in the scan. trackKey() reads no state this
        // touches, so the observable result is identical.
        for (Track& t : album.tracks) {
            const bool borrowedAlbum  = t.album.empty();
            const bool borrowedArtist = t.albumArtist.empty();
            if (borrowedAlbum)  t.album       = album.displayName;
            if (borrowedArtist) t.albumArtist = album.artist;
            t.trackKey = trackKey(t);
            if (borrowedAlbum)  t.album.clear();
            if (borrowedArtist) t.albumArtist.clear();
        }

        album.sortTracks();
        albums.push_back(std::move(album));
    }
    std::sort(albums.begin(), albums.end(),
        [](const Album& a, const Album& b){ return a.displayName < b.displayName; });
    return albums;
}

// ── FLAC metadata parsing ────────────────────────────────────────────────────

struct VorbisCtx {
    std::string title, artist, albumArtist, album, genre;
    int trackNumber = 0;
    int discNumber  = 0;
    int year        = 0;
};

// TRACKNUMBER/DISCNUMBER are both written either bare ("3") or as a
// position/total pair ("3/12") — take the part before the slash. Parsed in
// place; this used to build a substring per tag just to hand it to atoi.
// atoi's own semantics are kept exactly: leading whitespace, an optional sign,
// then digits, and 0 for anything unparseable.
static int parsePositionTag(std::string_view val) {
    const size_t slash = val.find('/');
    if (slash != std::string_view::npos) val = val.substr(0, slash);
    size_t i = 0;
    while (i < val.size() && asciiSpace((unsigned char)val[i])) i++;
    bool neg = false;
    if (i < val.size() && (val[i] == '+' || val[i] == '-')) neg = val[i++] == '-';
    long long v = 0;
    while (i < val.size() && asciiDigit((unsigned char)val[i])) {
        v = v * 10 + (val[i++] - '0');
        if (v > 1'000'000'000) break;      // tag junk; atoi would overflow anyway
    }
    return (int)(neg ? -v : v);
}

// DATE is written as a bare year ("1991"), a full ISO date ("1991-03-25"), or
// occasionally something looser. Take the first run of exactly four digits and
// accept it only as a plausible release year — a malformed tag should leave
// the field untagged (0) rather than poison a decade histogram.
static int parseYearTag(std::string_view val) {
    for (size_t i = 0; i + 4 <= val.size(); i++) {
        if (!asciiDigit((unsigned char)val[i])) continue;
        size_t run = 0;
        while (i + run < val.size() && asciiDigit((unsigned char)val[i + run])) run++;
        if (run == 4) {
            // Exactly four digits, so the value is direct arithmetic — no
            // substring and no atoi.
            const int y = (val[i]     - '0') * 1000 + (val[i + 1] - '0') * 100 +
                          (val[i + 2] - '0') * 10   + (val[i + 3] - '0');
            if (y >= 1000 && y <= 2999) return y;
        }
        i += run;   // -1 not needed: the loop's ++ lands past the digit run
    }
    return 0;
}

// One VORBIS_COMMENT entry, "KEY=value" with the key case-insensitive.
// The key is compared as a view against upper-case literals rather than being
// copied and upper-cased first, and only the values actually kept are
// materialised — this runs for every tag of every file in the library.
static void applyVorbisComment(VorbisCtx* ctx, const char* entry, unsigned length) {
    const std::string_view s(entry, length);
    const size_t eq = s.find('=');
    if (eq == std::string_view::npos) return;
    const std::string_view key = s.substr(0, eq);
    const std::string_view val = s.substr(eq + 1);
    if (iequalsUpper(key, "TITLE"))        ctx->title.assign(val);
    else if (iequalsUpper(key, "ARTIST"))  ctx->artist.assign(val);
    else if (iequalsUpper(key, "ALBUMARTIST") || iequalsUpper(key, "ALBUM ARTIST"))
                                           ctx->albumArtist.assign(val);
    else if (iequalsUpper(key, "ALBUM"))   ctx->album.assign(val);
    else if (iequalsUpper(key, "TRACKNUMBER")) ctx->trackNumber = parsePositionTag(val);
    else if (iequalsUpper(key, "DISCNUMBER"))  ctx->discNumber  = parsePositionTag(val);
    else if (iequalsUpper(key, "GENRE"))   ctx->genre.assign(val);
    // DATE is the standard field and ORIGINALDATE the reissue's original; YEAR
    // is non-standard but common. Prefer ORIGINALDATE when both are present —
    // "which year is this music from" is the question analytics asks, and on a
    // remaster DATE answers with the year of the reissue instead.
    else if (key == "ORIGINALDATE") ctx->year = parseYearTag(val);
    else if ((key == "DATE" || key == "YEAR") && ctx->year == 0)
                               ctx->year = parseYearTag(val);
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
    // One pass over the metadata blocks for both STREAMINFO and the tags —
    // the simple iterator opens the file once, where the two convenience
    // calls (FLAC__metadata_get_streaminfo + _get_tags) would open it twice.
    // That matters: this runs per file across the whole library.
    //
    // libFLAC takes the filename as UTF-8 on every platform (it converts to
    // wide internally on Windows), so no wpath dance here.
    FLAC__Metadata_SimpleIterator* it = FLAC__metadata_simple_iterator_new();
    if (it) {
        if (FLAC__metadata_simple_iterator_init(it, path.c_str(),
                                                /*read_only=*/true,
                                                /*preserve_file_stats=*/true)) {
            do {
                FLAC__MetadataType type = FLAC__metadata_simple_iterator_get_block_type(it);
                if (type != FLAC__METADATA_TYPE_STREAMINFO &&
                    type != FLAC__METADATA_TYPE_VORBIS_COMMENT) continue;
                FLAC__StreamMetadata* block = FLAC__metadata_simple_iterator_get_block(it);
                if (!block) continue;
                if (block->type == FLAC__METADATA_TYPE_STREAMINFO) {
                    const auto& si = block->data.stream_info;
                    t.sampleRate = (int)si.sample_rate;
                    t.channels   = (int)si.channels;
                    t.bitDepth   = (int)si.bits_per_sample;
                    if (si.sample_rate > 0 && si.total_samples > 0)
                        t.durationMs = (int)(si.total_samples * 1000 / si.sample_rate);
                } else {
                    const auto& vc = block->data.vorbis_comment;
                    for (unsigned i = 0; i < vc.num_comments; i++)
                        applyVorbisComment(&ctx, (const char*)vc.comments[i].entry,
                                           vc.comments[i].length);
                }
                FLAC__metadata_object_delete(block);
            } while (FLAC__metadata_simple_iterator_next(it));

            if (!ctx.title.empty())       t.title       = ctx.title;
            if (!ctx.artist.empty())      t.artist      = ctx.artist;
            if (!ctx.albumArtist.empty()) t.albumArtist = ctx.albumArtist;
            if (!ctx.album.empty())       t.album       = ctx.album;
            t.trackNumber = ctx.trackNumber;
            t.discNumber  = ctx.discNumber;
            t.genre       = ctx.genre;
            t.year        = ctx.year;
        }
        FLAC__metadata_simple_iterator_delete(it);
    }
    return t;
}

// ── Directory walk ───────────────────────────────────────────────────────────

// The shared shape of all three scans below: walk `root`, skip hidden
// directories, and hand every .flac/.wav file to `fn` as (path, lowercased
// extension). This loop was copy-pasted verbatim three times; the hidden-
// directory rule in particular is the kind that goes wrong by drifting in one
// copy only.
//
// It also stops asking the same question twice — the original built the
// filename string twice per directory entry just to test its first character.
template <typename Fn>
static void forEachAudioFile(const fs::path& root, Fn&& fn) {
    auto dit = fs::recursive_directory_iterator(
        root, fs::directory_options::skip_permission_denied);
    for (auto end = fs::recursive_directory_iterator(); dit != end; ++dit) {
        const auto& entry = *dit;
        if (entry.is_directory()) {
            const std::string name = entry.path().filename().u8string();
            if (!name.empty() && name[0] == '.')
                dit.disable_recursion_pending();  // e.g. a sibling .streamer/
            continue;
        }
        if (!entry.is_regular_file()) continue;
        std::string ext = entry.path().extension().u8string();
        lowerAscii(ext);
        if (ext != ".flac" && ext != ".wav") continue;
        fn(entry.path(), ext);
    }
}

// ── Full scan (original interface, kept for compatibility) ───────────────────

std::vector<Album> scanLibrary(const std::string& rootPath) {
    const fs::path root = fs::u8path(rootPath);
    if (!fs::exists(root)) return {};

    std::map<std::string, std::vector<Track>> byFolder;
    forEachAudioFile(root, [&](const fs::path& path, const std::string& ext) {
        const std::string folder   = path.parent_path().u8string();
        const std::string filePath = path.u8string();
        byFolder[folder].push_back(ext == ".flac" ? quickParseFLAC(filePath)
                                                  : quickParseWAV(filePath));
    });

    return buildAlbums(byFolder, rootPath);
}

// ── Incremental scan ─────────────────────────────────────────────────────────

IncrementalScanResult scanLibraryIncremental(
    const std::string& rootPath,
    const std::map<std::string, FileCache>& existing)
{
    IncrementalScanResult result;
    const fs::path root = fs::u8path(rootPath);
    if (!fs::exists(root)) return result;

    std::map<std::string, std::vector<Track>> byFolder;

    forEachAudioFile(root, [&](const fs::path& path, const std::string& ext) {
        const std::string filePath = path.u8string();

        // Check if file is unchanged
        const auto it = existing.find(filePath);
        if (it != existing.end()) {
            int64_t sz = 0, mt = 0;
            statSizeAndMtime(filePath, sz, mt);
            if (sz == it->second.fileSize && mt == it->second.fileMtime) {
                result.filesSkipped++;
                return;
            }
        }

        result.filesScanned++;
        const std::string folder = path.parent_path().u8string();
        byFolder[folder].push_back(ext == ".flac" ? quickParseFLAC(filePath)
                                                  : quickParseWAV(filePath));
    });

    result.albums = buildAlbums(byFolder, rootPath);
    return result;
}

// ── Parallel scan ────────────────────────────────────────────────────────────

std::vector<Album> scanLibraryParallel(const std::string& rootPath) {
    const fs::path root = fs::u8path(rootPath);
    if (!fs::exists(root)) return {};

    // Collect all audio file paths first. The extension is kept as a bool
    // rather than a second std::string per file — it only ever answers "FLAC
    // or WAV", and the walk has already narrowed it to those two.
    struct Entry { std::string path; bool flac; };
    std::vector<Entry> files;
    forEachAudioFile(root, [&](const fs::path& path, const std::string& ext) {
        files.push_back({path.u8string(), ext == ".flac"});
    });

    // Parse in parallel using hardware concurrency
    unsigned numThreads = std::thread::hardware_concurrency();
    if (numThreads == 0) numThreads = 4;

    std::vector<std::future<std::vector<Track>>> futures;
    const size_t chunkSize = (files.size() + numThreads - 1) / numThreads;

    for (unsigned i = 0; i < numThreads; i++) {
        const size_t start = i * chunkSize;
        const size_t end   = std::min(start + chunkSize, files.size());
        if (start >= files.size()) break;

        futures.push_back(std::async(std::launch::async, [&files, start, end]() {
            std::vector<Track> tracks;
            tracks.reserve(end - start);
            for (size_t j = start; j < end; j++)
                tracks.push_back(files[j].flac ? quickParseFLAC(files[j].path)
                                               : quickParseWAV(files[j].path));
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

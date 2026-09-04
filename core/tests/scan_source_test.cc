// The two candidate sources for a library scan must agree.
//
// scanLibraryIncremental() gets its file list either from the platform's media
// catalogue (Android's MediaStore) or by walking the tree (everywhere else).
// Those are very different mechanisms, and the whole design rests on them being
// interchangeable: the walk is the floor the index falls back to whenever it
// has nothing useful to say. If they ever disagreed, a phone and a desktop
// pointed at the same library would show different music, and nothing in the
// app would notice.
//
// The verification this replaces was "run it twice on a device and compare".
// That needs a phone, and it cannot run here. arc::fs::MediaIndex takes its
// backend as an INSTALLED pointer rather than a compile-time #ifdef precisely
// so a fake one can be installed on a desktop and the Android path exercised
// for real.
#undef NDEBUG
#include <cassert>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <unistd.h>

#include "core/library.h"
#include "arc/fs/media_index.hh"
#include "arc/fs/walk.hh"

namespace stdfs = std::filesystem;

namespace {

// ── A fixture of real, parseable WAV files ──────────────────────────────────
// They must be real: the scan opens them to read sample rate, channels and bit
// depth, which is exactly the part no media index can supply.
void writeWav(const stdfs::path& p, uint32_t rate, uint16_t channels,
              uint16_t bits, uint32_t frames) {
    stdfs::create_directories(p.parent_path());
    const uint32_t byteRate   = rate * channels * (bits / 8);
    const uint16_t blockAlign = static_cast<uint16_t>(channels * (bits / 8));
    const uint32_t dataBytes  = frames * blockAlign;

    std::ofstream f(p, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };

    f.write("RIFF", 4); u32(36 + dataBytes); f.write("WAVE", 4);
    f.write("fmt ", 4); u32(16); u16(1); u16(channels);
    u32(rate); u32(byteRate); u16(blockAlign); u16(bits);
    f.write("data", 4); u32(dataBytes);

    const std::vector<char> silence(dataBytes, 0);
    f.write(silence.data(), static_cast<std::streamsize>(silence.size()));
}

struct Fixture {
    stdfs::path root;
    std::vector<std::string> files;

    Fixture() {
        root = stdfs::temp_directory_path() /
               ("matrix_scan_source_test_" + std::to_string(::getpid()));
        stdfs::remove_all(root);
        stdfs::create_directories(root);

        add("Artist A/Record One (16-44.1)/01.wav", 44100, 2, 16);
        add("Artist A/Record One (16-44.1)/02.wav", 44100, 2, 16);
        add("Artist A/Record Two (24-96)/01.wav", 96000, 2, 24);
        add("Artist B/Another Record (16-44.1)/01.wav", 44100, 2, 16);
        add("Artist B/Another Record (16-44.1)/02.wav", 44100, 2, 16);
        add("Artist B/Another Record (16-44.1)/03.wav", 44100, 2, 16);
    }
    ~Fixture() {
        std::error_code ec;
        stdfs::remove_all(root, ec);
    }

    void add(const std::string& rel, uint32_t rate, uint16_t ch, uint16_t bits) {
        const stdfs::path p = root / rel;
        writeWav(p, rate, ch, bits, 128);
        files.push_back(p.u8string());
    }
};

// ── A stand-in for MediaStore ───────────────────────────────────────────────
// It reports the same files the walk would find, with the metadata a real index
// carries and WITHOUT the three fields none of them carry.
struct FakeIndex : arc::fs::MediaIndexBackend {
    std::vector<arc::fs::MediaRecord> rows;
    bool fail = false;
    bool empty = false;

    bool query(const std::string& root, std::vector<arc::fs::MediaRecord>& out,
               arc::Error& err) override {
        if (fail) {
            err.kind = arc::ErrorKind::Io;
            err.message = "no storage access";
            return false;
        }
        if (empty) return true;
        for (const auto& r : rows)
            if (r.path.rfind(root, 0) == 0) out.push_back(r);
        return true;
    }
    uint64_t generation() override { return 1; }
    bool observe(std::function<void()>, arc::Error&) override { return true; }
};

FakeIndex makeIndexFor(const Fixture& fx) {
    FakeIndex fake;
    for (const std::string& path : fx.files) {
        arc::fs::MediaRecord r;
        r.path = path;
        int64_t size = 0, mtime = 0;
        arc::fs::statFile(path, size, mtime);
        r.size = size;
        // A real index reports unix seconds, which is a DIFFERENT domain from
        // the walk's tick count. Using the same file's real size and a plausible
        // time is enough to exercise the comparison; the point being tested is
        // that each source is self-consistent, not that they share a clock.
        r.mtimeUnix = 1700000000;
        r.title = stdfs::u8path(path).stem().u8string();
        r.album = stdfs::u8path(path).parent_path().filename().u8string();
        r.albumArtist =
            stdfs::u8path(path).parent_path().parent_path().filename().u8string();
        r.durationMs = 1000;
        // sampleRate/channels/bitDepth deliberately left 0 — see MediaRecord.
        fake.rows.push_back(std::move(r));
    }
    return fake;
}

// Albums reduced to what a listener would actually notice, and ordered, so two
// scans can be compared for equality rather than eyeballed.
std::vector<std::string> summarize(const std::vector<Album>& albums) {
    std::vector<std::string> out;
    for (const Album& a : albums) {
        std::string line = a.name + " | " + a.artist + " |";
        std::vector<std::string> paths;
        for (const Track& t : a.tracks) {
            char buf[64];
            std::snprintf(buf, sizeof buf, " [%d/%d/%d]", t.sampleRate,
                          t.channels, t.bitDepth);
            paths.push_back(t.filePath + buf);
        }
        std::sort(paths.begin(), paths.end());
        for (const std::string& p : paths) line += " " + p;
        out.push_back(line);
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace

// THE test: both sources, same library.
static void testIndexAndWalkAgree() {
    Fixture fx;
    const std::map<std::string, Track> noCache;

    arc::fs::setMediaIndexBackend(nullptr);
    assert(!arc::fs::MediaIndex::available());
    const auto walked = summarize(
        scanLibraryIncremental(fx.root.u8string(), noCache).albums);
    assert(walked.size() == 3);   // three album folders in the fixture

    FakeIndex fake = makeIndexFor(fx);
    arc::fs::setMediaIndexBackend(&fake);
    assert(arc::fs::MediaIndex::available());
    const auto indexed = summarize(
        scanLibraryIncremental(fx.root.u8string(), noCache).albums);

    arc::fs::setMediaIndexBackend(nullptr);

    // Same albums, same artists, same tracks — AND the same sample rate,
    // channel count and bit depth, which the index never reported. Those can
    // only be right because the scan still opened the new files, which is the
    // half of the two-phase design that is easy to lose.
    assert(indexed == walked);
    for (const std::string& line : indexed) {
        assert(line.find("[44100/2/16]") != std::string::npos ||
               line.find("[96000/2/24]") != std::string::npos);
        assert(line.find("[0/0/0]") == std::string::npos);
    }
}

// Every way the index can decline must land on the walk, with the full library
// intact. A silent fallthrough to "no music" is the failure this guards.
static void testFallbackToWalk() {
    Fixture fx;
    const std::map<std::string, Track> noCache;

    arc::fs::setMediaIndexBackend(nullptr);
    const auto expected = summarize(
        scanLibraryIncremental(fx.root.u8string(), noCache).albums);

    {   // the query was refused — no storage grant yet
        FakeIndex fake = makeIndexFor(fx);
        fake.fail = true;
        arc::fs::setMediaIndexBackend(&fake);
        assert(summarize(scanLibraryIncremental(fx.root.u8string(), noCache).albums) ==
               expected);
    }
    {   // the index has nothing here — a .nomedia folder, or files just copied
        FakeIndex fake = makeIndexFor(fx);
        fake.empty = true;
        arc::fs::setMediaIndexBackend(&fake);
        assert(summarize(scanLibraryIncremental(fx.root.u8string(), noCache).albums) ==
               expected);
    }
    arc::fs::setMediaIndexBackend(nullptr);
}

// The cache must hit on the index path too, or a phone re-parses its whole
// library on every launch while reporting that it skipped it — which is exactly
// the bug this work started from.
static void testIndexPathHonoursTheCache() {
    Fixture fx;
    FakeIndex fake = makeIndexFor(fx);
    arc::fs::setMediaIndexBackend(&fake);

    const std::map<std::string, Track> noCache;
    auto first = scanLibraryIncremental(fx.root.u8string(), noCache);
    assert(first.filesScanned == 6);
    assert(first.filesSkipped == 0);

    // Feed the first scan's own tracks back as the cache, the way the app does.
    std::map<std::string, Track> cache;
    for (const Album& a : first.albums)
        for (const Track& t : a.tracks) cache[t.filePath] = t;
    assert(cache.size() == 6);

    auto second = scanLibraryIncremental(fx.root.u8string(), cache);
    assert(second.filesScanned == 0);
    assert(second.filesSkipped == 6);
    // ...and the library is still whole, not just the unchanged part of it.
    assert(summarize(second.albums) == summarize(first.albums));

    arc::fs::setMediaIndexBackend(nullptr);
}

// A file the index lists but the parsers cannot read is not music. Left in, it
// would become an album with one silent track.
static void testNonAudioRowsAreIgnored() {
    Fixture fx;
    FakeIndex fake = makeIndexFor(fx);
    arc::fs::MediaRecord junk;
    junk.path = (fx.root / "Artist A" / "Record One (16-44.1)" / "cover.jpg").u8string();
    junk.size = 10;
    junk.mtimeUnix = 1700000000;
    fake.rows.push_back(junk);
    arc::fs::setMediaIndexBackend(&fake);

    const std::map<std::string, Track> noCache;
    auto r = scanLibraryIncremental(fx.root.u8string(), noCache);
    assert(r.filesScanned == 6);   // the .jpg was not counted or opened

    arc::fs::setMediaIndexBackend(nullptr);
}

int main() {
    testIndexAndWalkAgree();
    testFallbackToWalk();
    testIndexPathHonoursTheCache();
    testNonAudioRowsAreIgnored();
    std::printf("scan_source_test: all assertions passed\n");
    return 0;
}

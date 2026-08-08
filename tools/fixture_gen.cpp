// tools/fixture_gen.cpp
//
// Debug-only dev tool: writes a small, fixed synthetic music library to disk
// (real sine-tone WAV audio, real folder layout) so the GUI's Album/EP/
// Single/Compilation/Live/Remix views and the album-variant grouping can be
// exercised without a real music collection. See
// docs/superpowers/specs/2026-08-08-fixture-gen-design.md.
//
// Self-verifies every generated album against the real classifyReleaseType()
// (core/src/variants.cpp, linked directly here - the same "link it alone"
// trick core/tests/variants_test.cc already uses) so this fixture can never
// silently drift out of sync with the classifier it exists to exercise.

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#include "core/library.h"
#include "core/variants.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr int kSampleRate = 44100;
constexpr int kChannels = 2;
constexpr int kBitsPerSample = 16;
constexpr double kTrackSeconds = 2.0;
constexpr double kBaseFreqHz = 220.0;
constexpr double kAmplitude = 0.3 * 32767.0;
constexpr int kFadeSamples = 220; // ~5 ms at 44.1 kHz
constexpr double kPi = 3.14159265358979323846; // <cmath>'s M_PI needs
                                                // _USE_MATH_DEFINES on MSVC;
                                                // not worth depending on it

struct FixtureTrack {
    std::string title;
};

struct FixtureAlbum {
    std::string folderName;
    std::vector<FixtureTrack> tracks;
    Album::ReleaseType expected;
};

// Writes one sine-tone WAV. trackIndex selects an ascending chromatic pitch
// so tracks in the same album are audibly distinct when played back through
// the real app (see spec's Testing section) - not decoration.
void writeToneWav(const fs::path& path, int trackIndex) {
    const double freq = kBaseFreqHz * std::pow(2.0, trackIndex / 12.0);
    const drwav_uint64 totalFrames =
        static_cast<drwav_uint64>(kTrackSeconds * kSampleRate);

    drwav_data_format format{};
    format.container = drwav_container_riff;
    format.format = DR_WAVE_FORMAT_PCM;
    format.channels = kChannels;
    format.sampleRate = kSampleRate;
    format.bitsPerSample = kBitsPerSample;

    drwav wav;
    std::string pathStr = path.string();
    if (!drwav_init_file_write_sequential_pcm_frames(
            &wav, pathStr.c_str(), &format, totalFrames, nullptr)) {
        std::fprintf(stderr, "fixture_gen: failed to open '%s' for writing\n",
                     pathStr.c_str());
        std::exit(1);
    }

    std::vector<int16_t> frame(kChannels);
    for (drwav_uint64 i = 0; i < totalFrames; i++) {
        double t = static_cast<double>(i) / kSampleRate;
        double envelope = 1.0;
        if (i < static_cast<drwav_uint64>(kFadeSamples))
            envelope = static_cast<double>(i) / kFadeSamples;
        else if (i >= totalFrames - static_cast<drwav_uint64>(kFadeSamples))
            envelope = static_cast<double>(totalFrames - i) / kFadeSamples;
        int16_t sample = static_cast<int16_t>(
            kAmplitude * envelope * std::sin(2.0 * kPi * freq * t));
        for (int c = 0; c < kChannels; c++) frame[c] = sample;
        if (drwav_write_pcm_frames(&wav, 1, frame.data()) != 1) {
            std::fprintf(stderr, "fixture_gen: short write to '%s'\n",
                         pathStr.c_str());
            drwav_uninit(&wav);
            std::exit(1);
        }
    }
    drwav_uninit(&wav);
}

// Writes one fixture album's folder + tracks, then asks the REAL classifier
// what it thinks this album is and aborts loudly if it disagrees with what
// this fixture was built to demonstrate.
void writeAlbum(const fs::path& artistDir, const FixtureAlbum& album) {
    fs::path albumDir = artistDir / album.folderName;
    fs::create_directories(albumDir);

    std::vector<Track> tracks;
    tracks.reserve(album.tracks.size());
    for (size_t i = 0; i < album.tracks.size(); i++) {
        char stem[16];
        std::snprintf(stem, sizeof(stem), "%02zu ", i + 1);
        std::string title = std::string(stem) + album.tracks[i].title;
        writeToneWav(albumDir / (title + ".wav"), static_cast<int>(i));

        Track t;
        t.title = title; // matches quickParseWAV's filename-stem rule
        tracks.push_back(std::move(t));
    }

    Album::ReleaseType actual = classifyReleaseType(album.folderName, tracks);
    if (actual != album.expected) {
        std::fprintf(stderr,
            "fixture_gen: '%s' classified as %d, expected %d - "
            "classifyReleaseType() no longer agrees with this fixture. "
            "Update docs/superpowers/specs/2026-08-08-fixture-gen-design.md "
            "and tools/fixture_gen.cpp together.\n",
            album.folderName.c_str(), static_cast<int>(actual),
            static_cast<int>(album.expected));
        std::exit(1);
    }
}

} // namespace

int main(int argc, char** argv) {
    fs::path root = (argc > 1) ? fs::path(argv[1]) : fs::path("fixture_library");

    std::error_code ec;
    fs::remove_all(root, ec); // disposable, regenerated fresh every run
    fs::path artistDir = root / "Fixture Artist";
    fs::create_directories(artistDir);

    using RT = Album::ReleaseType;

    const std::vector<FixtureAlbum> albums = {
        {"Full Length Album",
         {{"Opening"}, {"Second Wind"}, {"Middle Ground"}, {"Turning Point"}, {"Finale"}},
         RT::Album},
        {"Short Stories EP",
         {{"Intro"}, {"Core Idea"}, {"Outro"}},
         RT::Ep},
        {"One More Time",
         {{"One More Time"}},
         RT::Single},
        {"Live at the Fixture Hall",
         {{"Opening"}, {"Fan Favorite"}, {"Closing"}},
         RT::Live},
        {"Remix Collection",
         {{"Track One"}, {"Track Two"}, {"Track Three"}},
         RT::Remix},
        {"Greatest Hits",
         {{"Hit One"}, {"Hit Two"}, {"Hit Three"}, {"Hit Four"}, {"Hit Five"}, {"Hit Six"}},
         RT::Compilation},
        {"Second Album",
         {{"Opening"}, {"Second Wind"}, {"Middle Ground"}, {"Turning Point"}, {"Finale"}},
         RT::Album},
        {"Second Album (Deluxe Edition)",
         {{"Opening"}, {"Second Wind"}, {"Middle Ground"}, {"Turning Point"}, {"Finale"},
          {"Bonus Track One"}, {"Bonus Track Two"}},
         RT::Album},
        {"Second Album (feat. Someone Else)",
         {{"Opening"}, {"Second Wind"}, {"Middle Ground"}, {"Turning Point"}, {"Finale"}},
         RT::Album},
    };

    for (const auto& album : albums) writeAlbum(artistDir, album);

    std::printf("fixture_gen: wrote %zu albums to '%s'\n", albums.size(),
                root.string().c_str());
    return 0;
}

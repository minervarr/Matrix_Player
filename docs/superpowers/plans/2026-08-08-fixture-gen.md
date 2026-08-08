# matrix_fixture_gen Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build `matrix_fixture_gen`, a Debug-only standalone tool that writes
a small, fixed synthetic music library (real sine-tone WAVs, real folder
layout) to disk, so `matrix_player`'s Album/EP/Single/Compilation/Live/Remix
views and album-variant grouping can be exercised without a real collection.

**Architecture:** One new translation unit, `tools/fixture_gen.cpp`, wired
into `core/CMakeLists.txt`'s existing Debug-only block (next to
`variants_test`/`stats_test`), linking `core/src/variants.cpp` directly (not
`matrix_core`) for `classifyReleaseType()` self-verification, and
`third_party/dr_wav.h` (own `DR_WAV_IMPLEMENTATION`, no ODR conflict since
this target never links `decoder.cpp`).

**Tech Stack:** C++17, `<filesystem>`, `dr_wav.h` (vendored, writer API),
`core/include/core/{library,variants}.h`.

## Global Constraints

- Spec: `docs/superpowers/specs/2026-08-08-fixture-gen-design.md` — every
  requirement below is copied verbatim from it; the spec is authoritative if
  anything here is ambiguous.
- WAV only, 44.1 kHz / 16-bit / stereo, ~2 s/track, sine tone per track at
  `220 * 2^(n/12)` Hz (`n` = track index within its album), 30% full-scale
  amplitude (`0.3 * 32767`), ~220-sample (~5 ms) linear fade-in/out.
- Filenames: zero-padded `NN Title.wav` (e.g. `01 Opening.wav`) — this is
  also the track's title as the real scanner would read it (WAV has no tag
  support in this codebase; title = filename stem).
- Output root: `./fixture_library/` (cwd-relative), overridable via one
  optional positional CLI arg. Existing target directory is deleted and
  recreated every run.
- Self-verification: after writing each album, call the real
  `classifyReleaseType(albumName, tracks)` and exit non-zero with a clear
  message on any mismatch against the expected type table in the spec.
- No CLI flags beyond the optional output-path argument. No FLAC. No
  self-verification of variant *grouping* (only `classifyReleaseType`).
- Debug-only (`CMAKE_BUILD_TYPE STREQUAL "Debug"`), both platforms, no
  `WIN32`/`NOT WIN32` restriction, no opt-in `option()` flag.

---

### Task 1: `matrix_fixture_gen` tool + CMake wiring

**Files:**
- Create: `tools/fixture_gen.cpp`
- Modify: `core/CMakeLists.txt:96` (end of the existing
  `if(CMAKE_BUILD_TYPE STREQUAL "Debug")` block, after `stats_test`)

**Interfaces:**
- Consumes: `Track`, `Album::ReleaseType` (`core/include/core/library.h`);
  `classifyReleaseType(const std::string& albumName, const std::vector<Track>& tracks)`
  (`core/include/core/variants.h`); `dr_wav.h`'s writer API
  (`drwav_data_format`, `drwav_init_file_write_sequential_pcm_frames`,
  `drwav_write_pcm_frames`, `drwav_uninit`).
- Produces: the `matrix_fixture_gen` executable and, at runtime, a
  `fixture_library/` directory tree consumed manually via
  `matrix_player`'s own "Add Music Folder" (no other task depends on this
  programmatically).

- [ ] **Step 1: Write `tools/fixture_gen.cpp`**

```cpp
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
```

- [ ] **Step 2: Wire it into `core/CMakeLists.txt`**

Insert after the closing brace of `stats_test` (currently the last thing
before the final `endif()` at line 96), still inside the
`if(CMAKE_BUILD_TYPE STREQUAL "Debug")` block:

```cmake
    # ── matrix_fixture_gen ────────────────────────────────────────────────
    # Writes a small, fixed synthetic music library (real sine-tone WAVs, a
    # real folder layout) to disk so the GUI's Album/EP/Single/Compilation/
    # Live/Remix views and the variant-grouping rules can be exercised
    # without a real music collection — see
    # docs/superpowers/specs/2026-08-08-fixture-gen-design.md. Links
    # variants.cpp alone (same trick as variants_test above) to self-verify
    # every album it writes against the REAL classifyReleaseType(), so it
    # can never silently drift out of sync with the classifier it exists to
    # exercise. Both platforms — no OS-specific code at all, unlike
    # icon_preview/matrix_ui_capture's Wayland dependency.
    add_executable(matrix_fixture_gen ${CMAKE_SOURCE_DIR}/tools/fixture_gen.cpp src/variants.cpp)
    target_include_directories(matrix_fixture_gen PRIVATE include ${CMAKE_SOURCE_DIR}/third_party)
    if(MSVC)
        target_compile_options(matrix_fixture_gen PRIVATE /W3 /utf-8)
    else()
        target_compile_options(matrix_fixture_gen PRIVATE -Wall)
    endif()
```

- [ ] **Step 3: Reconfigure and build**

From a Windows shell with the MSYS2 UCRT64 toolchain on PATH (see this
session's earlier port work — `scripts\windows\build.ps1 -Debug`, or
directly: `cmake --build build_debug --target matrix_fixture_gen`).

Expected: builds cleanly, no warnings beyond the project's existing baseline
(`-Wall`, same as `variants_test`/`stats_test`).

- [ ] **Step 4: Run it and check the happy path**

```
build_debug\core\matrix_fixture_gen.exe
```

Expected: exits 0, prints `fixture_gen: wrote 9 albums to 'fixture_library'`,
and `fixture_library\Fixture Artist\` contains 9 subfolders matching the
table in the spec, each with the right track count and `NN Title.wav` files
of a few hundred KB each (2 s of 16-bit/44.1 kHz stereo ≈ 176 KB).

- [ ] **Step 5: Prove the self-verification actually catches a mismatch**

Temporarily edit one album's `expected` field in `tools/fixture_gen.cpp` to
a wrong value (e.g. change `"Live at the Fixture Hall"`'s `RT::Live` to
`RT::Album`), rebuild, and run again.

Expected: exits non-zero, stderr prints
`fixture_gen: 'Live at the Fixture Hall' classified as 5, expected 0 - ...`
(exact numbers depend on the `ReleaseType` enum values — `Live = 5`,
`Album = 0` per `core/include/core/library.h`).

Revert the temporary edit, rebuild, and re-run Step 4 to confirm it's back
to exit 0 / 9 albums.

- [ ] **Step 6: Manually verify against the real app**

Launch `build_debug\gui\matrix_player.exe`, Settings → Add Music Folder →
select the `fixture_library` directory produced above (or its `Fixture
Artist` subfolder — either works, the scanner recurses). Confirm:
- Albums tab shows `Full Length Album` and (as one merged tile — the one
  with more tracks, `Second Album (Deluxe Edition)`'s cover) the
  `Second Album` / `Second Album (Deluxe Edition)` pair, **plus** a
  *separate* tile for `Second Album (feat. Someone Else)`.
- EPs tab shows `Short Stories EP`.
- Singles tab shows `One More Time`.
- Live tab shows `Live at the Fixture Hall`.
- Remixes tab shows `Remix Collection`.
- Compilations tab shows `Greatest Hits`.
- Double-click a track in any album and confirm audible playback (a clean
  tone, no clicks at the start/end).

This step has no pass/fail automation — it's a human look, same as the
spec's Testing section describes. Note the result in the task's completion
message; if anything doesn't match, stop and report before continuing (a
mismatch here means either the fixture's design or the real scanner/grouping
code has a bug worth understanding, not something to paper over).

- [ ] **Step 7: Report, do not commit**

Summarize what was built and the Step 6 observations. Do **not** run
`git_wrapper`/`git add`/`git commit` — this session's user has explicitly
asked for no commits or pushes; leave `tools/fixture_gen.cpp` and the
`core/CMakeLists.txt` change as uncommitted working-tree changes, same as
the rest of this session's work.

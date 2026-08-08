# `matrix_fixture_gen` — synthetic test library generator

## Context

Manual verification of the Windows/Clang port's WASAPI output (see the port's own
notes) worked by copying Windows' built-in system-sound WAVs into a fake
`Artist/Album` folder and pointing the real scanner at it. That worked but is
Windows-only and not a real, repeatable fixture. This tool replaces that
one-off with a permanent, cross-platform Debug dev tool: it writes a small,
fixed synthetic music library to disk — real (if trivial) WAV audio, laid out
so the actual scanner (`core/src/library.cpp`) picks it up exactly like a real
collection — covering every `classifyReleaseType()` branch and the album
variant-grouping/outranking rules, so those UI paths (and manual audio
playback) can be exercised without a real music collection.

## Scope

**In scope**: generating a fixed, deterministic set of WAV-only fixture
albums on disk, self-verifying each one's expected `classifyReleaseType()`
result against the real function.

**Out of scope** (and why):
- **FLAC fixtures.** WAV has zero tag support in this scanner (confirmed in
  `library.cpp`'s `quickParseWAV` — track title is the filename stem;
  artist/album/discNumber/genre/year are never populated). FLAC would add
  real tags (needed for multi-disc/genre/year fixtures) but requires linking
  and driving libFLAC's *encoder*, real added complexity for a first version.
  Every `classifyReleaseType()` branch (Album/EP/Single/Live/Remix/
  Compilation) and the variant-grouping/outranking rules are reachable from
  album-name and track-count signals alone, which WAV-only fixtures cover
  completely. Multi-disc-specific behavior (`everyDiscIsMajority`'s
  per-disc/bonus-disc rule) is the one thing this can't exercise, since it
  needs `DISCNUMBER` tags — left for a future FLAC-capable version if ever
  needed.
- **Self-verifying variant *grouping*** (`groupAlbumVariants`/`variantKey`/
  `variantOutranks`). Self-verification is scoped to `classifyReleaseType()`
  only — cheap, well-isolated, exactly what `variants_test` already links in
  isolation. Reproducing `variantKey()`'s Album-struct field population would
  duplicate scanner logic for a second, separate check. The variant-pair
  fixture is still generated (folders 7a/7b/8 below) — it's verified by
  looking at the real app's grid (Settings → Add Music Folder → the real
  scan/UI), which is the actual thing being tested.
- **Configurability.** No CLI flags to pick a subset or vary counts. One
  fixed scenario, regenerated fully every run. Simpler to maintain, and the
  whole point is a known-good, always-complete set.

## Architecture

One standalone translation unit, `tools/fixture_gen.cpp`, building to
`matrix_fixture_gen`. No `matrix_core` link, no Vulkan/sqlite/audio_engine —
mirrors `matrix_ab_test`'s standalone-tool shape (root `CMakeLists.txt`, not
`gui/CMakeLists.txt`), not the GUI-linked shape of `icon_preview`/
`matrix_ui_capture`.

Dependencies:
- `third_party/dr_wav.h`, this TU's own `DR_WAV_IMPLEMENTATION` — safe
  because this tool never links `core/src/decoder.cpp` (the only other TU
  defining that macro), so there's no ODR collision.
- `core/src/variants.cpp` compiled directly as an extra source (not through
  `matrix_core`) for `classifyReleaseType()` — the same pattern
  `core/CMakeLists.txt`'s `variants_test` already uses; `variants.cpp` is
  documented as deliberately kept dependency-free for exactly this kind of
  isolated reuse.
- `<filesystem>`, `<cmath>`, `<cstdint>` — nothing else.

CMake: new block in root `CMakeLists.txt`, gated
`if(CMAKE_BUILD_TYPE STREQUAL "Debug")`, **both platforms** (no `WIN32`/
`NOT WIN32` restriction — the tool touches no OS-specific API, unlike
`icon_preview`/`matrix_ui_capture`'s Wayland dependency or `matrix_ab_test`'s
Windows-only USB headers). No opt-in `option()` flag — light enough to just
always build in Debug, like `icon_preview` does.

## The fixture scenario

Output root: `./fixture_library/` (relative to current working directory),
overridable with one optional positional CLI argument. If the target
directory already exists, it is deleted and recreated — the content is
disposable and regenerated fresh every run, so silently going stale is worse
than silently overwriting.

All audio: 44.1 kHz, 16-bit, stereo, ~2 s/track, a sine tone per track
(ascending chromatic steps from a 220 Hz base — `220 * 2^(n/12)`, `n` = track
index within its album) at 30% full-scale amplitude (`0.3 * INT16_MAX`) with
a short linear fade-in/out (~5 ms, ~220 samples at 44.1 kHz) to avoid
boundary clicks. Filenames are zero-padded `NN Title.wav`
(e.g. `01 Opening.wav`) — this matters because WAV tracks are never tagged
with a track number either (only title-from-filename), so lexical filename
order has to already be the intended play order.

One artist, `Fixture Artist`, eight album folders under it:

| # | Folder name | Tracks | Exercises |
|---|---|---|---|
| 1 | `Full Length Album` | 5 | Album (track-count fallback, ≥5) |
| 2 | `Short Stories EP` | 3 | EP (track-count fallback, 2–4) |
| 3 | `One More Time` | 1 | Single (track-count fallback, ==1) |
| 4 | `Live at the Fixture Hall` | 3 | Live (album-name term match) |
| 5 | `Remix Collection` | 3 | Remix (album-name term match) |
| 6 | `Greatest Hits` | 6 | Compilation (album-name term match) |
| 7a | `Second Album` | 5 | Variant pair, base tile |
| 7b | `Second Album (Deluxe Edition)` | 7 | Variant pair, same group as 7a — "Deluxe Edition" is a recognized Edition modifier, so it merges with 7a into one grid tile; the extra 2 tracks make 7b outrank 7a as the group's primary (`variantOutranks`'s track-count tie-break) |
| 8 | `Second Album (feat. Someone Else)` | 5 | Variant **control** — same artist/base name as 7a/7b, but `"feat."` isn't an Edition/Remix term, so `classifyModifier()` leaves it as an "Other" differentiator and it must stay a **separate** tile, not merge into 7's group |

Track titles inside each album are simple placeholders (`Opening`, `Second
Wind`, ...) except where the album-name signal alone isn't the point being
tested — none of the current scenarios need track-title-level remix/live
markers, since album-name matching already covers both, so every album here
relies on the folder name (the simpler, lower-precedence-independent trigger).

Precedence sanity check (the real `classifyReleaseType()` order is Remix →
Live → Compilation → track-count fallback): none of the chosen folder names
cross-trigger a higher-precedence category by accident (`Live at the Fixture
Hall` contains no remix term, `Remix Collection` contains no live term,
`Greatest Hits` contains neither) — verified by reasoning against the actual
term tables in `variants.cpp`, and re-verified for real at generation time by
the self-check below.

## Self-verification

After writing each album's files, the tool calls the real
`classifyReleaseType(albumName, tracks)` with the same folder name and a
`Track` list matching what was just written, and compares the result against
the expected type from the table above. Any mismatch prints the album,
expected vs. actual type, and the tool exits non-zero without leaving a
half-correct `fixture_library/` around silently passing as fine — a
mismatch means either this spec's understanding of the classifier is stale
or a real regression, and either way a human needs to look.

## Error handling

- Target directory exists → removed and recreated (see above; not an error).
- Filesystem write failure (disk full, permissions, path-too-long) → print
  the failing path and `std::strerror`-equivalent, exit non-zero. No partial
  retry logic — this is a dev tool, not something that needs to be robust
  against transient failure.
- `classifyReleaseType()` mismatch → see Self-verification above.

## Testing

No automated test *of* the tool itself (it's a test-fixture generator, not
production code the way `variants_test`/`stats_test` guard `core/`). Its own
correctness is exercised by:
1. The self-verification step above, every run.
2. Manually pointing the real `matrix_player` at the generated
   `fixture_library/` (Settings → Add Music Folder) and confirming: all six
   release-type sections show the right tile, the 7a/7b pair renders as one
   tile (with 7b's cover — more tracks wins), and 8 renders as a separate
   tile from that group.

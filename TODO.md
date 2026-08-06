# TODO — Matrix Player

Windows and Linux are equal peers; nothing here is one-platform unless it says
so. Items are grouped by what they touch, roughly in the order they'd be picked
up. See `CLAUDE.md` for the architecture this all sits in.

---

## Album variants and editions

Built. `core/include/core/variants.h` + `core/src/variants.cpp` group albums
that are the same release; the grid draws one tile per group and the album view
lists the rest under `OTHER VERSIONS` (`MORE REMIXES` on a remix page, where
"version" would say nothing), below the artist bio. Pinned by
`core/tests/variants_test.cc`.

- [x] **Group albums that are the same release.** Same release type, same
      artist, same base name, differing only in **quality** (the folder
      convention's `(24-96)` suffix, already stripped into
      `Album::displayName`), in **edition** (Deluxe, Edición Especial) or in
      **remix set**. All three real cases in the test library now collapse:
      *The End Of Genesys* + *(Deluxe)*, *Senderos De Traición* +
      *- Edición Especial*, and *Obsessed (Remixes)* +
      *Obsessed (horsegiirL & Luvhunter VIP Remix)*. Release type is part of
      the key, so a group always belongs to exactly one of the grid's tabs —
      *Obsessed* the single stays in Singles, apart from its own remixes.
- [x] **Multi-disc releases are no longer mis-filed as remix sets.**
      `classifyReleaseType()` counted remix-looking titles across the whole
      flat track list, so an album with a bonus remix disc could tip the
      majority and land in the Remixes tab — Anyma's *Genesys II* (10
      originals on disc 1, 11 remixes on disc 2) did exactly that. The
      majority is now counted **per tagged disc**, and a release only counts
      as a remix set if every disc is one. The function moved to
      `core/src/variants.cpp` on the way, so it is finally under test.
- [x] **Remix groups draw a 2×2 cover mosaic** rather than one member's art:
      a remix set is different music, not another pressing. Empty quadrants are
      flat; past four members the last quadrant fades into
      `CLR_TILE_MORE_GREEN` (a green deliberately distinct from the accent,
      which stays reserved for state). See `drawVariantMosaic()` and §8.2 of
      the design system.
- [x] **The grid shows one tile per group** — `rebuildGridIndices()` iterates
      `albumGroups_` and draws each group's `primary`. Search matches on ANY
      member, so grouping never hides a result.
- [x] **List the other variants in the album view, below the artist bio.**
      A horizontal strip of thumbnails (art / name / quality line), wrapping to
      a second row; clicking one opens that variant in the same panel. The
      album being viewed is never listed in its own strip.
      - Ranking (`variantOutranks`): **DSD outranks PCM**, then sample rate,
        then bit depth, then track count. There is no way to tell native DSD
        from a PCM→DSD conversion, so DSD is taken at face value.
      - Decided tie-break, implemented: quality wins over track count — a DSD
        standard edition takes the tile and a PCM deluxe drops into the strip.
      - Choosing a lower-quality variant is never forced by the hardware —
        outside bit-perfect mode the app resamples — so this list is for
        *wanting* to compare, not for working around a device limit.
      - Caveat, unchanged: `hasDsd` is hardcoded `false`
        (`core/src/library.cpp`) — nothing decodes DSD/DSF yet, so the "DSD
        outranks PCM" branch has no live input and sample rate + track count
        are the keys that actually fire. The rule is written and tested; it
        starts mattering once the DoP work below lands.

- [ ] **MP3 and DSD are not indexed at all.** The scanner takes `.flac` and
      `.wav` only (`core/src/library.cpp`), though the decoder plays MP3 fine.
      So "the same album, also in MP3" cannot exist in the library. The variant
      strip is already written for it — `variantFormatLabel()` prints `MP3` /
      `DSD` and stays silent for FLAC/WAV — but the label cannot fire until the
      scan takes `.mp3` (header + ID3 via the already-linked libmpg123) and
      `.dsf`/`.dff`. The test library is 518 files, all FLAC, so nothing is
      hidden today; this is the gap between what the UI can say and what the
      scanner can find.

## Names, languages and metadata

- [ ] **Two translation layers.** They are different jobs and only one of them
      follows the user's language:
      - **Borderless** — normalizes *album metadata* (deluxe / edición especial
        / remastered / en vivo / …) whatever language the library is in and
        whatever language the user speaks. Album names are never translated for
        the user; they are *classified*. **Started**: `classifyModifier()`
        (`core/src/variants.cpp`) reads a multilingual term table over an
        accent-folded, case-folded form, and tells an Edition modifier from
        anything else. That is all the variant grouping needed. It is a table,
        so another language is rows, not logic.
      - **Bordered** — the UI's own strings, which do follow the user. Does not
        exist: the app is English-only.
      `splitNameModifier()` itself is still pure punctuation matching (brackets,
      and a capitalised dash tail) — it moved to `core/src/variants.cpp` so the
      grouping and the drawing split names identically.
- [ ] **Classify modifier kinds** and style them differently: deluxe/reissue vs.
      feat. credits vs. "from the series …" source tags vs. classical
      work/movement suffixes. `ModifierKind` (`core/variants.h`) makes the first
      cut — Edition vs. everything else — but only the grouping reads it; the
      drawing still styles every modifier the same. Option to collapse
      modifiers in dense views and reveal on hover. Classical music (long
      structured names) is the hard case.
- [ ] Parse remaining useful Vorbis fields: date, genre, replaygain.
      `DISCNUMBER` is done (`parsePositionTag()` in `core/src/library.cpp`
      handles both "2" and "2/3"), so these three are what's left.

## UI / UX

- [ ] **Configurable quality colors.** `CLR_QUALITY_*` (`gui/src/theme.hh`) is
      fixed and ported verbatim from the Android sibling — full-chroma primaries
      chosen to match the DAC's own indicator. Let the user retune the hues.
- [ ] **Snap the album view's ad-hoc spacings to the `SP_*` scale.** `space(4)`,
      `space(10)`, `space(16)` bypass the step scale `gui/src/theme.hh` asks
      callers to prefer. Only two are free (±1px); the rest shift 2–3px, which
      is why they were left alone. Column widths (`space(49)`, `space(75)`) have
      no sensible token and should stay as they are.
- [ ] Keyboard shortcuts: Left/Right = seek 10s, F = fullscreen art.
      (Space = play/stop and Escape already work.)

## Audio

- [ ] **DoP** (DSD-over-PCM): the C++ packer already exists
      (`framework/audio_engine`'s `DsdPackager::packDop`, feeding
      `UsbAudioSink`'s raw-passthrough path — no `writeDop()` needed on
      `UsbAudioDriver`) and `core/src/decoder.cpp` already dispatches to
      `ae::DsdDecoder` for DSD magic bytes. What's actually missing is
      upstream of both: the scanner never indexes `.dsf`/`.dff` (see "MP3 and
      DSD are not indexed at all" above), so no DSD track is ever discovered
      to play in the first place.
- [ ] **DSD mode selector**: Native / DoP / PCM fallback (mirrors `DsdMode.java`).
- [ ] USB device picker (enumerate libusb devices by VID/PID) instead of the
      hardcoded Hiby FC4 ids.
- [ ] Seekbar: there is none. Position is text-only today, deliberately, but a
      real one needs the transport redesigned around it.
- [ ] **Verify the engine decoders on Windows.** The desktop swap is done and
      proven on Linux (FLAC decodes bit-identically at 16 and 24 bit; MP3 and
      tag-scanning verified), but the Windows half is written blind: the
      `_wsopen_s` path in `decoder.cpp`, MSVC flags in `ae_mpg123.cmake`, and
      above all a **desktop `config.h` for libmpg123**, which does not exist for
      Windows yet — `ae_add_mpg123` hard-errors there with instructions. Build
      with `scripts\windows\build.ps1` and fix what falls out.

## Listening analytics

Built. `play_events` (`core/src/db_stats.cpp`) is the one stored truth about
listening: a row per listen, keyed on `trackKey()`, carrying how much was
really heard, whether it finished, why it started and why it ended. Every
count, ranking and histogram is computed from it on demand — no derived
counter is stored, so nothing can drift. Pinned by `core/tests/stats_test.cc`.
See CLAUDE.md's "Listening analytics" section for the parts that are
load-bearing.

- [x] **Stable identity.** History was keyed on `file_path`, so renaming a
      folder orphaned it silently and forever. `trackKey()` folds
      albumArtist ⨯ album ⨯ title ⨯ disc ⨯ track with Edition modifiers
      stripped, so a re-rip at higher quality and a "(Deluxe)" retag both keep
      one history. Existing rows were backfilled from the metadata already in
      the database — no rescan needed.
- [x] **A log worth querying.** The old `play_history` stored only
      *(path, when)*. `play_events` adds ms actually heard, completion, local
      UTC offset, and `StartCause`/`EndCause`. Sessions are derived from the
      timestamps at query time rather than materialised.
- [x] **Honest listen time.** `flushTrackStats()` used to read `seekPosMs_`,
      the last known position — so skipping to the last minute of a track
      counted that minute as heard, and replaying a stretch counted it once.
      Now `onTimer()` sums each plausible forward step and ignores seeks.
- [x] **Schema versioning** (`PRAGMA user_version`), so one-shot data work can
      exist at all. The old blind-`ALTER` array stays for column adds.
- [x] **Genre and year** parsed from Vorbis comments, `ORIGINALDATE` preferred
      over `DATE` so a remaster reports the music's year, not the reissue's.
- [x] **The aggregate API**: totals, top tracks/albums/artists/genres, local
      hour histogram, daily buckets, recently played, per-track totals, skip
      rate, sessions.
- [x] **A calendar day is one bucket, whatever the offset did inside it.**
      `dailyListening()` and `Totals::activeDays` grouped on the UTC instant of
      local midnight — an instant that moves with the offset, so a
      daylight-saving Sunday or a day spent flying split into two rows and
      counted twice. They group on the local DATE now (`LOCAL_DAY`), and
      `DayBucket::dayLocal` is a local second rather than a UTC one. An earlier
      version of `stats_test` had this assertion backwards; it now pins both
      directions.
- [x] **An album is title AND artist.** `topAlbums()` grouped on the title
      alone, so two artists with a record called *Live* merged into one row
      under whichever name `MAX()` picked. Latent on this library — no
      collisions in 57 albums — but the same class of error as the `TRACK_REP`
      fan-out, in the other direction.
- [x] **Rankings can be ordered by time listened**, not only by play count
      (`TopSort`). Different questions: on the test library *Genesys II* leads
      by plays with 11 and by time with zero, because every one of those plays
      is a migrated row that recorded no duration.
- [x] **Range presets on local midnights** — `rangeFor()` in the new, pure
      `core/src/stats.cpp`, alongside the civil-date arithmetic a calendar view
      needs. Integer math, no `<ctime>`, so it is exactly testable.
- [x] **`recentlyPlayed()` matches the rankings**: takes a range, and resolves
      its labels through the same LEFT JOIN, so a deleted track keeps its place
      in the history unnamed instead of vanishing from it alone.
- [x] **`trackTotals()` reports first and last play**, out of the scan it was
      already doing.

Next, in the order it would be picked up:

- [ ] **A statistics screen.** The data is ready and tested; nothing draws it.
      Full-page overlay over the content area, same pattern as the settings
      panels (`gui/src/panels/settings_panels.hh`). **Decided**: reached from a
      fifth row on the Settings page (`Listening Statistics`, beside
      `EQ / AutoEQ Profiles`), not from the sidebar — the headphone switcher
      already occupies the sidebar's bottom. Needs its own design pass against
      `docs/UI_DESIGN_SYSTEM.md` — this is the first part of the app that is
      mostly numbers.
      - Note for that pass: on a library upgraded from `play_history`, most
        events are migrated rows carrying `ms_heard = 0` (43 of 51 on the test
        library). Everything time-based reads near-empty at first. The screen
        has to say so rather than disguise it.
- [ ] **ID3 tag reading**, so MP3s carry genre and year. Today only Vorbis
      comments are parsed, which leaves every MP3 out of `topGenres()` and out
      of any year-based view.
- [ ] **Explicit signal** (love / rating). Everything recorded so far is
      implicit; a playlist generator built only on play counts cannot tell
      "on heavy rotation" from "actually a favourite".
- [ ] **Playlist generation**, once the above exist. `start_cause` is the
      field that makes this possible — a track the listener chose has to
      weigh more than one that merely played next.

## Database

- [ ] Artwork: pick the highest-resolution candidate when an album folder has
      several. Today `COVER_NAMES` (`core/src/library.cpp`) picks by filename
      priority alone and never opens two candidates to compare.
      *(The cache half of this item is done: `gridArtTexCache_` has LRU
      eviction at `kMaxGridArtTextures`, and is now keyed by
      `artKey(albumIdx, sizeClass)` — the mosaic quadrants needed a half-size
      decode of the same cover.)*

---

*Removed from this list because they are done: USB driver wiring, gapless
playback, the parallel scan, the SQLite scan cache (size+mtime), libjpeg-turbo
artwork decode, the EQ processor port, FLAC Vorbis tag parsing, track-number
sorting, the album grid, the dark theme and borderless custom chrome, and the
sidebar's settings entry — pulled out of the type filters, below a hairline,
now a labelled `Settings` row rather than a gear icon; the `playback_state`
table with resume-on-launch; per-element cursor shapes; and keep-screen-on for
the fullscreen art window. The `play_history` / `track_stats` tables were
superseded by `play_events` — see "Listening analytics" above; both remain on
disk as the only checkable record of what was migrated.*

*Dropped rather than done: the **audio-server** daemon — JACK already fills
that role. **Discarded** by decision: the warning/alert HUD restyle, and
replacing the `DISC n` label.*

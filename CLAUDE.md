# CLAUDE.md

Guidance for Claude Code (claude.ai/code) working in this repository.

## What this project is

Matrix Player — a native C++17 music player that drives a USB DAC **directly**
via a first-party `audio_engine` library (libusbK/libusb, bypassing the OS
audio mixer entirely) and renders its GUI through a first-party Vulkan engine,
`vk_canvas`. **Windows and Linux are equal peers** — both build a real,
running GUI from this same source tree. ALSA and JACK2 are Linux's secondary
output backends (parallel to WASAPI on Windows); USB direct is primary and
bit-perfect everywhere.

Both engines are consumed as git submodules, each authored by "minervarr" and
developed independently — read their own `CLAUDE.md` files
(`framework/audio_engine/CLAUDE.md`, `framework/vk_canvas/CLAUDE.md`) before
touching anything inside them.

---

## Repository layout

```
matrix_player/
  core/                       — pure C++17 app logic, zero OS headers (one exception, see below)
    include/core/             — public headers: library.h, variants.h, decoder.h, db.h,
                                 stats.h, eq_manager.h, eq_profiles.h
    src/                      — library.cpp, variants.cpp, decoder.cpp, db.cpp,
                                 db_stats.cpp, eq_manager.cpp, eq_profiles.cpp
    src/variants.cpp           — album-variant grouping: one release held twice (another
                                 edition, another quality, or another remix set) becomes
                                 ONE grid tile, the rest listed in the album view. Also
                                 holds two things moved here to be testable —
                                 splitNameModifier() (out of player_view.cc: grouping and
                                 drawing must split names identically) and
                                 classifyReleaseType() (out of library.cpp), plus the
                                 multilingual edition/remix term tables classifyModifier()
                                 reads and the compilation table isCompilationAlbum()
                                 reads — a release type decided by NAME alone, because
                                 the two obvious signals (year spread, titles reused from
                                 other records) were measured on the real library and both
                                 rank ordinary albums ABOVE actual anthologies; see the
                                 comment on isCompilationAlbum() in core/variants.h.
                                 isLiveAlbum() is the sixth type and shares the REMIX
                                 machinery — everyDiscIsMajority(), the per-disc rule the
                                 Genesys II case forced — because "Edición Especial"
                                 reissues pair a studio disc with a live bonus disc and
                                 read 32-48% live when counted flat Also trackKey() — the stable identity the
                                 LISTENING LOG is recorded against, folded from
                                 albumArtist ⨯ album ⨯ title ⨯ disc ⨯ track with
                                 Edition modifiers stripped, so renaming a folder or
                                 re-ripping at a higher rate never orphans history.
                                 PURE: no OS, no Canvas, no sqlite — variants_test
                                 links it alone. Keep it that way
    include/core/stats.h       — listening-analytics vocabulary: StartCause/EndCause,
                                 the aggregate result types (Totals, TopEntry,
                                 HourBucket, DayBucket, PlayEvent, SessionStats) and
                                 the knobs the queries take (TopSort, RangePreset,
                                 StatsRange). No sqlite, no OS
    src/stats.cpp              — the PURE half of the above: Hinnant civil-date
                                 arithmetic plus rangeFor(), which turns "the last
                                 seven days" into unix seconds on LOCAL midnights.
                                 Integer math only — no <ctime>, no TZ database — so
                                 the answer is the same in every zone and can be
                                 asserted exactly. This is deliberately not in the
                                 GUI: a range boundary looks obvious, is not, and is
                                 invisible when wrong
    src/db_stats.cpp           — the play_events log and EVERY query derived from it.
                                 Split out of db.cpp, which was already carrying the
                                 library/settings/roots/EQ halves. Both define methods
                                 on the same Db over one connection; the seam is the
                                 src-private db_schema.h. The rule this file enforces:
                                 play_events is the ONLY stored truth about listening,
                                 so no derived counter is written anywhere and nothing
                                 can drift out of step with the log
    src/os/                   — the ONE platform split in core/: FolderWatcher's backend
                                 (windows_folder_watch.cpp: ReadDirectoryChangesW;
                                  linux_folder_watch.cpp: inotify) behind a PIMPL'd
                                 FolderWatcher::Impl — library.h itself has no OS types.
    tests/variants_test.cc     — assert-based, Debug-only, same convention as gui/'s two
    tests/stats_test.cc        — the listening log + its queries, over an in-memory DB.
                                 Links db.cpp + db_stats.cpp + variants.cpp + sqlite3
                                 and DELIBERATELY nothing else — no matrix_core, no
                                 ae_core, no library.cpp. The data layer must not need
                                 a decoder or a filesystem walk to answer a question
                                 about history; if this target ever needs another
                                 source, something leaked in
    CMakeLists.txt             — builds matrix_core (STATIC), links ae_core + sqlite3
  gui/
    src/
      gui_main.cc              — portable entry point: env/self-test parsing, constructs
                                  PlayerWindow, calls create()/run()
      player_view.hh/.cc        — the app: layout, drawing (Canvas), hit-testing, playback
                                  orchestration, gapless coordinator. Never touches a raw
                                  HWND/HMONITOR/wl_* type directly — everything real-OS goes
                                  through host_ (a Host*, see host.hh)
      host.hh                  — the Host interface: window creation, message pump, monitor
                                  info, timers, cross-thread events. Two implementations:
      os/windows_host.cc        — real Win32 window/message pump (WM_* dispatch, DPI,
                                  minidump crash handler, WinMain bootstrap)
      os/linux_host.cc          — real Wayland backend (vk_canvas's WaylandDisplay/
                                  WaylandWindow), timerfd for the seek-update timer,
                                  eventfd-woken cross-thread event queue, main() bootstrap
      os/alsa_output.cc/.hh     — Linux secondary output: thin AudioOutput adapter over
                                  audio_engine's AlsaSink (only built if ALSA was found —
                                  see MATRIX_HAVE_ALSA)
      os/jack_output.cc/.hh     — Linux secondary output: thin AudioOutput adapter over
                                  audio_engine's JackSink (MATRIX_HAVE_JACK)
      wasapi_output.cc/.hh      — Windows secondary output (unchanged, Windows-only)
      panels/settings_panels.hh/.cc — shared row-list/button/panel-header widgets used by
                                  all four settings panels (see "Settings panels" below)
      theme.hh                  — the color palette; player_view.cc and the panels both
                                  draw from this one place
      color.hh, layout_rect.hh  — portable ColorRef/LayoutRect (replace COLORREF/RECT)
      app_paths.hh/.cc          — WHERE FILES LIVE, and the one rule about it:
                                  READ-ONLY data (fonts/, assets/shaders/,
                                  eq_profiles.json) is exe-relative and stays that way;
                                  everything WRITTEN (matrix_player.db, the log, the
                                  ~45 MB atlas cache pruneStaleCaches() also deletes
                                  from) goes through stateDir(). The two are the same
                                  directory by default, so a build tree and the
                                  dist/linux/ tarballs remain one self-contained folder
                                  you can move anywhere. -DMATRIX_STATE_HOME=.matrix_player
                                  splits them, sending the writable half to $HOME/<name>/
                                  — what a system package needs, since /opt is root-owned
                                  and the atlas cache is per-user regardless (it holds
                                  whatever scripts the listener's own library contains).
                                  Deliberately NOT XDG; a plain dotdir, no spec involved.
                                  Also the single home of exe-directory discovery, which
                                  had drifted into four copies (both hosts, openLogFile,
                                  tools/ui_capture)
      ui_metrics.hh/.cc         — the single scale factor: 5 type roles from one ratio,
                                  plus space()/stroke(). Tested by ui_metrics_test.cc
      ui_fonts.hh               — the ONE place the UI face paths and the atlas cache
                                  name live. PlayerWindow and ArtWindow share that cache,
                                  so both must agree on it; they used to keep duplicate
                                  literals (three copies, counting ArtWindow's Windows
                                  branch). The cache name embeds the icon-set fingerprint
                                  so redrawn icons can't be served from a stale bake
      ui_icons.hh/.cc           — UI icons as MTSDF atlas glyphs: codepoint table +
                                  placement math. PURE (no Canvas/Vulkan) so
                                  ui_icons_test.cc links it directly — keep it that way
      ui_icons_draw.cc          — the Canvas-dependent half (drawUiIconGlyph), split off
                                  purely to keep the above testable
      ui_icons.gen.h            — GENERATED by tools/icon_font/build_icon_font.py
      hotkey_ids.hh, art_view.hh/.cc, audio_output.h, log_util.h
    CMakeLists.txt              — builds the matrix_player executable, per-platform source/lib lists
  framework/
    audio_engine/               — git submodule (github.com/minervarr/audio_engine).
                                  core/ (pure C++) + backends/{usb,alsa,jack,wasapi,flac,mp3,dsd}/
                                  + api/ (C ABI, not used by this app — we link the C++ targets
                                  ae_core/ae_usb/ae_alsa/ae_jack directly)
    vk_canvas/                  — git submodule (github.com/minervarr/Vk_Canvas_Lb_LAW).
                                  core/ (Renderer, Canvas, MSDF text, platform.hh seam) +
                                  platform/windows/ (Win32SurfaceProvider) +
                                  platform/linux/ (WaylandDisplay/WaylandWindow — real, not a stub)
  third_party/
    dr_wav.h                    — single-header WAV decoder (mackron/dr_libs), vendored
    dr_flac.h                   — no longer used by the player (decoding moved to
                                  audio_engine's libFLAC); still included by
                                  tools/ab_test.cpp, which is Windows-only and opt-in
    sqlite3.c/.h                — SQLite amalgamation, vendored
    soxr/                       — git submodule (resampler)
    libjpeg-turbo/              — git submodule (JPEG art decode, built via ExternalProject_Add)
  assets/fonts/                 — New Computer Modern (UI face; see gui/src/ui_fonts.hh, the ONE
                                  place font paths + the atlas cache name live) + multi-script
                                  fallback faces (CJK/Hangul). Greek/Cyrillic come from NewCM
                                  itself, so base and fallback text are one design
    icons/matrix-icons.otf      — the UI icon glyphs, generated (see tools/icon_font/)
  scripts/
    linux/build.sh               — cmake+ninja -> build/linux/ (see flags below)
    windows/build.ps1            — vswhere -> vcvars64 -> cmake+ninja -> build/ or build_debug/
  docs/
    UI_DESIGN_SYSTEM.md          — the GUI's visual language (colors, type, layout rules) —
                                  single source of truth alongside theme.hh
    ref_eq_pipeline.md           — Reference EQ signal chain (biquad → resample → dither/quantize)
  tools/
    ab_test.cpp                  — Windows-only A/B EQ listening-test tool (matrix_ab_test),
                                  opt-in via -DMATRIX_BUILD_AB_TEST=ON — not built by default
    icon_font/                   — UI icon pipeline: Inkscape SVGs (icons/*.svg) ->
                                  build_icon_font.py -> assets/fonts/icons/matrix-icons.otf
                                  + gui/src/ui_icons.gen.h. Both outputs are COMMITTED, so
                                  the C++ build never needs Python. See its README.md —
                                  Path>Union is mandatory (MSDFGEN_USE_SKIA=OFF, no
                                  overlap resolver), and each icon's design box is sized
                                  to where it's drawn (denser is NOT better — over-baking
                                  causes minification smear)
    icon_preview/                — Debug+Linux-only standalone window drawing ONLY the
                                  icons, at a size ladder + the app's real sizes. Run
                                  ./build/linux_debug/gui/icon_preview to judge icon
                                  sharpness without launching the player
  manifest.json                 — name/version/platforms/audio backends, machine-readable
  CMakeLists.txt                 — root: third-party targets (sqlite3/soxr/libjpeg-turbo/shaders),
                                  add_subdirectory(framework/...), add_subdirectory(core), add_subdirectory(gui)
  git_wrapper[.exe]              — mandatory commit tool, see USAGE_gitWrapper.md
```

**The `core/` rule**: zero OS headers, checked by `grep -rn "windows.h" core/` returning
nothing except inside `#ifdef _WIN32` blocks. The one intentional platform split is
`FolderWatcher`: its public interface (`core/include/core/library.h`) holds only an
opaque `std::unique_ptr<Impl>`; the real HANDLE/inotify-fd state lives in
`core/src/os/{windows,linux}_folder_watch.cpp`, picked by CMake's
`$<IF:$<PLATFORM_ID:Windows>,...>` generator expression.

---

## Build

**Linux** (this machine): needs cmake ≥ 3.22, ninja, GCC/Clang with C++17, ALSA
headers, jack2 dev headers (**never** pipewire-jack — verify with `ldd` + `pacman -Qo`
after building), wayland-client/wayland-cursor/xkbcommon dev headers, a Vulkan
loader + headers, and the Slang shader compiler (`slangc`).

```bash
scripts/linux/build.sh                 # interactive: prompts for microarch, then Release/Debug
scripts/linux/build.sh --release       # non-interactive Release -> build/linux/
scripts/linux/build.sh --debug         # non-interactive Debug -> build/linux_debug/
                                        #   (also builds audio_engine's smoke-test tools + matrix_ab_test)
scripts/linux/build.sh --clean         # wipe the target build dir first (combine with a mode flag)
scripts/linux/build.sh --share         # Release: universal + v3 + v4 + znver4 variants,
                                        #   packaged as tarballs under dist/linux/
```

Any non-interactive invocation (mode flag passed, or stdin not a TTY — e.g. CI)
defaults to Release/Universal. Extra args after the flags above pass straight
through to `cmake` (e.g. `-DMATRIX_ARCH_LEVEL=v4`, `-DMATRIX_BUILD_AB_TEST=ON`).

`vk_canvas`'s `VceShaders.cmake` resolves `slangc` from `$VULKAN_SDK/bin/slangc`,
falling back to a hardcoded **Windows** path if unset (that cmake file belongs to the
vk_canvas submodule — never patched in place). `scripts/linux/build.sh` already probes
for a `slangc` on `PATH` or at a couple of known locations and passes
`-DVCE_SLANGC=...` automatically; pass it yourself if your `slangc` lives elsewhere:
`scripts/linux/build.sh -DVCE_SLANGC=/path/to/slangc`.

Output: `build/linux/gui/matrix_player` (plus `matrix_player.log` and `eq_profiles.json`/
`fonts/`/`assets/` copied next to it by the build's POST_BUILD steps).

**Windows**: Visual Studio Build Tools (MSVC `cl.exe`), CMake, Ninja, libusbK driver
bound to the target USB DAC via Zadig.

```bat
scripts\windows\build.ps1            :: Release -> build\
scripts\windows\build.ps1 -Debug     :: Debug -> build_debug\ (smoke-test tools + matrix_ab_test)
scripts\windows\build.ps1 -Clean     :: wipe the target build dir first
scripts\windows\build.ps1 -V3        :: or -V4, x86-64 psABI microarch level
```

Output: `build\matrix_player.exe` (Release) or `build_debug\matrix_player.exe` (Debug).

**Both platforms**: submodules first if empty —
`git submodule update --init --recursive`.

**Tests**: there is no ctest/gtest framework, but there are four assert-based
pure-logic test executables, built **Debug-only** (see the bottom of
`gui/CMakeLists.txt` and of `core/CMakeLists.txt`) and run directly. Convention
matches `framework/vk_canvas/core/tests/*.cc`: plain `assert()`, `#undef NDEBUG`
in the test source so asserts survive an optimized build, no framework or
linkage against the engine.

```bash
scripts/linux/build.sh --debug
./build/linux_debug/gui/ui_metrics_test    # type scale + spacing/stroke math
./build/linux_debug/gui/ui_icons_test      # icon codepoints + placement math
./build/linux_debug/core/variants_test     # album-variant grouping, edition terms, trackKey()
./build/linux_debug/core/stats_test        # listening log, aggregate queries, schema migration
```

Keep them pure. `ui_icons.cc` is deliberately split from `ui_icons_draw.cc`
precisely so the test links the real placement code without dragging in
Canvas/Vulkan — don't collapse them back together.

The audio_engine submodule adds two more, same convention, same Debug-only
gating:

```bash
./build/linux_debug/framework/audio_engine/dsp_null_test   # DSP bit-exactness gate
./build/linux_debug/framework/audio_engine/dsp_bench       # ns/sample for the same paths
```

**`dsp_null_test` is the gate for any change to the audio path.** It holds
frozen copies of the EQ, the USB wire packing, and the dither/quantize stage as
they stood before optimization, and asserts the live code still produces
identical output. If it fails, the audio changed — that is the whole point.
Speed on this path comes from removing overhead, never from touching the
signal; see rule 9 in `framework/audio_engine/CLAUDE.md`.

**`-ffp-contract=off` is why `MATRIX_ARCH_LEVEL` is safe** (root
`CMakeLists.txt`, next to `-fno-math-errno`). GCC defaults to
`-ffp-contract=fast`, which fuses `a*b + c` into a single FMA wherever the ISA
has one. At the generic baseline there is no FMA to fuse into; at v3/v4/native/
znver4 there is, and the EQ's biquad accumulator then carries a wider
intermediate than the frozen oracle. Measured, not assumed: `-march=native`
with contraction at the default **fails** `dsp_null_test` test [2] at
`dsp_null_test.cpp:481`; with the flag it passes all 15681922 checks. This was
found while packaging, and it means every `--share` v3/v4/znver4 tarball built
before this flag existed shipped an EQ that did not match the reference. The
flag is global rather than scoped to the `ae_*` targets on purpose — the DSP
primitives are headers, so they compile into consumers like `player_view.cc`
too, and a per-target flag would miss exactly the copies that matter.

Hardware smoke tests, when a DAC is plugged in:

```bash
MATRIX_ISO_TEST=1 ./build/linux/gui/matrix_player   # 20 s 440 Hz through the iso path
```

then check `matrix_player.log` for `underrun` / `Transfer status` lines — a
clean run logs neither.

Everything else is manual. `tools/ab_test.cpp` (`matrix_ab_test`, Windows-only,
opt-in via `-DMATRIX_BUILD_AB_TEST=ON` or the Debug build presets above) is an
A/B listening-comparison tool for EQ changes, not an automated check. Validate
audio/DSP changes by building and listening; validate GUI changes by building
and running `matrix_player`, then exercising the affected panel directly.

---

## Listening analytics (`core/src/db_stats.cpp`, `core/include/core/stats.h`)

`play_events` is the **only stored truth** about listening — one row per
listen, opened when a track reaches the transport and closed when it leaves.
Nothing writes a derived counter anywhere, so no aggregate can drift out of
step with the log. Play counts, rankings and histograms are all computed on
demand from it.

These things are load-bearing and easy to undo by accident:

1. **Keyed on `trackKey()`, never on `file_path`.** A path is rewritten
   whenever a folder is renamed, and history keyed on it orphans silently and
   unrecoverably. `file_path` rides along as *provenance* — which copy of the
   track actually played — but it is not the identity.
2. **`utc_offset_min` is stored per event, not derived at query time.** "What
   hour do you listen at" is a question about local clocks; deriving it later
   smears every answer across a daylight-saving change or a move between
   timezones. `stats_test` pins this with two events at one UTC instant and
   two offsets.
3. **`start_cause` cannot be recovered after the fact.** A track the listener
   *chose* says far more about taste than one that merely played next, which
   is why it is recorded rather than inferred — and why `onNext()`'s seamless
   path has to bank its own outgoing event before handing off to the gapless
   coordinator (otherwise a listener's skip logs as an ordinary advance).
4. **Every join to `tracks` goes through `TRACK_REP`, never straight on
   `track_key`.** Because `trackKey()` merges editions and qualities on
   purpose, `tracks` holds several rows per key — so a plain
   `JOIN tracks ON track_key` fans out and counts one listen once per copy on
   disk. This is not hypothetical: on the test library it read 52 plays where
   the log held 43, and 27 of 518 track rows shared a key with another. The
   regression case in `stats_test` puts three copies of one track behind one
   key and asserts no ranking totals more than the log holds.
5. **A calendar day is grouped by its LOCAL DATE (`LOCAL_DAY`), never by the
   UTC instant of local midnight.** That instant *moves with the offset*, so
   grouping on it splits one calendar day into two rows whenever the offset
   changes inside it — a daylight-saving Sunday, or a day spent flying — and
   makes `Totals::activeDays` count that day twice. `DayBucket::dayLocal` is
   therefore a **local** second, already shifted, always a multiple of 86400:
   convert it with `statsCivilFromLocalDay()`, never with `localtime()`, which
   would shift it a second time. `stats_test` pins both directions — one
   calendar day stays one bucket across two offsets, and two genuinely
   different local dates still split.
6. **An album's identity is title AND artist.** `topAlbums()` groups on both.
   Two artists can each have a record called "Live", and grouping on the title
   alone merged them into one row whose artist column then read as whichever
   `MAX()` happened to pick — a wrong count under a misleading name. Same class
   of mistake as the fan-out above, in the other direction. `TopEntry::key`
   carries the pair joined by U+001F; `label`/`subLabel` carry the halves.
7. **Calendar arithmetic lives in `core/src/stats.cpp`, and is pure.** Integer
   math (Hinnant's civil algorithms), no `<ctime>`, no TZ database consulted
   behind its back — which is the only reason `rangeFor()`'s "last seven days"
   can be asserted to land on an exact local midnight. Range presets belong
   there and not in the GUI, where nothing can test them.

## Driver AutoEQ profiles (`eq_headphones`)

`eq_assignments` is keyed by **device** — VID:PID, `"alsa"`, `"jack"`. That was
never the real relationship: a DAC has no frequency response, the DRIVERS do, and
several pairs take turns on one output. So the two tables split the job:

- **`eq_assignments`** — which pair is on *this output* right now. Read by
  `applyDeviceEq()` on every track start; unchanged in shape.
- **`eq_headphones`** — the listener's inventory, global across outputs. What
  the sidebar quick-switcher lists. (Table and method names still say
  *headphone*; the UI says **DRIVER'S AUTOEQ**, because the list serves IEMs and
  speakers too and "drivers" alone would read as an output driver here. Renaming
  the schema would be a migration bought for nothing.)

Five things here are load-bearing:

1. **A row is earned, not selected.** `selectEqProfile()` applies a profile
   immediately, but `creditEqHeadphone()` only runs after `kEqCreditMs` (60 s)
   of audio that actually reached the DAC. This exists because the list is
   worthless once a stray click can occupy a row permanently. The gate rides on
   `statsMsHeard_`, which the listening log already accrues every 250 ms — there
   is no second timer, and adding one would be the wrong fix.
2. **`eqCreditBaselineMs_` is not optional.** `statsMsHeard_` counts the
   *track*, so swapping profiles three minutes in would credit the new pair
   instantly without a baseline taken at selection time.
3. **`creditEqHeadphone()` is two statements, never `INSERT OR REPLACE`.**
   Replacing the row would wipe `pinned` and `use_count`, silently unpinning a
   pair every time it played — and the prune would then be free to evict it.
4. **`clearEqProfile()` deletes the `"global"` assignment as well as the active
   device's.** It is the sidebar's `No AutoEQ` row and the EQ panel's `Clear`
   button, one implementation. `applyDeviceEq()` runs at EVERY track start and
   falls back to a `"global"` row when the device key has none, so clearing only
   the device key lets such a row put the profile back on the next song — "off"
   that un-sticks by itself. Nothing has ever *written* `"global"`
   (`saveEqAssignment` has one call site and it always passes
   `getActiveDeviceKey()`), so any such row is legacy.
5. **The sidebar shows what FITS, and it is not four.** `kEqHpMaxRows` is 4, but
   the sidebar holds three rows below Settings, so `drawHeadphoneBlock()` clamps
   the saved list to the space that exists and drops rows from the LIST rather
   than hiding the block — the header, `No AutoEQ` and `Search more…` are its
   minimum. Order is pinned → most-used → most-recent (`loadEqHeadphones`), and
   with that few rows on screen the ordering is what decides reachability.

The prune keeps 12 unpinned rows; pinned rows are exempt and are not counted
against that budget. Pinning and removing happen only in the EQ panel's
`My Drivers` tab — the sidebar block is a switcher and nothing else, its
`No AutoEQ` row being a switch position rather than an edit.

### Schema versioning

Two mechanisms in `db.cpp`, with a strict division of labour:

- **`MIGRATIONS[]`** — blind `ALTER TABLE ... ADD COLUMN`, fired one statement
  at a time with errors ignored, because a re-run failing with "duplicate
  column name" *is* the "already applied" signal. Right for adding a column
  and nothing else. **Never fold these into `SCHEMA`**: `sqlite3_exec` stops a
  multi-statement string at the first error, so one expected failure would
  silently skip everything after it.
- **`SCHEMA_STEPS[]`** — one-shot data work, gated on `PRAGMA user_version`,
  each step in its own transaction. Re-running the `play_history` backfill
  would duplicate the entire listening log on every launch, which is exactly
  what the version guard exists to prevent. Version 1 is the baseline (the
  schema before analytics); a fresh database is stamped current and skips
  every step.

### Documented gaps

- Only Vorbis comments are read, so **MP3s carry no genre and no year** —
  nothing parses ID3 yet. `topGenres()` excludes empty genres rather than
  bucketing them as "Unknown", which would otherwise top the chart meaning
  nothing.
- `topAlbums`/`topArtists`/`topGenres` need the join to `tracks`, so a track
  deleted from the library drops out of *those* rankings. Its plays survive
  everywhere else, including `topTracks` — that is the one place a deletion
  costs history, and it is asserted in `stats_test`.
- `play_history` and `track_stats` are left on disk untouched, with no
  writers. They are the only remaining copy of the pre-migration aggregates,
  and a migration nothing can be checked against is one nobody can trust.
- `StartCause::Shuffle` has no producer yet — the app has no shuffle.
  (`StartCause::Playlist` DOES have one now: the Playlists panel, see below.)

## Playlists (`drawPlaylistSection` in `gui/src/player_view.cc`)

The consumer of the generated-playlist queries. Three lists — Heavy Rotation,
Forgotten Favourites, Never Heard — behind one sidebar row.

**It is a SECTION, not a panel.** `PlayerWindow::NavSection` says which of the
seven sidebar rows is showing; Albums/EPs/Singles/Compilations/Live/Remixes are
one section over six release types (`albumTypeFilter_`), Playlists is the
other. That row order is deliberate and is NOT the enum's order (the enum is
frozen by the `albums` table); it lives in `recalcLayout()`. It draws into
the same content area with the same two levels the album section has — a grid
of tiles, then one of them opened full-page — and everything outside that area
keeps working.

It used to be `SettingsPanel::Playlists`, borrowing the settings overlay. Do
not put it back: the panel dispatchers (`onPanelClick`/`onPanelMouseMove`/
`onPanelWheel`/`onPanelKeyDown`) divert every event before the sidebar or the
transport bar is hit-tested, so while a playlist was up you could not click
Singles, could not click Settings, and could not press Space to stop the music.
Deliberately NOT another `AlbumTypeFilter` value either — that enum is cast
straight to `Album::ReleaseType`, and a value with no release type behind it
would empty `gridIndices_`, which `nextAlbumInSection()` also reads.

A tile's artwork is generated (`drawPlaylistTileArt`): for an ordered list a
2×2 mosaic of its top entries' covers, **quadrant 1 = top-right = first place**,
then anticlockwise; the fourth quadrant is the fourth record's cover at exactly
four, and a fade to black past that. Covers are distinct RECORDS, not rows.
Unordered lists get a flat treatment instead (custom image / solid colour /
gradient — Never Heard is generated, so it gets the gradient).

Two more things here are load-bearing:

1. **`neverHeard()` takes `limit = 0` meaning UNLIMITED**, the opposite of every
   other query in `Db`, where `limit <= 0` returns nothing. The call sites use
   separate named constants precisely so a later edit cannot collapse the two
   conventions into one shared variable.
2. **Every manual track pick calls `clearQueue()` first.** Without it
   `queueActive()` stays true after the first playlist play, and every later
   manual choice is logged as `StartCause::Playlist` — which `IS_AFFINITY`
   excludes, so the generated lists quietly stop learning from real choices.
   Nothing crashes; only the history goes wrong.

3. **A tile's `albums[]` holds album INDICES, so `onScanDone()` has to rebuild
   it.** Every index into `albums_` dies on a rescan; a stale one paints some
   other record's cover into a rank quadrant, which looks like working art and
   is simply wrong. `loadPlaylistCovers()` takes `albumsMu_` itself, so it must
   run outside the scope that just held it — that lock is not reentrant.

`TopEntry` carries no duration, so each row's key is resolved through
`trackKeyIndex_` once per load under a single `albumsMu_` lock — never per row
per frame, since that lock is shared with the gapless thread.

**Playing from a playlist does not move the view.** `onPlay()` retargets the
album view at whatever started, but never while the listener is in this
section: starting a track from a list is not a request to leave the list, and
the transport bar already says what is playing. `applyTrackMetadata()` follows
the same rule at a gapless boundary — it only re-points the album view when
that view was already following the music.

---

## Audio engine (`framework/audio_engine/`)

**`UsbAudioDriver`** (`framework/audio_engine/backends/usb/usb_audio.h`) — the
primary, bit-perfect path on both platforms:

```cpp
UsbAudioDriver driver;
driver.open(0x32BB, 0x0004);   // Hiby FC4: VID=0x32BB PID=0x0004
driver.parseDescriptors();
driver.configure(44100, 2, 16);
driver.start();
driver.writeFloat32(pcmData, numSamples);  // call from the decode loop
driver.stop();
driver.close();
```

`open(vid, pid)` is the same signature on Windows and Linux (both go through
libusb/libusbK); this app wires it into `PlayerWindow::onPlay()` via the
`AudioOutput` interface (`gui/src/audio_output.h`) — see `UsbAudioOutput`.

### Secondary output backends (Audio Settings panel)

| Platform | Secondary backend(s) | Adapter |
|---|---|---|
| Windows | WASAPI (shared/exclusive) | `gui/src/wasapi_output.hh/.cc` |
| Linux | ALSA (system default device) | `gui/src/os/alsa_output.hh/.cc` — wraps `AlsaSink` |
| Linux | JACK (auto-connects to physical outputs) | `gui/src/os/jack_output.hh/.cc` — wraps `JackSink` |

ALSA/JACK are only compiled in when `audio_engine`'s own CMake found their dev
headers (`ae_alsa`/`ae_jack` targets exist) — `gui/CMakeLists.txt` checks
`if(TARGET ae_alsa)` and defines `MATRIX_HAVE_ALSA`/`MATRIX_HAVE_JACK`
accordingly, so the Audio Settings panel only ever offers backends this build
actually has.

Three things about switching between them are load-bearing:

1. **A backend is closed by NAME, never by a destructor.**
   `applyAudioSettingsPanel()` and `shutdown()` call `output_->stop()` +
   `output_->close()` and then `reset()`. Leaving it to `unique_ptr`'s
   assignment ran a whole backend's teardown inside a Wayland click callback,
   and switching JACK → ALSA there killed the app outright. `onStop()` alone is
   not enough — it stops the output and never closes it.
2. **A JACK client handle can die while the pointer still looks fine.** When
   the server goes, libjack frees the client object; `client` is then dangling,
   every `if (client)` guard still passes, and the next `jack_*` call jumps
   through a freed vtable. `JackSink::live()` is the only legal way to reach
   that handle and deliberately **leaks** it once `serverGone` is set — read
   its comment before "fixing" the missing `jack_client_close()`.
   `jack_on_shutdown` is registered in `open()`, not `start()`, because a
   client opened only to enumerate ports (which the settings panel does) must
   learn about it too.
3. **An audio failure must reach the screen.** It goes through
   `audioNotice_` (§8.7 of `UI_DESIGN_SYSTEM.md`) carrying
   `AudioOutput::lastError()` — the driver's own words. `Host::showErrorMessage`
   is stderr-only on Linux and is the log-side companion, not the report. And
   stderr only survives because `openLogFile()` `dup2`s it onto stdout's
   descriptor: opening the same log path twice gave the two streams independent
   offsets, and stdout's writes silently erased every `[AlsaSink]`/`[JackSink]`
   line.

### Tested device

- Hiby FC4 — VID `0x32BB`, PID `0x0004`, UAC2, High-Speed USB
- Windows: MI_00 (interface 0) must have **libusbK** bound via Zadig
- Linux: no driver *binding* needed (libusb talks to the kernel's usbfs
  directly, and `UsbAudioDriver::open()` auto-detaches `snd-usb-audio` itself —
  don't blacklist the module), but the device node **does** need permissions:
  `/dev/bus/usb/*` is root:root 0664, so a normal user can't open it
  read-write and the USB backend fails with `LIBUSB_ERROR_ACCESS`. Install
  `scripts/linux/70-matrix-player-usb.rules` (one-time, needs root once — see
  the header comment in that file); adding another DAC is one line there.
- Supports: 44.1k–768kHz PCM, 16/24/32-bit, DSD native (alt=4)

### DoP (DSD-over-PCM)

The packing itself is already implemented — not a `UsbAudioDriver` method.
It happens one layer up, at the decoder: `DsdPackager::packDop`
(`framework/audio_engine/core/dsp/dsd/dsd_packager.h`, called from
`backends/dsd/dsd_decoder.cpp`) produces pre-packed DoP bytes, which flow
straight through `UsbAudioSink::write()`'s existing raw-passthrough branch
(`backends/usb/usb_sink.cpp`) — the same path any other pre-packed PCM takes.
`UsbAudioDriver` itself needs no DoP-specific method, and `core/src/decoder.cpp:203`
already instantiates `ae::DsdDecoder` for DSD magic bytes.

What's still genuinely missing is one layer further up: the library
**scanner** (`core/src/library.cpp`) does not index `.dsf`/`.dff` at all, so
no DSD track is ever discovered and no file ever reaches the decoder this
way — that's why `hasDsd` is hardcoded `false`
(`core/src/library.cpp:66`). See TODO.md's "MP3 and DSD are not indexed at
all" item; that's the real remaining gap, not a missing `writeDop()`.

### Reference EQ signal chain

Full detail in `docs/ref_eq_pipeline.md`: Reference EQ (the non-bit-perfect
playback mode) runs biquads in 64-bit double precision, then quantizes to
int32 exactly once — before soxr resampling if the device rate differs from
the source, never before it — to avoid a second rounding error. Bit-perfect
mode is unrelated to this path and aborts outright on any format mismatch
rather than resampling.

The DSP primitives it rests on live in `framework/audio_engine/core/dsp/`:
`eq_processor.h` (cascade, SSE2/NEON stereo, no FMA), `dither.h`
(`ae::TpdfQuantizer`), and `round.h` (exact inline `llround`/`lrint`
replacements — both are libm *calls* otherwise, once per sample). All of it is
pinned by `dsp_null_test`.

---

## GUI engine (`framework/vk_canvas/`)

The GUI is **entirely vk_canvas-rendered** — no native OS controls anywhere in
the app (the four settings panels, described below, replaced the last native
Win32 dialogs). `player_view.cc` draws through `Canvas` (rect/text/image
primitives, MSDF text) and never allocates a raw window/control itself — the
real window, message pump, and monitor queries live behind `Host` (`host.hh`).

### The Host abstraction

```cpp
class Host {
public:
    virtual bool init(PlayerWindow* owner, UiMode initialMode) = 0;
    virtual SurfaceProvider& surfaceProvider() = 0;
    virtual AssetReader&     assetReader()     = 0;
    virtual MonitorInfo primaryMonitor() const = 0;
    virtual void pump(bool haveWork) = 0;
    virtual void postAppEvent(AppEvent id, intptr_t p1, intptr_t p2 = 0) = 0;
    virtual void startTimer(TimerId, int intervalMs) = 0;
    // ... window/mode/hotkey/error-dialog methods, see gui/src/host.hh
};
std::unique_ptr<Host> make_host();  // os/windows_host.cc or os/linux_host.cc
```

`PlayerWindow` calls `host_->` for anything OS-real; `Host::pump()` dispatches
back into `PlayerWindow`'s public `on*()` methods (`onMouseMove`, `onTimer`,
`onHostResized`, ...) — the same methods `windows_host.cc`'s old `wndProc`
switch and `linux_host.cc`'s Wayland callbacks both call into, so
`player_view.cc`'s layout/drawing/hit-testing code is identical on both
platforms.

**Mouse back/forward** (the two thumb buttons) reach `PlayerWindow::onNavBack()`
/ `onNavForward()` from both hosts. Windows needs only `windows_host.cc`
(`WM_XBUTTONDOWN`), but Linux needed the vk_canvas submodule: `PointerEvent::
button` only knew left/right/middle, so `input.hh` gained `3 = back, 4 =
forward` and `wayland_display.cc` gained the `BTN_SIDE`/`BTN_EXTRA` mapping.
That is a submodule change — commit it with `git_wrapper`, which pushes
submodules before the parent.

**What has no Wayland equivalent, by design** (documented narrowing, not a
silent gap): global hotkeys (Alt+F/J/C/U/G/H/L edge-snap/mode-toggle) are
system-wide `RegisterHotKey` calls on Windows but focused-window-only checks
on Linux (no cross-compositor equivalent); `adaptToCurrentMonitor()`/
`snapToEdge()` are no-ops on Linux (Wayland clients cannot query "which
monitor" or reposition themselves); Essential UI mode has no Linux window-
sizing logic yet (always opens at a 1200×700 default).

### Settings panels (`gui/src/panels/settings_panels.hh/.cc`)

Four vk_canvas-native panels replaced the app's last native OS chrome —
Manage Folders, Audio Settings, EQ Settings, and a from-scratch subfolder
browser (replacing `SHBrowseForFolderW` on **both** platforms, not just
stubbing it on Linux). They're full-page overlays over the content area
(the same pattern the album view already used), not modal popups — Wayland
has no child/owned-window primitive to build a real modal on. Shared
row-list/button/header widgets live in `settings_panels.cc`; per-panel
draw/click/hover logic lives in `player_view.cc` (`drawManageFolders`,
`drawAudioSettings`, `drawEqSettings`, `drawFolderPicker`, and the
`onPanel*` dispatchers).

### Visual language

`docs/UI_DESIGN_SYSTEM.md` is the written map of colors, type, and layout
rules (dark/serif/single-accent, square corners everywhere except the radio
dot, green reserved for state — never for mere hover). It cites `file:line`
into `theme.hh`/`player_view.cc` and is meant to be updated alongside the code
whenever the look changes, not left to drift.

---

## Design decisions (don't change without reason)

| Decision | Choice | Why |
|---|---|---|
| GUI | vk_canvas (Vulkan) | Custom-rendered, no OS control chrome anywhere — "squeeze the most of every platform," not generic dialogs bolted onto custom UI |
| Platforms | Windows + Linux, equal peers | Both build and run the real GUI from this tree; no platform is "the" project |
| Decoding | `audio_engine`'s own backends: libFLAC, libmpg123, DFF/DSF | First-party and already written for Android — one implementation decodes identically on every platform. Chosen by magic bytes, never by extension. Not FFmpeg |
| WAV | vendored dr_wav (single-header) | The one format the engine has no decoder for |
| DB | SQLite (embedded) | Single file, same model as the Android sibling player |
| USB driver | libusb/libusbK | Best isochronous support cross-platform; libusbK (Zadig) only needed on Windows |
| Audio stack | Bypassed entirely for the primary path | No WASAPI/PulseAudio mixer — raw USB isochronous to DAC |
| Linux secondary outputs | ALSA + JACK2 (never pipewire-jack) | Mirrors WASAPI's role: a fallback when no DAC is plugged in, or for testing without hardware |
| Album art (fullscreen) | Separate window (`ArtWindow`, both platforms) | Dual-monitor: art on one screen, controls on other |
| Submodules | `audio_engine`, `vk_canvas`, `soxr`, `libjpeg-turbo` | dr_flac + sqlite3 vendored directly (single-header / amalgamation, no submodule needed) |
| Build | CMake + Ninja | MSVC `cl.exe` on Windows, GCC/Clang on Linux, no `.sln`/Makefiles |
| `core/` | Zero OS headers (one PIMPL'd exception: FolderWatcher) | Portable app logic reusable without dragging in either platform's headers |

---

## Reference: Android sibling player

A separate Android music player by the same author shares this project's
architecture (scan strategy, artwork cache, gapless pipeline, EQ, DSD
handling). Its path is a **Windows-machine-specific local clone path** from
earlier work on this project and is **unconfirmed from this (Linux) machine**
— don't assume it still exists at any specific path; ask before relying on it.

---

## Committing

Use `git_wrapper` (`./git_wrapper` on Linux, `git_wrapper.exe` on Windows) —
**never** plain `git commit`/`git push`. It forces author/committer identity to
`nava <nava@noreply.com>`, strips stray `Co-Authored-By:`/"Generated with"
trailers, and pushes submodules before the parent so fresh clones don't break.
See `USAGE_gitWrapper.md`. Three verbs: `commit` (commit only), `push`
(push only, submodule-order-aware), `save` (both).

---

## manifest.json

Machine-readable project metadata (name/version/platforms/language/audio
backends/database) — see the file itself. Folder names describe
*architecture* (`core/`, `framework/`, `gui/`); this file is where project
identity/metadata lives, never encoded into a folder name.

---

## Key TODOs (see TODO.md for the full list)

1. Parse FLAC Vorbis comment tags for metadata (title/artist/album/duration)
2. Index `.dsf`/`.dff` in the library scanner — DoP packing and decoder
   dispatch already exist (see "DoP (DSD-over-PCM)" above), the scanner just
   never finds these files today
3. Parallel folder scan (`std::thread` pool, one per CPU core)
4. Essential UI mode's Linux window-sizing (currently always 1200×700)

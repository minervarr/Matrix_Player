# CLAUDE.md

Guidance for Claude Code (claude.ai/code) working in this repository.

## What this project is

Matrix Player — a native C++17 music player that drives a USB DAC **directly**
via a first-party `audio_engine` library (libusbK/libusb, bypassing the OS
audio mixer entirely) and renders its GUI through a first-party Vulkan engine,
`vk_canvas`. **Windows and Linux are equal peers** — both build a real,
running GUI from this same source tree. ALSA and JACK2 are Linux's secondary
output backends (parallel to WASAPI on Windows); USB direct is primary and
bit-perfect everywhere. `android/` holds a **vertical slice** of the same app
(one touch track list over the same `core/`), not a third full platform — see
its own section below.

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
    include/core/facets.h      — GUIDED SEARCH's vocabulary: Chip/Suggestion/EmptyReason.
    src/facets.cpp             — ...and its whole implementation. The search box does not
                                 parse a sentence — the listener builds the query out of
                                 CHIPS accepted from suggestions computed against the REAL
                                 library, so a value that does not exist can never be
                                 typed in. Chips of the SAME kind OR together, chips of
                                 DIFFERENT kinds AND; the listener never picks the
                                 connective (sameGroup() only tells the UI which word to
                                 DRAW). Two rules carry the design: a value present
                                 nowhere is never suggested at all, and a value present
                                 but not ALONGSIDE the chips already placed IS suggested,
                                 disabled, and can name the chip that killed it — "24-bit
                                 (0)" alone cannot tell those two apart, which is the
                                 confusion the split exists to remove. albumYear() is here
                                 because Album carries no year: it is the modal non-zero
                                 Track::year, and 0 means UNKNOWN, never 1970. PURE — no
                                 sqlite, no Canvas, no OS; facets_test links this TU alone,
                                 and Android consumes the same code. Keep it that way
    include/core/streamer_db.h — read-only reader for a FOREIGN database:
    src/streamer_db.cpp          <music root>/.streamer/library.db (or its sibling one
                                 level up), belonging to an external Qobuz-style download
                                 tool. Artist bios/photos and extra album metadata come
                                 from it, keyed by Album::name (== that DB's albums.id ==
                                 the folder name). Never written, never migrated, never
                                 merged into Db's schema, and "no match" is the expected
                                 common case rather than an error — most libraries have no
                                 such folder at all. PlayerWindow keeps one per music root
                                 (streamerDbs_), since only some roots have one
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
    tests/facets_test.cc       — guided search: what is suggested, what is suggested
                                 DISABLED, and which chip an empty result blames.
                                 Links src/facets.cpp and NOTHING else — not even
                                 variants.cpp
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
                                  plus space()/stroke(). Takes the window's SHORT SIDE,
                                  not its height: identical for every window wider than
                                  tall, but a 1080x1920 monitor would otherwise scale
                                  1.78x because the screen is TALL, not big. Tested by
                                  ui_metrics_test.cc
      ui_orientation.hh/.cc     — Horizontal or Vertical, derived from the shape the
                                  window ALREADY has and never asked of the OS, which is
                                  why it works the same on Wayland (a client cannot size
                                  or position itself), on Windows, and on Android (where
                                  rotation arrives as an ordinary resize — the manifest
                                  already declares configChanges=orientation and locks no
                                  screenOrientation). Automatic by default; Alt+L takes a
                                  manual override that STICKS, persisted in settings.
                                  Replaces UiMode{Essential,Complete}, which was a window
                                  rectangle wearing a mode's name. NAMED ui_orientation
                                  because vk_canvas's Android platform already ships an
                                  orientation.hh (physical device orientation, from the
                                  accelerometer). PURE — ui_orientation_test links it alone
      rail_layout.hh/.cc        — where everything in BAR A goes: the seven filter
                                  initials, the search cell, Settings, the AutoEQ box.
                                  Computed once along the bar's LONG AXIS and mapped at
                                  the end, so horizontal is vertical rotated 90° CCW and
                                  there is one layout rather than two. Settings is pinned
                                  at the near end and never moves — opening search must
                                  not cost what was typed. Cells SHRINK uniformly when
                                  nine of them plus the box will not fit, which is the
                                  ordinary vertical bar (its long extent is the window's
                                  WIDTH). PURE: no Canvas, no theme, no metrics —
                                  rail_layout_test asserts every anchor in all states,
                                  including that the two orientations ARE that rotation
                                  of each other, rect by rect
      bar_a.hh/.cc              — bar A's DRAWING and HIT-TESTING, from plain values.
                                  SHARED WITH ANDROID: android/src/android_player_view.cc
                                  fills the same BarAModel and gets the same rail, so the
                                  phone shows the app's real navigation bar rather than a
                                  lookalike. It takes a Canvas and a struct and nothing
                                  else. The rule is checkable on its INCLUDES (the prose mentions
                                  player_view.cc on purpose, so a grep over the whole
                                  file would only ever match itself) and must stay empty:
                                    grep '^#include' gui/src/bar_a.* | grep -E 'player_view|host|core/db|audio_output'
                                  Anything it would need from those is APP STATE and
                                  belongs in the caller, arriving as a value. BarAItem is
                                  deliberately NOT PlayerWindow's integer kSidebar*Hit
                                  vocabulary, whose low end IS AlbumTypeFilter — an enum
                                  frozen by the albums table and meaningless to a layer
                                  that draws letters; PlayerWindow translates at its own
                                  edge (sidebarHitToPick/pickToSidebarHit). drawSearchField
                                  lives here too, because bar A's search and the EQ panel's
                                  must not drift apart. The extraction was verified by
                                  byte-comparing all 20 ui_capture states in BOTH
                                  orientations against the previous build: identical
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
  android/                      — the Android VERTICAL SLICE (see its own section below).
                                  Gradle + NDK, its own CMakeLists.txt reaching UP into
                                  core/ and framework/; src/ holds AndroidHost +
                                  AndroidPlayerView, which is deliberately NOT a port of
                                  PlayerWindow
  packaging/
    arch/                        — PKGBUILD + .desktop + icon (Arch package)
    windows/matrix-player.iss    — Inno Setup installer script (built by
                                  scripts/windows/package.ps1)
  scripts/
    linux/build.sh               — cmake+ninja -> build/linux/ (see flags below)
    windows/build.ps1            — MSYS2/Clang -> cmake+ninja -> build/windows/ or build/windows_debug/
    windows/package.ps1          — Release build + Inno Setup -> the Windows installer
  docs/
    UI_DESIGN_SYSTEM.md          — the GUI's visual language (colors, type, layout rules) —
                                  single source of truth alongside theme.hh
    ref_eq_pipeline.md           — Reference EQ signal chain (biquad → resample → dither/quantize)
    superpowers/specs/*.md       — the design doc PER FEATURE, dated, written before the
                                  code. Read the matching one before reopening a decision:
                                  the release-type/quality colors, the two UI typography
                                  passes, the raster glyph cache, the Android port, the
                                  audio startup notice, fixture-gen, the Windows installer
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
    fixture_gen.cpp              — writes a small SYNTHETIC library (real sine-tone WAVs
                                  in a real folder layout) so every release-type view and
                                  the variant grouping can be exercised without a music
                                  collection. Debug-only, both platforms, built by
                                  core/CMakeLists.txt as matrix_fixture_gen — it links
                                  variants.cpp alone and self-verifies each album it
                                  writes against the REAL classifyReleaseType(), so the
                                  fixtures cannot drift from the classifier they exercise
    ui_capture/                  — headless screenshotter: injects its own Host into the
                                  REAL PlayerWindow (see create()'s injectedHost) and
                                  reaches a named UI state by SYNTHESIZING the clicks on
                                  the rects recalcLayout() itself computed, so captures
                                  follow the layout instead of drifting behind it
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

**Windows**: [MSYS2](https://www.msys2.org/) (UCRT64 environment) — Clang, not MSVC,
invoked directly (`clang.exe`/`clang++.exe` targeting `x86_64-w64-windows-gnu`, **not**
`clang-cl`; no Visual Studio involved at all). From an MSYS2 UCRT64 shell:

```bash
pacman -S mingw-w64-ucrt-x86_64-clang mingw-w64-ucrt-x86_64-cmake \
          mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-libc++ \
          mingw-w64-ucrt-x86_64-llvm mingw-w64-ucrt-x86_64-lld
```

`llvm` (for `llvm-ar`/`llvm-ranlib`) and `lld` are both required, not optional: Release
enables LTO (`-flto=thin` under Clang), and MSYS2's default GNU `ar`/`ranlib`/`ld` can't
archive or link the resulting LLVM-bitcode objects — confirmed by an actual failed
Release build (`CMAKE_CXX_COMPILER_AR-NOTFOUND`, then a `LLVMgold.dll` plugin-load
failure) before both packages were installed. Also needs the [Vulkan
SDK](https://vulkan.lunarg.com/) (`VULKAN_SDK` env var set — the installer does this)
and the **Microsoft Visual C++ Redistributable** (`winget install --id
Microsoft.VCRedist.2015+.x64 -e`) — the latter is a free runtime, not a build tool: the
Vulkan SDK's `slangc.exe` shader compiler is itself an MSVC-built binary and won't run
without it, regardless of what compiles the app itself. libusbK driver bound to the
target USB DAC via Zadig, same as always.

```bat
scripts\windows\build.ps1            :: no mode flag, interactive console -> asks microarch, then build type
scripts\windows\build.ps1 -Release   :: Release -> build\windows\ (skips both prompts)
scripts\windows\build.ps1 -Debug     :: Debug -> build\windows_debug\ (smoke-test tools + matrix_ab_test)
scripts\windows\build.ps1 -Clean     :: wipe the target build dir first
scripts\windows\build.ps1 -Native    :: -march=native, tuned to this exact CPU
scripts\windows\build.ps1 -V3        :: or -V4, x86-64 psABI microarch level
scripts\windows\build.ps1 -Custom znver4   :: any -march value CMake can validate
scripts\windows\build.ps1 -Msys2Root D:\msys64   :: only if MSYS2 isn't at the default C:\msys64
```

Bare `build.ps1` on an interactive console mirrors `scripts/linux/build.sh`'s two-question
prompt (microarch target, then build type) instead of defaulting silently — pass any mode
flag above (or run from a non-interactive caller, like `package.ps1`, which always passes
`-Release`) to skip straight to the build.

Output: `build\windows\gui\matrix_player.exe` (Release) or `build\windows_debug\gui\matrix_player.exe` (Debug),
plus three sibling DLLs (`libc++.dll`, `libgcc_s_seh-1.dll`, `libwinpthread-1.dll`) —
everything else the app itself needs (libgcc, libstdc++, winpthread, its own code)
links in fully static (`-static -static-libgcc -static-libstdc++`, confirmed by an
isolated `std::thread`/`mutex`/`condition_variable` smoke test to actually eliminate
those DLLs — see root `CMakeLists.txt`'s Windows/Clang block). The three DLLs survive
only because a vendored libFLAC CMake rule (`-Wl,-Bstatic ... -Wl,-Bdynamic -lm`,
untouched — see `framework/audio_engine/CLAUDE.md` on keeping libFLAC pristine) resets
the linker's static/dynamic mode without restoring it, so Clang's own auto-appended
C++ runtime resolves dynamically; shipping the three DLLs (`gui/CMakeLists.txt`'s
POST_BUILD step, resolved from wherever `clang++.exe` itself lives — not hardcoded)
was the pragmatic call over more invasive linker-ordering surgery for one DLL chain.
No MSYS2 install is needed on an end-user machine either way — only these three files
next to the exe, `vulkan-1.dll` (GPU-driver-provided), and the OS's own
`api-ms-win-crt-*.dll`/`KERNEL32.dll`.

`core/src/decoder.cpp`'s `openBinary()` uses `_wsopen_s`/`_SH_DENYNO` (needs
`<share.h>`, easy to miss since it's not needed for the sibling `<io.h>`/`<sys/stat.h>`
calls in the same block) and `gui/src/color.hh` `#undef`s `RGB`/`GetRValue`/
`GetGValue`/`GetBValue` before declaring its own portable versions of the same names —
`<windows.h>`'s `wingdi.h` macros of those names, if already in scope from an earlier
include in the same translation unit, collide by text substitution, not just shadow.
Both were latent bugs nobody had hit before this toolchain existed to compile them.

**Both platforms**: submodules first if empty —
`git submodule update --init --recursive`.

**Tests**: there is no ctest/gtest framework, but there are eight assert-based
pure-logic test executables, built **Debug-only** (see the bottom of
`gui/CMakeLists.txt` and of `core/CMakeLists.txt`) and run directly. Convention
matches `framework/vk_canvas/core/tests/*.cc`: plain `assert()`, `#undef NDEBUG`
in the test source so asserts survive an optimized build, no framework or
linkage against the engine.

```bash
scripts/linux/build.sh --debug
./build/linux_debug/gui/ui_metrics_test    # type scale + spacing/stroke math
./build/linux_debug/gui/ui_icons_test      # icon codepoints + placement math
./build/linux_debug/gui/ui_text_test       # ordinal suffixes (the teens: 11th, not 11st)
./build/linux_debug/gui/ui_orientation_test # Horizontal/Vertical from window shape
./build/linux_debug/gui/rail_layout_test   # bar A's anchors, and the 90-degree rotation
./build/linux_debug/core/variants_test     # album-variant grouping, edition terms, trackKey()
./build/linux_debug/core/stats_test        # listening log, aggregate queries, schema migration
./build/linux_debug/core/facets_test       # guided search: suggestions, counts, empty reasons
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
5. **The switcher is a BOX that unfurls, and it no longer has a row budget.**
   `drawEqBox()` shows two things — a discreet `×` meaning *no profile* and the
   active profile's name — and touching the name unfurls the saved list WHOLE,
   from the box to the far end of bar A. `kEqHpMaxRows`, `listCapacity` and
   `drawHeadphoneBlock()`'s "drop rows from the LIST rather than hide the block"
   are all gone: they existed only because the old sidebar had three rows'
   worth of space below Settings. Order is still pinned → most-used →
   most-recent (`loadEqHeadphones`).
6. **The unfurled list HIDES the filter letters; it does not float over them.**
   Not a style choice — the renderer emits every rect before every glyph, so a
   panel drawn last still comes out UNDER text drawn earlier, and the letters
   showed straight through the first version. It is handled in the GEOMETRY
   (`RailInput::eqListOpen` → empty letter rects) so they also stop
   hit-testing underneath it. Same trap the chip strip documents on `rcChips_`.

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

## Guided search (`core/src/facets.cpp`, the sidebar's search box)

The box does **not** parse a sentence. The listener types, the app offers
values that exist in *this* library, and an accepted suggestion becomes a
**chip**:

```
[ Björk ]  AND  [ 1990–1999 ]  AND  [ 24-bit ]
```

Chips of the same **group** OR together, chips of different groups AND. The
listener never picks the connective — `matches()` applies it and the UI only
*draws* the word `sameGroup()` implies. Year and Decade are deliberately one
group: both answer "when".

What is load-bearing:

1. **Two kinds of nothing, and they must not be confused.** A value present
   nowhere in the library is never suggested at all; a value present but not
   *alongside the chips already placed* IS suggested, **disabled**, and can
   name the chip that killed it. `"24-bit (0)"` alone cannot tell those two
   apart — which is exactly the confusion the split exists to remove. Both
   rules are asserted in `core/tests/facets_test.cc`.
2. **A suggestion is counted against the chips NOT in its own group**
   (`withoutGroup()`). Counting it against its own siblings would make picking
   one option grey out all the alternatives to it, which is backwards.
3. **`explainEmpty()` blames the CONSTRAINT, not the wish.** The last chip is
   the subject of the sentence (it is what the listener just asked for); the
   culprit is whichever *earlier* chip, removed, brings results back. So the
   screen reads "No 24-bit in 1990s — your 24-bit releases are from 2002
   onward", never "24-bit matched nothing". When no single removal helps, the
   combination itself is impossible and it says so.
4. **The suggestions come from the WHOLE library, never from the section on
   screen.** Asking for "1990s" while Singles is open is a question about the
   library; filtering the menu by the open tab would hide most of the answer
   and report counts that mean nothing.
5. **A chip is indivisible.** Backspace on an empty box takes the last chip
   back *whole* — deleting it letter by letter would pass through half-values
   that were never valid queries. Tab and Enter both accept, because the box
   behaves like a completion field.
6. **`albumYear()` is derived and 0 means UNKNOWN.** `Album` carries no year;
   it is the modal non-zero `Track::year`. Nothing reads ID3, so an MP3-only
   album genuinely has none — it must render as "Unknown" and never as 1970.
7. **The grid filter matches ANY member of a variant group**
   (`rebuildGridIndices()`), for the same reason search always has: grouping
   must not HIDE a result whose track title only exists on the variant sitting
   behind the tile.
8. **`facets.cpp` links nothing.** Not even `variants.cpp`. It reads
   `Album`/`Track` as plain data, which is what lets the Android build consume
   the search unchanged.
9. **`normalize()` is the ONE definition of "matches by name"** — the
   suggestions and `rebuildGridIndices()`'s live filter both go through it.
   They did not always: the grid used to compare raw bytes, so typing `bjork`
   offered Björk as a chip while simultaneously filtering the grid to nothing.
   The needle is normalized once per rebuild, never per album.

### Why `suggest()` is shaped the way it is

It runs on the UI thread on **every keystroke**, so its cost is a UI
property, not an implementation detail. It was quadratic (measured: 344 ms per
keystroke at 2000 albums, 1443 ms at 4000, from re-deriving each album's
year/quality/genre once per *candidate*). Three things keep it linear, and all
three are easy to undo by accident:

- **`Facts` is derived once per album.** The public `matches()` is a one-album
  adapter over the same internal matcher the fast path uses — deliberately, so
  there is no second implementation that could drift from it and make the grid
  and the counts beside it disagree.
- **Counting is bucketed, one library pass per chip GROUP.** For every kind
  except Name a chip test is an equality on one derived value, so an album
  contributes to exactly one candidate of that group — two in the Year/Decade
  group, which is why those two kinds share a group *and* share a pass. Going
  back to "one full library scan per candidate" is what made it quadratic.
- **Name candidates are CAPPED (`kMaxNameCands`), and that is a real
  narrowing.** A fuzzy match cannot be bucketed, and one common letter typed
  into a large library matches most artists and most titles. The cap keeps the
  best matches by how well they match what was typed (prefix beats middle,
  shorter label beats longer, ties alphabetically) — and the survivors are the
  only rows built, because a row whose count was never computed would render as
  "exists but blocked", the one thing a disabled row is supposed to mean.

`explainEmpty()` walks the library once per chip, and the GUI's
`searchEmptyReason()` calls it again per blocked suggestion row — up to nine
scans. It used to run **every frame** the grid was empty; the sentence is now
cached behind `markSearchEmptyDirty()`, the same pattern as the EQ panel's
header lines. `refreshSuggestions()` is the funnel that invalidates it;
`onScanDone()` marks it separately, since a rescan changes the answer with
nothing typed.

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
    virtual bool init(PlayerWindow* owner) = 0;
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
silent gap): global hotkeys (Alt+F/J/C/U/G/H edge-snap) are system-wide
`RegisterHotKey` calls on Windows but focused-window-only checks on Linux (no
cross-compositor equivalent); `adaptToCurrentMonitor()`/`snapToEdge()` are
no-ops on Linux (Wayland clients cannot query "which monitor" or reposition
themselves).

**Alt+L is NOT in that list any more**, and that is the point of the
orientation work: it used to ask the compositor for a window size and wait for
an asynchronous `configure`, which is why it felt slow on KDE. It now flips a
layout the app draws itself, and needs nothing from the OS.

### The frame: two symmetric bars (`ui_orientation.hh`, `rail_layout.hh`)

The GUI is **two bars of one thickness facing each other, with the content
centred between them**. Bar A is navigation, bar B is the transport. That
thickness is `space(130)` — the transport bar's own height, the value this app
has always drawn its now-playing strip at.

| | bar A (navigation) | bar B (transport) |
|---|---|---|
| Vertical | top | bottom |
| Horizontal | left | right |

**Horizontal is Vertical rotated 90° counter-clockwise, and that is enforced,
not intended.** `computeRailLayout()` places everything along the bar's long
axis and maps to window rects in exactly one function; `rail_layout_test`
asserts the two orientations are that rotation of each other rect by rect, so
an edit that special-cases one of them fails the test. Bar B's drawing does the
same thing at draw time — every line of its text is authored once for a *wide*
bar and emitted under a −90° rotation when the layout is horizontal
(`authored()`/`unauthored()` in `drawFrame()`).

Five things here are load-bearing:

1. **Orientation is derived, never requested.** It comes from the window's own
   width and height. Nothing asks the OS for a size, which is why it works
   identically on Wayland, Windows and Android — and why the old `Alt+L`
   sluggishness on KDE is gone: that path used to request a fullscreen and wait
   for an asynchronous `configure`.
2. **The old sidebar was `space(277)`, 2.1× the bar.** Nothing that lived in it
   survives at 130 with text, which is why bar A is a rail of *initials*
   (A E S C L R P) rather than a narrower list of words. `MATRIX PLAYER` has no
   home in it and is gone.
3. **Three cells read `S`** — Singles, Search, Settings — and they are separated
   by `theme.hh`'s existing text ladder: PRIMARY 242, SECONDARY 170, DIM 128.
   That ladder is FULL: 128 is already the WCAG floor, so a fourth `S` has
   nowhere to go. Position is what teaches them; colour only confirms.
4. **The letters are NOT rotated in the horizontal layout**, though everything
   else in the frame is. A single capital reads the same upright either way.
   Rotation is for text whose LENGTH runs along the bar — the AutoEQ profile
   name, the now-playing title. `Canvas` honours `setRotation()` for text but
   **not** for `image()`, so nothing in either bar may depend on a rotated
   bitmap; the transport artwork is square and needs none. The three transport
   buttons are drawn unrotated on purpose: a Prev triangle turned on its side
   points up, which is a different instruction.
5. **Overlays inside a bar must HIDE what they cover, not float over it.** The
   renderer emits every rect before every glyph, so a panel drawn last still
   comes out under text drawn earlier. Both the open search field and the
   unfurled AutoEQ list are handled in the geometry (`RailInput::searchOpen` /
   `eqListOpen` return empty rects for what they cover), which also stops the
   covered cells hit-testing underneath. Same trap `rcChips_` documents.

The **simple variant** of each orientation (thumbnail + play button) is
designed but deliberately not built — see
`docs/superpowers/specs/2026-08-12-orientation-modes-design.md`.

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
| Platforms | Windows + Linux, equal peers (+ an Android vertical slice in `android/`) | Both build and run the real GUI from this tree; no platform is "the" project. Android runs a smaller touch UI over the same `core/` — see its section above |
| Decoding | `audio_engine`'s own backends: libFLAC, libmpg123, DFF/DSF | First-party and already written for Android — one implementation decodes identically on every platform. Chosen by magic bytes, never by extension. Not FFmpeg |
| WAV | vendored dr_wav (single-header) | The one format the engine has no decoder for |
| DB | SQLite (embedded) | Single file, same model as the Android sibling player |
| USB driver | libusb/libusbK | Best isochronous support cross-platform; libusbK (Zadig) only needed on Windows |
| Audio stack | Bypassed entirely for the primary path | No WASAPI/PulseAudio mixer — raw USB isochronous to DAC |
| Linux secondary outputs | ALSA + JACK2 (never pipewire-jack) | Mirrors WASAPI's role: a fallback when no DAC is plugged in, or for testing without hardware |
| Album art (fullscreen) | Separate window (`ArtWindow`, both platforms) | Dual-monitor: art on one screen, controls on other |
| Submodules | `audio_engine`, `vk_canvas`, `soxr`, `libjpeg-turbo` | dr_flac + sqlite3 vendored directly (single-header / amalgamation, no submodule needed) |
| Build | CMake + Ninja | Clang (MSYS2 UCRT64, targeting `x86_64-w64-windows-gnu`) on Windows, GCC/Clang on Linux — no MSVC, no Visual Studio, no `.sln`/Makefiles |
| `core/` | Zero OS headers (one PIMPL'd exception: FolderWatcher) | Portable app logic reusable without dragging in either platform's headers |

---

## Android (`android/`) — a vertical slice, in this tree

There is a real Android target **in this repository** (Gradle + NDK,
`android/CMakeLists.txt` reaching up into `core/` and `framework/`; build it
with `android/gradlew`). Read
`docs/superpowers/specs/2026-08-08-android-native-port-design.md` before
touching it.

It is a **vertical slice, not a third peer platform**: scan a folder, draw a
flat touch-scrollable track list, tap to play through `ae::AAudioSink`.
Deliberately out of scope for now — sidebar, grid, album view, EQ, settings
panels, the gapless coordinator, `Db`-backed persistence and the listening
log. So the header of this file still holds: Windows and Linux are the two
platforms that build the *whole* app.

Two boundaries here are the point of the design, and re-crossing them is how
this turns into a mess:

1. **`AndroidPlayerView` is NOT a port of `PlayerWindow`,** and
   `AndroidHost` is NOT an implementation of `Host`. `Host::init()` is
   hard-typed to the concrete `PlayerWindow`, with no seam another owner could
   implement; and a touch, phone-sized UI is genuinely different from a
   sidebar-and-grid desktop one. They are structural siblings that share
   *primitives*, not code: the same `Canvas`, the same `theme.hh` and
   `ui_metrics.hh`, unmodified — plus, since the orientation work,
   `ui_orientation.cc` and `rail_layout.cc`, which are pure enough to cross
   with no shim, **and `bar_a.cc`, which is the drawing itself**. Bar A now
   crosses whole: `AndroidPlayerView::recalcLayout()` fills a `BarAModel` and
   calls the same `drawBarA()`/`barAHitTest()` the desktop calls. That was done
   by lifting the two methods out of `PlayerWindow` behind a plain-data view
   model — NOT by widening `Host`, which is the boundary this rule protects.
   **Bar B has not crossed**: the transport still lives in `player_view.cc`,
   so the phone draws bar A over the flat track list and no transport bar.
   Closing that gap is the same move again (`bar_b.hh/.cc` over a `BarBModel`),
   and it is the one piece of the frame still owned by one platform.
2. **What it reuses, it reuses UNCHANGED** — `matrix_core` (`scanLibrary`,
   `Decoder`, and `facets` when guided search reaches the phone) and
   `vk_canvas_core`, both added by `add_subdirectory` straight from this tree.
   That is exactly why the purity rules above (`core/`'s zero OS headers,
   `facets.cpp`/`variants.cpp` linking nothing) are worth keeping: they are
   what makes one `add_subdirectory` enough. Under the NDK toolchain
   `PLATFORM_ID` is `Android`, so `FolderWatcher` already falls through to the
   inotify backend — no new platform split was needed.

`android/CMakeLists.txt` defines `sqlite3` itself and compiles the shader set
from the **desktop** root list into `app/src/main/assets/shaders/` (vk_canvas's
own Android demo list omits MSDF, and this app draws text) — both documented in
place, and neither is a fork.

### Building it, and four things that were wrong until 2026-08-13

```bash
cd android && VULKAN_SDK=/opt/shader-slang sh gradlew assembleDebug --no-daemon
# SUCCESSFUL is not enough — check the APK actually carries its assets:
unzip -l app/build/outputs/apk/debug/app-arm64-v8a-debug.apk | grep -c "assets/fonts/"   # 69
```

`gradlew` has no execute bit in the repo, hence `sh gradlew`.

**This target had never built.** All four causes were silent in different ways:

1. **`ae_aaudio` did not exist.** `android/CMakeLists.txt` linked it and no
   CMake in either repo defined it — `git log -S ae_aaudio` over
   `framework/audio_engine/CMakeLists.txt` returns *no commit at all*. The
   sources were committed; the build rule never was, because audio_engine's own
   `platform/android/` Gradle project compiles them into one library for its
   own app and exports nothing. Fixed in the submodule, beside `ae_alsa`/
   `ae_jack`. **Sink only** — `aaudio_source.cpp` is capture, and
   `backends/mediacodec/` stays unbuilt on purpose: this engine decodes FLAC
   and MP3 with its own vendored libraries everywhere, because some phones ship
   no FLAC decoder and a per-device decode path would make "bit-perfect
   everywhere" depend on the handset.
2. **`compileSdkVersion` named a platform that is not installed**, and the SDK
   is root-owned, so Gradle's attempt to install it failed with *"The SDK
   directory is not writable"*. Now 36, and `targetSdkVersion` is 36 too:
   Android 16 stops honouring orientation and resizability restrictions on
   large screens, which is what `ui_orientation.hh` already assumes.
3. **`ui_min_text_size.gen.h` was looked for only under `build/windows/`**, so
   an Android build on a Linux-only machine failed telling the developer to run
   a desktop build they had already run. It now searches all four desktop build
   trees; the file is identical in each.
4. **No typeface was wired at all.** `AndroidHost` passed `font=nullptr` to
   `Canvas`, so every string fell through to the engine's built-in stroke font.
   Text appeared, which is exactly why nobody noticed. `AndroidHost::initFonts()`
   is now the desktop path from `PlayerWindow::create()` with one substitution —
   an `AndroidAssetReader` where the desktop reads files, because the faces live
   inside the APK. Paths come from `gui/src/ui_fonts.hh`, so the two platforms
   cannot drift onto different faces.

**It is a `RasterFont`, not MTSDF, on BOTH platforms.** `Canvas::useMsdf()`,
`Renderer::initMsdf()` and the `.msdf.cache` filename all keep the name from
when it was `MsdfFont`; the class bakes per-size coverage instead
(`raster_font.hh:18`). Baking is on the CPU on both, too — the desktop's GPU
baker is opt-in behind `MATRIX_GPU_GLYPHS` and was *measured slower*
(`player_view.cc:362`).

**Still not verified: none of this has run on a device.** It compiles, links,
and packages its assets. Nothing more is claimed.

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
4. The SIMPLE variant of each orientation (thumbnail + play button) — designed
   but deliberately not built yet, see the orientation spec

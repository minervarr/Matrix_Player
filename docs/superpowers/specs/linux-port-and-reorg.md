# Linux port + repo reorg

**Status**: Phases 1–8 complete, Phase 9 (final verification pass) pending.
**Plan**: `~/.claude/plans/i-don-t-understand-wht-hashed-possum.md`
**Branch**: `worktree-linux-port-reorg` (isolated worktree).

## Why

The user pulled the latest `audio_engine` and `vk_canvas` submodules; both had
undergone their own internal restructures since Matrix_Player last integrated
them, breaking the CMake build (`add_subdirectory` pointed at directories that
no longer existed). Fixing that was combined with two other decisions made
explicitly by the user in the same pass, rather than staged separately:

1. Reorganize the whole repo to the `core/`/`framework/`/`gui/`/`third_party/`/
   `assets/`/`scripts/<platform>/` convention already used by this author's
   other repos (`audio_engine`, `scanersito`).
2. Bring up a real, working Linux/Wayland build — this development machine is
   Linux, and both engines already had genuine (if under-documented) Linux
   support: `audio_engine`'s ALSA/JACK backends were hardware-verified in its
   own Phase 1; `vk_canvas`'s `platform/linux/` had a real ~900-line Wayland
   implementation despite its own docs still calling it "planned."

A later decision, made explicitly when Task 3 (host abstraction) turned out
larger than planned: replace **all four** native Win32 dialogs (Manage
Folders, Audio Settings, EQ Settings, the `SHBrowseForFolderW` folder picker)
with vk_canvas-native panels on **both** platforms — not just stub them on
Linux — because leaving native OS dialogs as the one non-custom-rendered part
of an otherwise fully custom-rendered app contradicts the whole point of using
vk_canvas. User's own words: "squeeze the most of every platform... not
something like electron."

## What changed, phase by phase

**Phase 1 — CMake dependency graph.** Fixed `add_subdirectory` paths for the
restructured `audio_engine` (now `ae_core`/`ae_usb`/`ae_alsa`/`ae_jack`
targets instead of the old `audio_engine_windows.lib`), fixed `eq_manager.h`'s
`#include` path, corrected a `vk_canvas` submodule pin that had drifted to the
wrong commit.

**Phase 2 — Folder reorg.** Full `git mv` to `core/`/`framework/`/`gui/`/
`third_party/`/`assets/`/`scripts/<platform>/`; root `CMakeLists.txt` split
into root + `core/CMakeLists.txt` + `gui/CMakeLists.txt`; new `manifest.json`.
Found and fixed non-portable code the plan didn't know existed: unconditional
Win32 wide-char calls in `library.cpp`/`decoder.cpp`, MSVC's `_stricmp` in
`eq_profiles.cpp` — all discovered only by actually attempting a Linux build,
not by grep.

**Phase 3 — Host abstraction.** The single riskiest phase: `player_view.cc`
(3600+ lines) was pervasively Win32-typed — not just the message pump, but
`RECT`/`COLORREF`/`HWND`/`HMONITOR` throughout layout and drawing code. Split
into three treatments: convenience POD types got portable replacements
(`LayoutRect`, `ColorRef` — zero call-site changes beyond the type), real OS
handles moved behind a new `Host` interface (`gui/src/host.hh`), and
message-shaped dispatch logic was re-routed (not rewritten) so the same
`on*()` methods that `windows_host.cc`'s `wndProc` called now get called by
`linux_host.cc`'s Wayland event callbacks instead.

**Phase 4 — Portable entry point.** `gui_main.cc` replaces the old
`main.cpp`; Windows bootstrap (DPI awareness, minidump crash handler,
`timeBeginPeriod`, COM init) moved into `windows_host.cc`'s own `WinMain`;
Linux got an equivalent `main()` in `linux_host.cc` (log redirection,
`SIGSEGV`/`SIGABRT` handler — no minidump equivalent, a documented gap).

**Phase 5 — Real inotify FolderWatcher.** Not a stub: recursive tree watch
(inotify has no native subtree flag, so new subdirectories are watched live
via `IN_CREATE`+`IN_ISDIR`), `eventfd`+`poll()` for clean shutdown, the same
~500ms coalescing debounce as the Windows `ReadDirectoryChangesW` backend.
Smoke-tested standalone (outside the CMake tree): watched a scratch directory,
externally created files and a new subdirectory, confirmed the callback fired
correctly including for files created inside the newly-added subdirectory.

**Phase 6 — chrono/thread portability.** `audio_output.h`/`log_util.h` off
`GetTickCount`/`SwitchToThread`/`GetLocalTime` onto
`std::chrono`/`std::this_thread`/`localtime_r`.

**Phase 7 — vk_canvas-native settings panels.** All four native dialogs
replaced with real panels (`gui/src/panels/settings_panels.hh/.cc` for shared
row-list/button/header widgets, per-panel logic in `player_view.cc`). The
folder picker is a from-scratch subfolder browser (directory listing + ".."
navigation + "select this folder"), removing `shlobj.h`/`SHBrowseForFolderW`
from the Windows build too. Linux's Audio Settings panel gained two new
secondary output backends — `AlsaOutput`/`JackOutput` (`gui/src/os/`), thin
`AudioOutput` adapters over `audio_engine`'s `AlsaSink`/`JackSink` — mirroring
WASAPI's role on Windows. The `useWasapi_` bool became an `AudioBackend` enum
(`Usb`/`Wasapi`/`Alsa`/`Jack`) so `onPlay()`'s rate-negotiation and
bit-depth-quantize logic branches correctly for all four backends instead of
a Windows-only boolean silently mis-branching for the two new Linux ones.

**Phase 8 — Documentation.** This file; `CLAUDE.md` rewritten against the
real final tree (it had drifted badly — claiming a `windows_matrix_player/
src/` layout that never existed on disk, and "GDI+ image loading" when the
GUI had already moved to vk_canvas before this session even started).

## Verification stance

Everything Linux-side in this plan was verified by actually building and
running on this development machine — not just compiling. That includes:
CMake configure + full build at every staged checkpoint; **running the built
GUI** against this machine's real Wayland session (window opens, resizes,
renders text/art, scans a library, handles USB-DAC-not-found gracefully, shuts
down cleanly); the inotify watcher (external `touch`/`mkdir` from another
shell); `coredumpctl`+`gdb` used twice to diagnose and fix real crashes
(`LinuxHost::primaryMonitor()` called before its `WaylandDisplay` existed; a
shader-asset path mismatch after the executable moved into `gui/`'s own build
subdirectory).

**Not verified, and not claimed as done:**

- **Anything Windows/MSVC-specific.** No Windows machine is available from
  this session. `windows_host.cc`, the WASAPI branch of Audio Settings, and
  all four panels' Windows code paths are reviewed for platform-symmetry with
  the Linux implementation, not built or run.
- **Settings-panel interaction** (clicking rows, typing in the EQ search box,
  switching backends). No input-injection tool (`ydotool`/`wtype`) is
  installed on this machine, so this is compile-verified and code-reviewed,
  not interactively click-tested.
- **Essential UI mode's Linux sizing** — always opens at a 1200×700 default
  regardless of monitor; a real per-monitor sizing pass is unimplemented.
- **`ArtWindow`** (fullscreen album art, a second window) is Windows-only.
  Wayland has no per-monitor window-targeting API, so porting it needs its
  own design pass rather than a mechanical port.
- **Real playback through the new ALSA/JACK backends** — code-reviewed
  against `AlsaSink`/`JackSink`'s documented contracts, but not run against a
  live `jackd` or physical ALSA hardware output from this session (the app's
  own automatic startup only opens the primary USB path; exercising the
  secondary backends requires the not-yet-click-tested Audio Settings panel).

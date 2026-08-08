# Windows installer

Date: 2026-08-08

## Problem

There is no Windows installer. Today "installing" means copying
`build\matrix_player.exe` plus three DLLs, `assets\`, `fonts\`, and
`eq_profiles.json` out of a raw build tree by hand. Linux already has a real
package (`packaging/arch/`, `pacman -U`, upgrade-in-place, clean uninstall).
Windows needs the same experience: download one file, run it, it installs or
upgrades cleanly, and the result carries no development artifacts.

## Design

### Installer tool: Inno Setup

Free, script-based (`.iss`), no Visual Studio/MSVC dependency — consistent
with the project's existing MSYS2/Clang-only Windows toolchain. Produces a
single `matrix-player-setup-X.Y.Z.exe`. Natively supports detecting a prior
install (via a fixed `AppId` GUID, generated once and hardcoded permanently
into the script) and upgrading its files in place — no separate "uninstall
first" step, mirroring `pacman -U`.

### Install location: per-user, no admin

Installs to `{localappdata}\Matrix Player`, not `Program Files`. This needs no
UAC elevation (install and every future upgrade) and matches how the app
already works today: `app_paths.hh` writes `matrix_player.db`, the log, and
the atlas cache next to the executable by default. A `Program Files` install
would make that directory read-only to a normal user and require wiring up
`MATRIX_STATE_HOME` to split writable state elsewhere — unnecessary
complexity this design avoids by installing somewhere already writable.

### The shipped file set is an explicit allowlist, never a directory copy

`gui/CMakeLists.txt`'s POST_BUILD steps already define the complete runtime
set for a Release build: `matrix_player.exe`, `libc++.dll`,
`libgcc_s_seh-1.dll`, `libwinpthread-1.dll`, `assets\` (compiled shaders),
`fonts\`, `eq_profiles.json`. Nothing else is ever required to run the app —
Debug-only test executables and `matrix_ab_test` aren't built in Release at
all.

The packaging script stages exactly those seven paths into a scratch
directory and points Inno Setup at the scratch directory, never at
`build\` directly. This is the guard against shipping a stray
`matrix_player.db` / `matrix_player.log` / glyph atlas cache that a dev build
tree accumulates from actually being run — none of those are build outputs,
so a raw copy of `build\` would silently leak whoever built the installer's
own local listening history and library scan into the package.

### Versioning

Read from `manifest.json`'s `"version"` field (single source of truth,
already exists) and passed into both the Inno Setup script (`AppVersion`,
output filename) and the packaging script — no version number duplicated by
hand anywhere.

### Build script: `scripts/windows/package.ps1`

Same pattern as the existing `scripts/windows/build.ps1`:
1. Run a Release build (reuses `build.ps1`'s logic, or calls it).
2. Stage the allowlisted files into a clean temp directory.
3. Read the version from `manifest.json`.
4. Invoke Inno Setup's `ISCC.exe` against `packaging/windows/matrix-player.iss`
   with the version passed in.
5. Output `dist/windows/matrix-player-setup-X.Y.Z.exe` — mirrors
   `dist/linux/`'s existing convention for `--share` tarballs.

### Inno Setup script: `packaging/windows/matrix-player.iss`

- Fixed `AppId` GUID (generated once, never changed — this is what makes
  upgrade detection work release over release).
- Start Menu shortcut always created; Desktop shortcut offered as an
  installer checkbox (checked by default, standard for a consumer app).
- Real uninstaller entry in "Add or Remove Programs".
- **Uninstall removes only the program files** (the exe, DLLs, assets, fonts,
  `eq_profiles.json`) — it does **not** delete `matrix_player.db`, the log, or
  the atlas cache. Those are the listener's own library index, listening
  history, and EQ assignments; silently destroying them on every uninstall
  (e.g. before reinstalling a newer version by hand, or troubleshooting)
  would be real data loss for what is, on this machine, the only copy. A
  listener who wants a fully clean wipe can delete the install folder
  themselves after uninstalling.
- No code signing for this iteration — the installer will trigger Windows
  SmartScreen's "unrecognized app" warning on first run for other users, same
  as any unsigned freeware installer (accept-and-continue, does not block
  installation). Can be added later without reworking anything here.

### Update flow

No in-app update checker. Exactly the `pacman -U` model the user asked to
match: a new release produces a new `matrix-player-setup-X.Y.Z.exe` (same
`AppId`), the user downloads and runs it, Inno Setup detects the existing
install and upgrades the program files in place. Library, settings, and
listening history are untouched because they live in files the installer
never lists.

## Out of scope

- Code signing / SmartScreen reputation.
- An in-app "check for updates" feature or GitHub Releases API polling.
- CI/GitHub Actions automation for building or publishing the installer —
  this iteration is a local script, same as `build.ps1` and the Arch
  `PKGBUILD` both are today.
- `Program Files` / per-machine (all-users) installation.

## Testing

Manual, per project convention:
1. Fresh machine (or a clean `{localappdata}\Matrix Player` — delete it
   first): run the installer, confirm Start Menu shortcut, confirm the app
   launches and plays audio.
2. Bump the version in `manifest.json`, rebuild the installer, run it again
   over the existing install: confirm it upgrades without prompting to
   uninstall first, and that the pre-existing `matrix_player.db` (library +
   listening history) survives untouched.
3. Uninstall via "Add or Remove Programs": confirm the program files are gone
   and `matrix_player.db` / the log / the atlas cache remain.
4. Inspect the staged files before compressing: confirm no `.db`, `.log`, or
   atlas-cache file ever reaches the installer, even when built from a
   `build\` tree that has been run locally.

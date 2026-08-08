# Windows packaging

Builds the installer produced by `scripts\windows\package.ps1`. Mirrors
`packaging/arch/`'s role for Linux.

## One-time setup

Install Inno Setup 6 (free): `winget install --id JRSoftware.InnoSetup -e`,
or download from https://jrsoftware.org/isdl.php.

## Build

```bat
scripts\windows\package.ps1
```

Runs a Release build, stages the exact runtime file set into a scratch
directory, and compiles `matrix-player.iss` into
`dist\windows\matrix-player-setup-<version>.exe`. The version comes from
`manifest.json` — bump it there before packaging a new release.

## AppId

`matrix-player.iss`'s `AppId` is a fixed GUID, generated once, and must never
change — it's what makes running a newer installer over an older install
upgrade in place instead of installing side-by-side. Everything else in the
`[Setup]` section can change freely between releases.

## What gets installed, and what doesn't

Only `matrix_player.exe`, the three MinGW runtime DLLs, `assets\`, `fonts\`,
and `eq_profiles.json` — exactly what `gui/CMakeLists.txt`'s POST_BUILD steps
produce next to the executable. `matrix_player.db`, the log, and the glyph
atlas cache are runtime-generated, never listed in `matrix-player.iss`'s
`[Files]` section, and therefore never touched by install OR uninstall — a
listener's library and listening history survive both an upgrade and a
removal of the program itself.

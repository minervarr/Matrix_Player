# Windows Installer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Produce a real Windows installer (`matrix-player-setup-X.Y.Z.exe`) that installs Matrix Player per-user with no admin prompt, upgrades a prior install in place when a new version is run, and never ships a scrap of development junk (no `.db`, `.log`, or glyph atlas cache).

**Architecture:** Inno Setup compiles a script (`packaging/windows/matrix-player.iss`) that installs a fixed, explicit list of files into `{localappdata}\Matrix Player`. A PowerShell script (`scripts/windows/package.ps1`) drives the whole pipeline: run the existing Release build, copy exactly the runtime files into a scratch staging directory (never the raw `build\` tree), then invoke Inno Setup's compiler (`ISCC.exe`) against the `.iss` script with the version read from `manifest.json`.

**Tech Stack:** Inno Setup 6 (`ISCC.exe`, free, no MSVC dependency), PowerShell 5.1 (matches `scripts/windows/build.ps1`'s existing style), the project's existing Release build (`scripts/windows/build.ps1`).

## Global Constraints

- Install location: `{localappdata}\Matrix Player` (per-user, no admin/UAC). Inno Setup directive: `PrivilegesRequired=lowest`.
- `AppId` is fixed forever at `{649F7290-7C25-48AF-94EF-D55EE9FE5C09}` (already generated for this project — never regenerate it; this GUID is what lets a later version detect and upgrade an earlier one in place instead of installing side-by-side).
- Version comes from `manifest.json`'s `"version"` field — the only place it's written by hand. Both the `.iss` script and `package.ps1` read it from there; never hardcode a version number anywhere else.
- Shipped file set is an explicit allowlist, copied into a scratch staging directory — **never** a raw copy of `build\`: `matrix_player.exe`, `libc++.dll`, `libgcc_s_seh-1.dll`, `libwinpthread-1.dll`, `eq_profiles.json`, `assets\` (recursive), `fonts\` (recursive). Nothing else, ever.
- The installer must never list (and therefore never delete on uninstall) `matrix_player.db`, `matrix_player.log`, or the glyph atlas cache files — those are the listener's own library index, listening history, and EQ assignments, not program files.
- Out of scope for this plan: code signing, an in-app update checker, CI/GitHub Actions automation, all-users/`Program Files` installs. Do not add any of these.
- Output convention: `dist\windows\matrix-player-setup-<version>.exe` (mirrors the existing `dist/linux/` convention for `--share` tarballs). `dist/` is already gitignored at the repo root.

---

### Task 1: Inno Setup script (`packaging/windows/matrix-player.iss`)

**Files:**
- Create: `packaging/windows/matrix-player.iss`
- Create: `packaging/windows/README.md`

**Interfaces:**
- Consumes: nothing from other tasks (this task is tested standalone with a hand-made dummy staging directory).
- Produces: an Inno Setup script that accepts three preprocessor defines from the command line — `MyAppVersion` (e.g. `0.1.0`), `MyStageDir` (absolute path to a directory containing the 7 allowlisted paths), `MyOutDir` (absolute path to write the compiled installer into). Task 2's `package.ps1` passes all three via `/D` flags. If not passed, each falls back to a placeholder default so the script alone still compiles for a syntax check.

- [ ] **Step 1: Install Inno Setup 6 if it isn't already on this machine**

Check first:
```powershell
Test-Path "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe"
Test-Path "${env:ProgramFiles}\Inno Setup 6\ISCC.exe"
```
If both are `False`, install it: `winget install --id JRSoftware.InnoSetup -e` (or download from https://jrsoftware.org/isdl.php). Confirm afterward that one of the two paths above now exists.

- [ ] **Step 2: Write `packaging/windows/matrix-player.iss`**

```iss
; Matrix Player — Windows installer (Inno Setup 6).
;
; AppId is fixed permanently. Do not regenerate it: it is the only thing
; that lets Inno Setup recognize "this is the same app, a newer version" and
; upgrade an existing install's files in place, instead of installing a
; second copy side-by-side. See docs/superpowers/specs/2026-08-08-windows-installer-design.md.
;
; MyAppVersion/MyStageDir/MyOutDir are passed in via /D from
; scripts/windows/package.ps1. The fallbacks below only exist so this file
; can be syntax-checked standalone (see the README in this directory).
#define MyAppName "Matrix Player"
#define MyAppExeName "matrix_player.exe"
#ifndef MyAppVersion
  #define MyAppVersion "0.0.0"
#endif
#ifndef MyStageDir
  #define MyStageDir "stage"
#endif
#ifndef MyOutDir
  #define MyOutDir "out"
#endif

[Setup]
AppId={{649F7290-7C25-48AF-94EF-D55EE9FE5C09}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher=nava
DefaultDirName={localappdata}\Matrix Player
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#MyOutDir}
OutputBaseFilename=matrix-player-setup-{#MyAppVersion}
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\{#MyAppExeName}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional shortcuts:"

; Every path here is what Inno Setup tracks and removes on uninstall.
; matrix_player.db / matrix_player.log / the glyph atlas cache are
; deliberately never listed — they are the listener's library index,
; listening history, and EQ assignments, generated at runtime, not program
; files. Not listing them here is what keeps them untouched by uninstall.
[Files]
Source: "{#MyStageDir}\matrix_player.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#MyStageDir}\libc++.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#MyStageDir}\libgcc_s_seh-1.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#MyStageDir}\libwinpthread-1.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#MyStageDir}\eq_profiles.json"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#MyStageDir}\assets\*"; DestDir: "{app}\assets"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#MyStageDir}\fonts\*"; DestDir: "{app}\fonts"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "Launch {#MyAppName}"; Flags: nowait postinstall skipifsilent
```

- [ ] **Step 3: Write `packaging/windows/README.md`**

```markdown
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
```

- [ ] **Step 4: Syntax-check the script standalone with a dummy staging directory**

```powershell
$dummy = "packaging\windows\_dummy_stage"
Remove-Item -Recurse -Force $dummy -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path "$dummy\assets", "$dummy\fonts" | Out-Null
"placeholder" | Out-File "$dummy\matrix_player.exe"
"placeholder" | Out-File "$dummy\libc++.dll"
"placeholder" | Out-File "$dummy\libgcc_s_seh-1.dll"
"placeholder" | Out-File "$dummy\libwinpthread-1.dll"
"placeholder" | Out-File "$dummy\eq_profiles.json"
"placeholder" | Out-File "$dummy\assets\dummy.txt"
"placeholder" | Out-File "$dummy\fonts\dummy.txt"

$iscc = "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe"
if (-not (Test-Path $iscc)) { $iscc = "${env:ProgramFiles}\Inno Setup 6\ISCC.exe" }
& $iscc "/DMyAppVersion=0.0.0-test" "/DMyStageDir=$((Resolve-Path $dummy).Path)" "/DMyOutDir=$((Resolve-Path 'packaging\windows').Path)" "packaging\windows\matrix-player.iss"
```

Expected: `ISCC.exe` exits 0 and prints `Successful compile` (or equivalent
final summary line), producing
`packaging\windows\matrix-player-setup-0.0.0-test.exe`.

- [ ] **Step 5: Clean up the dummy artifacts**

```powershell
Remove-Item -Recurse -Force "packaging\windows\_dummy_stage"
Remove-Item -Force "packaging\windows\matrix-player-setup-0.0.0-test.exe"
```

- [ ] **Step 6: Commit**

```bash
git add packaging/windows/matrix-player.iss packaging/windows/README.md
git_wrapper commit "Add the Inno Setup script for the Windows installer"
```

---

### Task 2: Packaging driver script (`scripts/windows/package.ps1`)

**Files:**
- Create: `scripts/windows/package.ps1`

**Interfaces:**
- Consumes: `scripts/windows/build.ps1` (called with no args → Release build into `build\`, per that script's existing behavior); `manifest.json`'s `"version"` field; `packaging/windows/matrix-player.iss` from Task 1, invoked with `/DMyAppVersion=<version> /DMyStageDir=<abs path> /DMyOutDir=<abs path>` exactly as Task 1 defined those three defines.
- Produces: `dist\windows\matrix-player-setup-<version>.exe`. Also produces (and leaves behind, gitignored) `dist\windows\stage\` — the exact 7-path allowlisted staging tree Task 3 will inspect.

- [ ] **Step 1: Write `scripts/windows/package.ps1`**

```powershell
# Windows installer packaging -> dist\windows\matrix-player-setup-<version>.exe
# Thin driver, same scripts/<platform>/ convention as build.ps1: runs the
# Release build, stages the exact runtime file set (never a raw copy of
# build\ — see the comment below), then hands the stage off to Inno Setup.
#
# Usage: scripts\windows\package.ps1 [-SkipBuild] [-IsccPath <path>]
param(
    [switch]$SkipBuild,
    [string]$IsccPath
)

$ErrorActionPreference = "Stop"
Set-Location (Join-Path $PSScriptRoot "..\..")

if (-not $SkipBuild) {
    Write-Host "Building Release..."
    & (Join-Path $PSScriptRoot "build.ps1")
    if ($LASTEXITCODE -ne 0) { throw "Release build failed." }
}

$BuildDir = "build"
if (-not (Test-Path (Join-Path $BuildDir "matrix_player.exe"))) {
    throw "$BuildDir\matrix_player.exe not found. Run without -SkipBuild, or run scripts\windows\build.ps1 first."
}

$Manifest = Get-Content "manifest.json" -Raw | ConvertFrom-Json
$Version = $Manifest.version
Write-Host "Packaging Matrix Player v$Version..."

# Explicit allowlist copy into a scratch directory — deliberately never a
# raw copy of build\. A build tree that has actually been run locally (for
# dev testing) accumulates matrix_player.db, matrix_player.log, and a glyph
# atlas cache next to the exe; none of those are build outputs, and a plain
# directory copy would silently ship whoever packaged the installer's own
# local listening history and library scan inside it.
$StageDir = "dist\windows\stage"
if (Test-Path $StageDir) { Remove-Item -Recurse -Force $StageDir }
New-Item -ItemType Directory -Force -Path $StageDir | Out-Null

$FilesToStage = @(
    "matrix_player.exe",
    "libc++.dll",
    "libgcc_s_seh-1.dll",
    "libwinpthread-1.dll",
    "eq_profiles.json"
)
foreach ($f in $FilesToStage) {
    $src = Join-Path $BuildDir $f
    if (-not (Test-Path $src)) { throw "Expected build output missing: $src" }
    Copy-Item $src (Join-Path $StageDir $f)
}
foreach ($d in @("assets", "fonts")) {
    $src = Join-Path $BuildDir $d
    if (-not (Test-Path $src)) { throw "Expected build output missing: $src" }
    Copy-Item $src (Join-Path $StageDir $d) -Recurse
}
Write-Host "Staged $($FilesToStage.Count) files + assets\ + fonts\ into $StageDir"

if (-not $IsccPath) {
    $candidates = @(
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "${env:ProgramFiles}\Inno Setup 6\ISCC.exe"
    )
    $IsccPath = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $IsccPath) {
        $cmd = Get-Command iscc.exe -ErrorAction SilentlyContinue
        if ($cmd) { $IsccPath = $cmd.Source }
    }
}
if (-not $IsccPath -or -not (Test-Path $IsccPath)) {
    throw "Inno Setup's ISCC.exe not found. Install it (winget install --id JRSoftware.InnoSetup -e, or https://jrsoftware.org/isdl.php), or pass -IsccPath explicitly."
}

$OutDir = "dist\windows"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$StageDirAbs = (Resolve-Path $StageDir).Path
$OutDirAbs = (Resolve-Path $OutDir).Path

Write-Host "Compiling installer with Inno Setup..."
& $IsccPath "/DMyAppVersion=$Version" "/DMyStageDir=$StageDirAbs" "/DMyOutDir=$OutDirAbs" "packaging\windows\matrix-player.iss"
if ($LASTEXITCODE -ne 0) { throw "Inno Setup compilation failed." }

Write-Host ""
Write-Host "Success! Output: $OutDir\matrix-player-setup-$Version.exe"
```

- [ ] **Step 2: Run it end to end**

```powershell
scripts\windows\package.ps1
```

Expected: exits 0, and prints `Success! Output: dist\windows\matrix-player-setup-0.1.0.exe` (version will match whatever `manifest.json` currently has).

- [ ] **Step 3: Verify the staged file set is exactly the allowlist — nothing extra**

```powershell
Get-ChildItem -Recurse "dist\windows\stage" -File | Select-Object -ExpandProperty Name | Sort-Object -Unique
```

Expected: only files under `assets\`, `fonts\`, plus `matrix_player.exe`,
`libc++.dll`, `libgcc_s_seh-1.dll`, `libwinpthread-1.dll`,
`eq_profiles.json` at the top level. In particular, confirm there is **no**
`matrix_player.db`, `matrix_player.log`, or any file whose name contains
`atlas` or `cache` anywhere in the listing — if `build\` had been run
locally before packaging, those would be the leak this step catches.

- [ ] **Step 4: Verify the installer file exists and is a reasonable size**

```powershell
Get-Item "dist\windows\matrix-player-setup-$((Get-Content manifest.json -Raw | ConvertFrom-Json).version).exe" | Select-Object Name, Length
```

Expected: the file exists. (No fixed size threshold — the bundled fonts
dominate the size regardless of packaging correctness, per the existing Arch
`PKGBUILD` comment about the same 124 MB font set.)

- [ ] **Step 5: Commit**

```bash
git add scripts/windows/package.ps1
git_wrapper commit "Add the Windows installer packaging script"
```

---

### Task 3: End-to-end install / upgrade / uninstall verification

**Files:**
- None created or modified — this task only runs the artifacts from Tasks 1–2 and checks their behavior. If it uncovers a bug, fix it in `packaging/windows/matrix-player.iss` or `scripts/windows/package.ps1` (from Tasks 1–2) and re-run this task from Step 1.

**Interfaces:**
- Consumes: `dist\windows\matrix-player-setup-<version>.exe` from Task 2. Inno Setup's generated uninstaller, always named `unins000.exe`, written by the installer into `{app}` (i.e. `%LOCALAPPDATA%\Matrix Player\unins000.exe`) — this is an Inno Setup convention, not something either script defines.
- Produces: nothing consumed by a later task — this is the final verification.

- [ ] **Step 1: Clean any prior install on this machine**

```powershell
$AppDir = "$env:LOCALAPPDATA\Matrix Player"
if (Test-Path "$AppDir\unins000.exe") {
    & "$AppDir\unins000.exe" /VERYSILENT /SUPPRESSMSGBOXES
    Start-Sleep -Seconds 2
}
Remove-Item -Recurse -Force $AppDir -ErrorAction SilentlyContinue
```

- [ ] **Step 2: Silent-install and verify the program files + shortcut**

```powershell
$Version = (Get-Content manifest.json -Raw | ConvertFrom-Json).version
& "dist\windows\matrix-player-setup-$Version.exe" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART
Start-Sleep -Seconds 3

Test-Path "$env:LOCALAPPDATA\Matrix Player\matrix_player.exe"
Get-ChildItem "$env:APPDATA\Microsoft\Windows\Start Menu\Programs" -Filter "Matrix Player*"
```

Expected: both `Test-Path` returns `True`, and the `Get-ChildItem` shows a
`Matrix Player.lnk` shortcut.

- [ ] **Step 3: Plant a fake library/settings file to stand in for real user data**

```powershell
"fake listening history" | Out-File "$env:LOCALAPPDATA\Matrix Player\matrix_player.db"
```

- [ ] **Step 4: Re-run the same installer over the existing install (upgrade-in-place)**

```powershell
& "dist\windows\matrix-player-setup-$Version.exe" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART
Start-Sleep -Seconds 3
Test-Path "$env:LOCALAPPDATA\Matrix Player\matrix_player.exe"
Test-Path "$env:LOCALAPPDATA\Matrix Player\matrix_player.db"
Get-Content "$env:LOCALAPPDATA\Matrix Player\matrix_player.db"
```

Expected: it does **not** prompt to uninstall first (silent flags make this
moot, but note for a future manual/interactive run: it should not), the exe
is still present, and `matrix_player.db` still exists with its original
content `fake listening history` — proof the upgrade never touched it,
because `matrix-player.iss`'s `[Files]` section never lists it.

- [ ] **Step 5: Uninstall and verify program files are gone but the fake data survives**

```powershell
& "$env:LOCALAPPDATA\Matrix Player\unins000.exe" /VERYSILENT /SUPPRESSMSGBOXES
Start-Sleep -Seconds 3

Test-Path "$env:LOCALAPPDATA\Matrix Player\matrix_player.exe"
Test-Path "$env:LOCALAPPDATA\Matrix Player\assets"
Test-Path "$env:LOCALAPPDATA\Matrix Player\matrix_player.db"
```

Expected: the first two are `False` (program files and the `assets\`
directory Inno Setup created are removed), the third is `True` (the fake
library file survives uninstall untouched).

- [ ] **Step 6: Clean up the fake data and do one real launch**

```powershell
Remove-Item -Recurse -Force "$env:LOCALAPPDATA\Matrix Player" -ErrorAction SilentlyContinue
& "dist\windows\matrix-player-setup-$Version.exe" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART
Start-Sleep -Seconds 2
Start-Process "$env:LOCALAPPDATA\Matrix Player\matrix_player.exe"
```

Manually confirm the window opens and looks normal (per `CLAUDE.md`: GUI
changes are validated by actually running the app, not by an assertion).
Close it, then leave the install in place or remove it — your call, this was
the last verification step.

- [ ] **Step 7: Commit the plan's completion**

Nothing to commit from this task (no files were created or modified) — skip
`git_wrapper` here.

---

## Self-review notes

- Spec coverage: Inno Setup choice (Task 1), per-user no-admin install (Task 1 `[Setup]`), fixed AppId / upgrade-in-place (Task 1 + verified in Task 3 Step 4), explicit allowlist staging (Task 2), version from `manifest.json` (Task 2), uninstall preserves user data (Task 1 `[Files]` design + verified in Task 3 Step 5), output naming convention (Task 2) — all covered. Code signing / in-app updater / CI are explicitly out of scope per the spec and this plan adds none of them.
- No placeholders: every step has literal, runnable code or an exact expected result to check against.
- Type/name consistency checked: `MyAppVersion`/`MyStageDir`/`MyOutDir` defines in Task 1's `.iss` match the `/D` flags Task 2's `package.ps1` passes, verbatim. The staged file list in Task 2 matches the `[Files]` section in Task 1 exactly (5 top-level files + `assets\` + `fonts\`).

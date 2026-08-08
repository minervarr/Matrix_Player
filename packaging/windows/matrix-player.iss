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

#pragma once
#include <string>

// Where the app's files live — the ONE place that answers it.
//
// Two directories, and the split between them is the whole point:
//
//   exeDir()   — READ-ONLY data shipped alongside the binary: fonts/,
//                assets/shaders/, eq_profiles.json. Nothing is ever written
//                here, which is what lets a package install it root-owned.
//   stateDir() — everything the app WRITES: matrix_player.db, the log, and
//                the ~45 MB MTSDF atlas cache (which pruneStaleCaches() also
//                deletes from).
//
// They are the same directory by default, so a build tree and the tarball in
// dist/linux/ stay exactly what they have always been: one self-contained
// folder you can move anywhere. Defining MATRIX_STATE_HOME at build time
// (see gui/CMakeLists.txt) moves the writable half to $HOME/<that name>/,
// which is what a system package needs — /opt/matrix_player is root-owned and
// the atlas cache is per-user regardless, since bakeFallbackGlyphs() bakes
// whatever CJK/Hangul/Kana the listener's own library happens to contain.
//
// No XDG variable is read, deliberately. This is a plain dotdir in the
// ~/.ssh / ~/.gnupg tradition and depends on no specification.
//
// Host::exeDir() forwards here rather than the reverse: openLogFile() needs a
// path before any Host exists, and this used to be four separate copies of the
// same /proc/self/exe (or GetModuleFileNameW) block — in both host backends,
// in openLogFile(), and in tools/ui_capture. That is precisely the drift
// ui_fonts.hh's comment warns about, one level down.
namespace app_paths {

// Directory holding the executable, UTF-8, WITH a trailing separator.
// Falls back to "./" if the OS refuses to say.
const std::string& exeDir();

// Directory for files the app writes, UTF-8, WITH a trailing separator.
// Created on first call. Equal to exeDir() unless MATRIX_STATE_HOME was
// defined at build time; falls back to exeDir() if the home directory is
// unknown or the directory cannot be created and written to, so a stripped
// environment degrades to the old behaviour instead of failing to start.
const std::string& stateDir();

}  // namespace app_paths

#pragma once
#include <string>

// Android's backend for the app_paths::exeDir()/stateDir() seam declared,
// unmodified, in gui/src/app_paths.hh. Desktop's app_paths.cc computes both
// by probing the OS (GetModuleFileNameW / readlink("/proc/self/exe") / $HOME)
// — none of that is meaningful on Android. Here both are set once, at
// startup, from values the platform already hands the app for free.
namespace app_paths {

// Must be called exactly once, before the first exeDir()/stateDir() call —
// from AndroidHost's window-init path (android_host.cc), using
// android_app->activity->internalDataPath for writableDir (a plain field,
// no JNI needed). See app_paths_android.cc for why assetsDirSentinel is
// currently unused.
void setAndroidPaths(std::string assetsDirSentinel, std::string writableDir);

}  // namespace app_paths

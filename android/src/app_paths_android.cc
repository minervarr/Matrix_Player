#include "app_paths_android.hh"

#include "../../gui/src/app_paths.hh"  // unmodified — same declared signatures

namespace app_paths {

namespace {
std::string g_exeDir;
std::string g_stateDir;
}  // namespace

void setAndroidPaths(std::string assetsDirSentinel, std::string writableDir) {
    g_exeDir = std::move(assetsDirSentinel);
    g_stateDir = std::move(writableDir);
    if (!g_stateDir.empty() && g_stateDir.back() != '/') {
        g_stateDir += '/';
    }
}

// Deliberately unused by AndroidPlayerView: the only exeDir()-relative read
// on desktop is a plain file open (eq_profiles.json — see player_view.cc's
// only call site), but this slice's font/shader loading goes through vk_canvas's
// AndroidAssetReader (AAssetManager_open on a path string, not a filesystem
// directory — see android_platform.cc). Kept wired to whatever
// setAndroidPaths() was given (currently always "") only so the header's
// two-function contract needs no #ifdefs at call sites.
const std::string& exeDir() {
    return g_exeDir;
}

const std::string& stateDir() {
    return g_stateDir;
}

}  // namespace app_paths

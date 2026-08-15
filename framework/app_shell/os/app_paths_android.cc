#include "app_paths_android.hh"

#include "app_paths.hh"  // unmodified — same declared signatures

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

// EMPTY, and that emptiness is load-bearing rather than a gap.
//
// PlayerWindow::create() builds its font paths as exeDir() + "fonts/…" and
// reads them through Host::dataReader(), which on Android is the APK's
// AAssetManager. An empty exeDir() therefore makes that expression produce
// exactly the asset name AAssetManager wants — the same string the desktop
// turns into an absolute filesystem path, resolved by a different reader.
// That is the whole reason player_view.cc needs no #ifdef to find its
// typeface. Setting this to anything non-empty would break the font load.
//
// eq_profiles.json is the one other exeDir()-relative read, and it is a plain
// file open (std::ifstream) rather than a reader call, so it simply finds
// nothing here: the phone has no AutoEQ profile catalogue yet. It fails the
// way an absent file fails, not the way a bug does.
const std::string& exeDir() {
    return g_exeDir;
}

const std::string& stateDir() {
    return g_stateDir;
}

}  // namespace app_paths

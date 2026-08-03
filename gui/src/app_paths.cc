#include "app_paths.hh"

#include <cstdlib>
#include <filesystem>
#include <system_error>

#ifdef _WIN32
#  include <windows.h>
#  include <io.h>       // _access
#else
#  include <climits>
#  include <unistd.h>
#endif

namespace app_paths {
namespace {

std::string discoverExeDir() {
#ifdef _WIN32
    wchar_t exePathW[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exePathW, MAX_PATH) == 0) return "./";
    std::wstring dirW = exePathW;
    const auto slash = dirW.rfind(L'\\');
    if (slash == std::wstring::npos) return "./";
    dirW = dirW.substr(0, slash + 1);
    int len = WideCharToMultiByte(CP_UTF8, 0, dirW.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "./";
    std::string dir(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, dirW.c_str(), -1, dir.data(), len, nullptr, nullptr);
    if (!dir.empty() && dir.back() == '\0') dir.pop_back();
    return dir;
#else
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return "./";
    buf[n] = '\0';
    std::string path(buf);
    const auto slash = path.rfind('/');
    return slash == std::string::npos ? "./" : path.substr(0, slash + 1);
#endif
}

#ifdef MATRIX_STATE_HOME
// The user's home directory, or empty if the environment does not say.
std::string homeDir() {
#ifdef _WIN32
    if (const char* p = std::getenv("USERPROFILE")) return p;
#endif
    if (const char* p = std::getenv("HOME")) return p;
    return {};
}

// Writable in the sense that matters: the directory exists AND this process
// can create files in it. create_directories() succeeding proves neither on
// its own — an existing root-owned directory reports success and then refuses
// every write, which is the exact failure this whole split exists to avoid.
bool usableForWriting(const std::string& dir) {
#ifdef _WIN32
    return _access(dir.c_str(), 6) == 0;   // 6 = read|write
#else
    return ::access(dir.c_str(), R_OK | W_OK | X_OK) == 0;
#endif
}

std::string discoverStateDir(const std::string& fallback) {
    const std::string home = homeDir();
    if (home.empty()) return fallback;

    std::string dir = home;
    if (dir.back() != '/' && dir.back() != '\\') dir += '/';
    dir += MATRIX_STATE_HOME;
    dir += '/';

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);   // already-exists is not an error
    if (!usableForWriting(dir)) return fallback;
    return dir;
}
#endif  // MATRIX_STATE_HOME

}  // namespace

const std::string& exeDir() {
    // Resolved once: /proc/self/exe cannot change under a running process, and
    // stateDir() below may be called before main() has done anything else.
    static const std::string dir = discoverExeDir();
    return dir;
}

const std::string& stateDir() {
#ifdef MATRIX_STATE_HOME
    static const std::string dir = discoverStateDir(exeDir());
#else
    static const std::string dir = exeDir();
#endif
    return dir;
}

}  // namespace app_paths

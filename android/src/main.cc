#include <android_native_app_glue.h>
#include <android/log.h>

#include <exception>
#include <memory>

#include <filesystem>

#include "android_host.hh"  // app_shell: the third Host
#include "storage_permission.hh"  // app_shell: has_all_files_access()
#include "arc/fs/paths.hh"        // archive_engine: the storage layout
#include "arc/fs/volume.hh"
#include "player_view.hh"
#include "os/aoas_output.hh"  // AOAS relay backend: hands over the android_app for JNI
#include "os/media_session_android.hh"  // foreground service + MediaSession: same
#include "os/bt_codec_android.hh"       // A2DP codec control: same

#define LOG_TAG "MatrixMain"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// The Android entry point, and the sibling of gui/src/gui_main.cc: construct
// the app, hand it a Host, run it. Everything below create() is the same
// player_view.cc the desktop runs — there is no Android build of the UI.
//
// There USED to be one (AndroidPlayerView, a flat touch-scrollable track list)
// because the port assumed a phone needed its own smaller app. It did not: the
// screen is a rectangle like any other, PlayerWindow touches no OS header, and
// the only thing genuinely missing was this Host. See android_host.hh.
//
// ── Why this function is so loud ─────────────────────────────────────────────
//
// When android_main() RETURNS, the activity is finished — so every failure
// here, however it happens, looks identical from the outside: the app opens
// and closes. There is no window left to put an error in and no console to
// print one to. The phase lines below are the only way to tell "create()
// refused" from "something threw" from "it ran and quit", and they cost one
// logcat line each at startup. Filter with:
//
//     adb logcat -s MatrixMain:V AndroidHost:V matrix_player:V
void android_main(android_app* state) {
    LOGI("phase 1/5: entry -- constructing PlayerWindow");
    // The AOAS relay backend's JNI upcalls need the activity (its classloader
    // and its bindService). Available from the first line of android_main.
    matrixAoasSetApp(state);
    // Same reason, same moment: the foreground service and the MediaSession are
    // reached by JNI upcalls that need the activity's classloader.
    matrixMediaSessionSetApp(state);
    matrixBtCodecSetApp(state);

    // ── The storage layout, stated once before anything asks for a path ──────
    //
    // Two facts only this file can know, because core/ does no JNI:
    //
    //   privateDir  ANativeActivity::internalDataPath — /data/user/0/<pkg>/files.
    //               There is no way to derive it; it carries the package name
    //               and the user id. This is where the DATABASE belongs: real
    //               ext4, no FUSE in front of it, no permission attached.
    //
    //   all-files   whether shared storage is reachable at all yet. On the very
    //               first launch it is not — AndroidHost asks for the grant and
    //               defers onHostReady() until it arrives — so anything below
    //               that depends on it is simply skipped this time round.
    //
    // The external root is left at arc_fs's own default (/storage/emulated/0):
    // Environment.getExternalStorageDirectory() would be the exact answer, but
    // reaching it needs a Java activity class, and this app runs on the stock
    // NativeActivity with no subclass of its own.
    {
        arc::fs::PlatformDirs dirs;
        if (state->activity->internalDataPath)
            dirs.privateDir = state->activity->internalDataPath;
        arc::fs::setPlatformDirs(dirs);
        arc::fs::setAllFilesAccess(has_all_files_access(state));
    }
    const arc::fs::Layout paths = arc::fs::layout("matrix_player");

    // Create home/ once shared storage is actually reachable. Creating a NEW
    // top-level directory there needs the all-files grant, so on a first launch
    // this does nothing and the fallback below picks the old location; from the
    // second launch on, home/ exists.
    if (arc::fs::hasAllFilesAccess()) arc::fs::ensureLayout(paths);

    // Where to look for music when the launch intent does not say.
    //
    // home/Music if it is there, and the phone's own Music/ otherwise. Nothing
    // is ever MOVED between them: a library already configured keeps working,
    // because PlayerWindow::onHostReady() only consults this when the database
    // holds no root at all. Migrating hundreds of gigabytes is the listener's
    // decision, and Android's shared storage has no symlink to bridge the two.
    std::error_code ec;
    const bool haveHomeMusic = std::filesystem::is_directory(paths.music, ec) && !ec;
    const std::string scanRoot =
        haveHomeMusic ? paths.music : std::string("/storage/emulated/0/Music");
    LOGI("storage: private=%s  home=%s  scan-root=%s%s",
         paths.state.c_str(), paths.home.c_str(), scanRoot.c_str(),
         haveHomeMusic ? "" : "  (home/Music absent -- using the phone's Music/)");

    try {
        PlayerWindow win;

        LOGI("phase 2/5: create() -- host init, Vulkan, DB, fonts");
        // "scan_root" is the intent extra this app is launched with, and the
        // fallback is the one computed above. app_shell knows neither — it
        // reads whatever key it is handed. See android_host.hh.
        if (!win.create(std::make_unique<AndroidHost>(
                state, "scan_root", scanRoot.c_str()))) {
            // create() logs its own reason first (Host::showErrorMessage is a
            // logcat line here); this only says which stage refused.
            LOGE("phase 2/5 FAILED: create() returned false -- the activity "
                 "will now finish. The line above this one is the reason.");
            return;
        }

        LOGI("phase 3/5: create() OK -- entering run()");
        win.run();

        LOGI("phase 4/5: run() returned -- shutting down");
        win.shutdown();
        LOGI("phase 5/5: clean exit");
    } catch (const std::exception& e) {
        // An exception escaping android_main() is otherwise a bare abort with
        // no message attached to it.
        LOGE("FATAL: unhandled exception: %s", e.what());
    } catch (...) {
        LOGE("FATAL: unhandled non-standard exception");
    }
}

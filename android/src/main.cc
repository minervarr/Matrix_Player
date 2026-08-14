#include <android_native_app_glue.h>
#include <android/log.h>

#include <exception>
#include <memory>

#include "android_host.hh"
#include "player_view.hh"

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
    try {
        PlayerWindow win;

        LOGI("phase 2/5: create() -- host init, Vulkan, DB, fonts");
        if (!win.create(std::make_unique<AndroidHost>(state))) {
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

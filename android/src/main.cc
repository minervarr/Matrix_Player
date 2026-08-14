#include <android_native_app_glue.h>

#include <memory>

#include "android_host.hh"
#include "player_view.hh"

// The Android entry point, and the sibling of gui/src/gui_main.cc: construct
// the app, hand it a Host, run it. Everything below create() is the same
// player_view.cc the desktop runs — there is no Android build of the UI.
//
// There USED to be one (AndroidPlayerView, a flat touch-scrollable track list)
// because the port assumed a phone needed its own smaller app. It did not: the
// screen is a rectangle like any other, PlayerWindow touches no OS header, and
// the only thing genuinely missing was this Host. See android_host.hh.
void android_main(android_app* state) {
    PlayerWindow win;
    if (!win.create(std::make_unique<AndroidHost>(state))) return;
    win.run();
    win.shutdown();
}

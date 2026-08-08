#include <android_native_app_glue.h>

#include "android_host.hh"
#include "android_player_view.hh"

void android_main(android_app* state) {
    AndroidHost host(state);
    AndroidPlayerView view;
    host.attach(&view);
    host.run();
}
